#pragma once

/// @file
/// Which lights of a view cast shadows, from where, and where their shadow maps go.
///
/// A shadow map is a picture of the scene's depth as a light sees it; a point that is
/// farther from the light than what the map holds in its direction is in shadow. All the
/// shadow maps of a view share one square texture, the shadow atlas, each in a square
/// tile of its own. Planning decides, for the lights of a view that cast shadows, what
/// each of them gets:
///
/// - the sun - the brightest directional light that casts shadows - gets up to four
///   maps, the cascades. Each covers a slice of what the camera sees: a small slice
///   near the camera, larger ones behind it, so that near shadows are sharp and far
///   ones exist;
/// - a spot light gets one map, looking along its cone;
/// - a point light gets six, one for each side of a cube around it, and so does a rect
///   light, around its middle: what it lights is in front of it, so that about half of
///   what its maps show is never looked up, but six alike are what the shaders look a
///   point up in.
///
/// @code
/// ShadowPlan plan;
/// planShadows(view, sceneLights.lights(), clusters, shadowSettingsFor(ShadowQuality::Medium), plan);
///
/// for (const ShadowView& shadow : plan.views) {
///     // Draw what casts shadows as seen through shadow.viewProjection, depth only,
///     // into the tile at shadow.tileOrigin that is shadow.tileSize texels a side.
/// }
/// @endcode
///
/// Planning is arithmetic and runs without a GPU. Depth in a shadow map is 0 at the
/// light's near plane and 1 at its far one - not reversed, unlike the depth buffer of a
/// view: shadow maps are read through a comparison sampler, which wants it this way.

#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cordite {

/// The most cascades the shadow of a directional light can have.
inline constexpr uint32_t kMaxShadowCascades = 4;

/// The most shadow maps a view can have. A point or a rect light takes six of them, a spot
/// light one, the sun one for each cascade.
inline constexpr uint32_t kMaxShadowViews = 64;

/// The smallest shadow map, in texels a side. Every size is a power of two times this.
inline constexpr uint32_t kMinShadowTileSize = 64;

/// How far towards a directional light things are still drawn into its shadow maps,
/// measured from the part of the view a cascade covers, in metres. What stands further
/// off than this casts no shadow into the view.
inline constexpr float kShadowCasterDistance = 250.0f;

/// How much the shadows of a view may cost.
enum class ShadowQuality : uint8_t {
    /// No shadows at all.
    Off,
    Low,
    Medium,
    High,
};

inline constexpr std::array<ShadowQuality, 4> kShadowQualities = {ShadowQuality::Off, ShadowQuality::Low,
                                                                  ShadowQuality::Medium, ShadowQuality::High};

/// The name used in files and by tools: "off", "low", "medium", "high".
std::string_view nameOf(ShadowQuality quality);

/// The name shown to people: "Off", "Low", "Medium", "High".
std::string_view displayNameOf(ShadowQuality quality);

/// The quality with the given nameOf(), or nothing. Upper and lower case are not told
/// apart.
[[nodiscard]] std::optional<ShadowQuality> shadowQualityFromName(std::string_view name);

/// What a shadow quality comes to in numbers.
struct ShadowSettings {
    /// The length of a side of the shadow atlas in texels: a power of two, or 0 for no
    /// shadows. Of a size that is not a power of two, only the largest one that fits
    /// is used. The atlas is 4 bytes a texel.
    uint32_t atlasSize = 4096;
    /// The size of one cascade of the sun's shadow, and of the shadow map of a point or
    /// spot light, in texels a side, for a light of medium shadow resolution.
    uint32_t cascadeSize = 1024;
    uint32_t localSize = 512;
    /// How far the filter that softens the edge of a shadow reaches, in texels: 0 reads
    /// the map once, 1 at 3 x 3 places, 2 at 5 x 5.
    uint32_t filterRadius = 1;

    bool operator==(const ShadowSettings&) const = default;
};

[[nodiscard]] ShadowSettings shadowSettingsFor(ShadowQuality quality);

/// One shadow map: the view from a light into a tile of the atlas.
struct ShadowView {
    /// World space to the clip space of the map: x and y from -1 to 1 with +Y up, depth
    /// from 0 at the light's near plane to 1 at its far one.
    Mat4 viewProjection{1.0f};
    /// The tile of the atlas the map is drawn into, in texels: its left and top edge,
    /// and the length of its side.
    UVec2 tileOrigin{0, 0};
    uint32_t tileSize = 0;
    /// How large a texel of the map is in the world: in metres for a directional light,
    /// whose map is as wide everywhere; in metres one metre in front of the light for
    /// the others, whose texels grow with the distance from it - the distance along
    /// the way the map looks, not the one to the point looked up.
    float texelSize = 0.0f;
    /// The directions in the world in which the map's x and y grow: unit vectors, at
    /// right angles to each other and to the way the light looks.
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    /// True for the maps of point, spot and rect lights.
    bool perspective = false;
    /// The light the map belongs to: an index into LightClusters::lights.
    uint32_t light = 0;
};

/// The cascades of the sun's shadow.
struct ShadowCascades {
    /// How many there are: none when no directional light casts shadows.
    uint32_t count = 0;
    /// The distance from the camera at which each cascade ends, in metres, measured
    /// along the direction the camera looks in. Zero for cascades that are not there.
    std::array<float, kMaxShadowCascades> ends{};
    /// Where the shadows start to fade out, and where they are gone.
    float fadeStart = 0.0f;
    float fadeEnd = 0.0f;
    /// The view of the first cascade in ShadowPlan::views; the others follow it.
    uint32_t firstView = 0;
};

/// The shadow maps of one view.
struct ShadowPlan {
    std::vector<ShadowView> views;
    ShadowCascades cascades;
    /// Lights that are to cast shadows but got no shadow map: the atlas was full, the
    /// view had its kMaxShadowViews, or the light is a directional one that is not the
    /// brightest.
    uint32_t lightsWithoutRoom = 0;
};

/// Plans the shadow maps of `view`. `clusters` must have been built from `lights` for
/// that view: only lights that reach something in view get a shadow map, the sun first
/// and then the point, spot and rect lights nearest to the camera. A light that finds no
/// room for its map at the size it asks for gets a smaller one, down to
/// kMinShadowTileSize, before it goes without. The sun, which comes first, takes three
/// quarters of the atlas at most: if its cascades would take more, the far ones are made
/// smaller. The lights of `clusters` are told which shadow views are theirs
/// (GpuLight::shadowView); `plan` is replaced. Nothing here allocates memory once `plan`
/// has held as many views before.
void planShadows(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters,
                 const ShadowSettings& settings, ShadowPlan& plan);

/// The distances at which the cascades of a shadow end: `count` (1 to 4) values between
/// `nearPlane` and `distance`, the last of them `distance`. A `distribution` of 0 makes
/// the cascades equally deep, 1 makes each deeper than the one before by a constant
/// factor. Entries past `count` are zero.
[[nodiscard]] std::array<float, kMaxShadowCascades> cascadeEnds(float nearPlane, float distance, uint32_t count,
                                                                float distribution);

/// Which of the six shadow maps of a point light a point is looked up in: `offset` is
/// from the light to the point. 0 and 1 are the maps that look along +X and -X, 2 and 3
/// along +Y and -Y, 4 and 5 along +Z and -Z; the one along the largest part of the
/// offset is it. The shaders do the same (shaders/common/Shadows.slang).
[[nodiscard]] uint32_t pointShadowFace(const Vec3& offset);

} // namespace cordite
