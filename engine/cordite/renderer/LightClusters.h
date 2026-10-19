#pragma once

/// @file
/// Clustered lighting: which lights reach which part of a view.
///
/// A scene may hold hundreds of lights, but a pixel is lit by the few whose range it is
/// in. To find those few without asking every light about every pixel, the volume a
/// camera sees is cut into a grid of clusters - columns across, rows down, slices into
/// the depth - and each cluster gets the list of the lights that touch it. A pixel
/// shader works out which cluster its point is in and reads that list.
///
/// @code
/// LightClusterBuilder builder;              // kept from frame to frame
/// LightClusters clusters;
/// builder.build(view, sceneLights.lights(), clusters);
///
/// upload(clusters.lights);        // one GpuLight per light: the directional ones first
/// upload(clusters.grid);          // per cluster: where its list starts, how long it is
/// upload(clusters.indices);       // the lists: indices into `lights`
/// @endcode
///
/// Slices are thin near the camera and thick far away (each is a constant factor deeper
/// than the one before), because that is how much of the screen things take up. An
/// orthographic view is sliced evenly instead. The shader side of the arithmetic is in
/// shaders/common/Lights.slang; clusterOf() here is the same function, and a test keeps
/// the two in step.

#include "cordite/core/Uuid.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RectLights.h"

#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// The grid a view is cut into: 16 x 9 x 24 clusters.
inline constexpr uint32_t kClusterColumns = 16;
inline constexpr uint32_t kClusterRows = 9;
inline constexpr uint32_t kClusterSlices = 24;
inline constexpr uint32_t kClusterCount = kClusterColumns * kClusterRows * kClusterSlices;

/// How many lights a view can have. More than these are left out, the ones named last
/// first.
inline constexpr uint32_t kMaxDirectionalLights = 4;
inline constexpr uint32_t kMaxLocalLights = 1024;

/// The clusters do not reach further than this, however far the camera sees: beyond it,
/// everything is in the last slice.
inline constexpr float kMaxClusterDepth = 500.0f;

/// The kinds of light, as the shaders number them.
enum class GpuLightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Rect = 3,
};

/// The smallest a side of a rect light is taken to be, in metres: the light of its face is
/// its color over its area.
inline constexpr float kMinRectLightSize = 0.01f;

/// How a light casts shadows; see LightComponent, which these come from.
struct SceneLightShadow {
    bool enabled = false;
    /// Shadow-map texels by which a surface is looked up nearer the light, and along
    /// its normal.
    float bias = 1.0f;
    float normalOffset = 1.0f;
    /// The size of the shadow map against what a light usually gets: 0.5, 1 or 2.
    float resolutionScale = 1.0f;
    /// Directional: how far the shadows reach from the camera in metres, into how many
    /// cascades that is divided, and how (0: evenly, 1: by a constant factor).
    float distance = 60.0f;
    uint32_t cascades = 4;
    float cascadeDistribution = 0.75f;
};

/// A light as the renderer collects it from the scene. All of it is in world space.
struct SceneLight {
    GpuLightType type = GpuLightType::Point;
    Vec3 position{0.0f};
    /// Unit vector along which a directional, spot or rect light shines.
    Vec3 direction{0.0f, -1.0f, 0.0f};
    /// Linear color times intensity.
    Vec3 color{1.0f};
    /// Point, spot and rect: where the light has faded to nothing, in metres - from the
    /// middle of a rect light, at least as far as its corners (makeSceneLight).
    float range = 10.0f;
    /// Spot: the angles from the axis at which the light starts to fade and at which
    /// it ends, in radians.
    float innerConeAngle = 0.35f;
    float outerConeAngle = 0.5f;
    /// The lighting channels the light belongs to, one bit each.
    uint32_t channels = 1;
    SceneLightShadow shadow;
    /// The light's right in the world - the +X axis of its entity, of length one: which
    /// way round its cookie is, and along which a rect light is wide.
    Vec3 right{1.0f, 0.0f, 0.0f};
    /// Rect: how wide and how high its rectangle is, in metres - along its right and along
    /// the up at right angles to that and to `direction` (rectShapeOf).
    float width = 1.0f;
    float height = 1.0f;
    /// Spot and directional: the texture the light shines through (its cookie); nil for
    /// none.
    Uuid cookie;
    /// Directional: how far its cookie reaches along the light's right and up before it
    /// repeats, in metres.
    float cookieSize = 10.0f;
    /// How brightly the light lights a fog lit by lights (FogSettings::volumetric),
    /// against how brightly it lights a surface: 1 as `color` says, 0 not at all.
    float volumetricIntensity = 1.0f;
};

/// A light as the shaders read it: `Light` in shaders/common/Lights.slang.
struct GpuLight {
    Vec3 position{0.0f};
    float range = 0.0f;
    Vec3 color{0.0f};
    /// The GpuLightType in the low 8 bits, the channels in the 8 above.
    uint32_t typeAndChannels = 0;
    Vec3 direction{0.0f, -1.0f, 0.0f};
    /// The first of the light's shadow views (ShadowPlan::views), or -1 when it casts
    /// no shadows: one view for a spot light, six for a point or a rect light, one for
    /// each cascade of a directional light.
    int32_t shadowView = -1;
    /// A spot light's brightness at an angle whose cosine is c: saturate(c * scale + offset).
    float spotScale = 0.0f;
    float spotOffset = 1.0f;
    /// Shadow-map texels by which a surface is looked up nearer the light, and along
    /// its normal.
    float shadowBias = 0.0f;
    float shadowNormalOffset = 0.0f;
    /// Where the cookie of the light is laid on what it lights: its right, divided by the
    /// tangent of a spot light's outer cone or by the size of a directional light's
    /// cookie, so that how far a point is from `position` along it and along the up made
    /// of it and `direction` is where on the cookie the point is (cookieUvOf,
    /// LightCookies.h). Nothing for a light without a cookie.
    Vec3 cookieRight{0.0f};
    /// The layer of the cookie in the cookie array of the view (RenderResources::
    /// cookies()), or -1 for none.
    int32_t cookie = -1;
    /// What the light throws into a fog lit by lights (VolumetricFog.h), as `color` is
    /// what it shines on surfaces with: `color` times SceneLight::volumetricIntensity.
    Vec3 fogColor{0.0f};
    float padding = 0.0f;
    /// A rect light's rectangle, whose middle is `position`: from there to the middle of its
    /// right edge, at right angles to `direction` - and half its height, along
    /// cross(rectRight, direction) (rectShapeOf). Nothing for the other lights.
    Vec3 rectRight{0.0f};
    float rectHalfHeight = 0.0f;
};

static_assert(sizeof(GpuLight) == 112, "must match Light in shaders/common/Lights.slang");

/// The right of `light` at right angles to the way it shines, of length one: its `right`
/// without what of it runs along `direction` - whatever the transform of its entity has
/// done to its axes - or, where that leaves nothing, the right of a camera that looks the
/// way the light shines, with +Y up.
[[nodiscard]] Vec3 rightAcrossOf(const SceneLight& light);

/// The rectangle of a rect light: its middle at `position`, as wide and as high as it says
/// - kMinRectLightSize at the least, which is also what a size that is no number is taken
/// for - along rightAcrossOf() and along the up at right angles to that and to the way it
/// shines, which it shines towards.
[[nodiscard]] RectLightShape rectShapeOf(const SceneLight& light);

/// The same of a light as the shaders read it - written down twice, here and in
/// rectLightUp() of shaders/common/Lights.slang. Of no size for a light that is no rect.
[[nodiscard]] RectLightShape rectShapeOf(const GpuLight& light);

/// Where the list of a cluster is in LightClusters::indices.
struct ClusterRange {
    uint32_t offset = 0;
    uint32_t count = 0;

    bool operator==(const ClusterRange&) const = default;
};

/// How a point's depth becomes a slice, in a form the shaders share:
/// perspective: slice = log2(depth) * scale + bias; orthographic: slice = depth * scale + bias.
struct ClusterDepthMapping {
    float scale = 1.0f;
    float bias = 0.0f;
    bool orthographic = false;

    bool operator==(const ClusterDepthMapping&) const = default;

    /// The slice a depth (metres in front of the camera) falls in, from 0 to
    /// kClusterSlices - 1; depths in front of the first slice or beyond the last are
    /// counted to those.
    [[nodiscard]] uint32_t sliceOf(float depth) const;

    /// The depth at which slice `slice` begins; `kClusterSlices` gives where the last
    /// one ends.
    [[nodiscard]] float sliceStart(uint32_t slice) const;
};

/// The depth mapping for a view.
[[nodiscard]] ClusterDepthMapping clusterDepthMapping(const CameraView& view);

/// The index of the cluster a point of the target falls in: `uv` from (0, 0) at the top
/// left to (1, 1), `depth` in metres in front of the camera.
[[nodiscard]] uint32_t clusterOf(Vec2 uv, float depth, const ClusterDepthMapping& mapping);

/// The index of a cluster from its column, row and slice.
[[nodiscard]] constexpr uint32_t clusterIndex(uint32_t column, uint32_t row, uint32_t slice) {
    return (slice * kClusterRows + row) * kClusterColumns + column;
}

/// The lights of a view, sorted into clusters.
struct LightClusters {
    /// Directional lights first - they reach everywhere and are in no list - then the
    /// point, spot and rect lights.
    std::vector<GpuLight> lights;
    /// For each of `lights`, which of the lights given to the builder it was made from.
    std::vector<uint32_t> sources;
    uint32_t directionalCount = 0;
    /// One per cluster, indexed by clusterIndex().
    std::vector<ClusterRange> grid;
    /// The lists of all clusters, one after another: indices into `lights`, in
    /// increasing order within a list.
    std::vector<uint32_t> indices;
    ClusterDepthMapping depthMapping;
    /// Lights that were left out because the view has more than it can hold.
    uint32_t droppedLights = 0;

    /// The lights of the cluster with the given index.
    [[nodiscard]] std::span<const uint32_t> lightsOf(uint32_t cluster) const;
};

/// Sorts lights into the clusters of a view.
///
/// Keep one for as long as there are views to draw: it remembers where the clusters of
/// the last projection are, which is a good part of the work and the same from frame
/// to frame for a camera whose lens does not change, and it reuses its memory.
class LightClusterBuilder {
public:
    /// Sorts `lights` into the clusters of `view`. The result is the same for the same
    /// input, whatever was built before: the builder and `clusters` are only reused for
    /// what they hold in memory. Lights without brightness or range are left out, as
    /// are those that reach no cluster.
    void build(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters);

private:
    struct Pair {
        uint32_t cluster;
        uint32_t light;
    };

    /// Makes m_bounds and the others hold the boxes for the projection of `view`.
    void updateBounds(const CameraView& view, const ClusterDepthMapping& mapping);

    /// Adds a pair to m_pairs for every cluster that `reach`, a sphere in view space,
    /// touches.
    void addPairs(const Sphere& reach, uint32_t light, const ClusterDepthMapping& mapping);

    /// Cuts the lists of the clusters from m_pairs.
    void writeLists(LightClusters& clusters) const;

    /// The boxes of the clusters in view space, indexed by clusterIndex(), ...
    std::vector<Aabb> m_bounds;
    /// ... of each row of a slice (slice * kClusterRows + row) and of each of its
    /// columns (slice * kClusterColumns + column): what a light does not touch, none
    /// of the clusters in it has to be asked about.
    std::vector<Aabb> m_rowBounds;
    std::vector<Aabb> m_columnBounds;
    /// What the boxes were worked out for.
    Mat4 m_boundsProjection{0.0f};
    ClusterDepthMapping m_boundsMapping;
    bool m_hasBounds = false;

    /// Working memory of build().
    std::vector<const SceneLight*> m_local;
    std::vector<Pair> m_pairs;
};

/// LightClusterBuilder::build() with a builder made for the one call: for where a view
/// is built once, such as a test or a tool.
void buildLightClusters(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters);

} // namespace cordite
