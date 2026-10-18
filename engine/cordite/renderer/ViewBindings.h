#pragma once

/// @file
/// What the passes of a scene are told about the view they draw, its lights and their
/// shadows.
///
/// The camera, the size of the target, the lights and the shadow maps are the same for
/// every pass of a view, so they live in one binding set that all of those passes bind
/// first (register space 0):
///
/// | Register | Holds                                                             |
/// |----------|-------------------------------------------------------------------|
/// | b0       | `ViewConstants` (shaders/common/View.slang)                       |
/// | t0       | The lights: directional ones first (shaders/common/Lights.slang)  |
/// | t1       | The cluster grid: where each cluster's list is                    |
/// | t2       | The lists: indices into the lights                                |
/// | t3       | The shadow views: where each shadow map is in the atlas           |
/// | t4       | The shadow atlas (shaders/common/Shadows.slang)                   |
/// | t5       | The environment's cube map (shaders/common/Environment.slang)     |
/// | t6       | The environment's image, for the sky                              |
/// | t7       | The cookies of the lights, a layer each (Lights.slang)            |
/// | t8       | What lights throw into the fog (shaders/common/VolumetricFog.slang) |
/// | t9       | The environment probes that light the view (Environment.slang)    |
/// | t10      | Their harmonics, nine a probe                                     |
/// | t11      | Their reflections, a cube a probe                                 |
/// | s0       | The sampler that compares a depth with the atlas                  |
/// | s1       | The sampler of the cube map                                       |
/// | s2       | The sampler of the sky: it wraps around from left to right        |
/// | s3       | The sampler of the cookies: it repeats them                       |
/// | s4       | The sampler of the fog's light: it stops at the edges             |
///
/// @code
/// CORDITE_TRY_ASSIGN(ViewBindings view, ViewBindings::create(device));
///
/// view.update(commandList, {.camera = cameraView, .targetSize = targets.size()}, lightClusters,
///             resources.cookies(), {.plan = &shadowPlan, .atlas = shadowAtlas.texture(), .filterRadius = 1},
///             {.environment = &environments.none()});
/// depthPass.draw(commandList, targets.depthFramebuffer(), batches,
///                {.view = view.bindings(), .instances = instances, .joints = palettes.bindings()});
/// lightingPass.draw(commandList, targets, view.bindings());
/// @endcode
///
/// update() may be called any number of times in a command list: what is recorded after
/// a call sees that call's values, what was recorded before keeps its own. Ask for
/// bindings() after update(), not before: the set is replaced when a buffer has to grow
/// or the atlas, the environment, the cookies or the fog's light is another texture.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentMaps.h"
#include "cordite/renderer/EnvironmentProbes.h"
#include "cordite/renderer/Fog.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/ShadowPlan.h"
#include "cordite/renderer/VolumetricFog.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cordite {

/// A view of a scene, as its passes need to know it. All colors are linear.
struct SceneView {
    CameraView camera;
    /// The world to clip space as the view was when it was last drawn, for its motion
    /// vectors (MotionVectors.h): none for a view that goes on from no picture, which then
    /// has nothing moving in it.
    std::optional<Mat4> previousViewProjection;
    /// The size of the target in pixels.
    UVec2 targetSize{1, 1};
    /// Ambient light arriving from above and from below.
    Vec3 skyColor{0.0f};
    Vec3 groundColor{0.0f};
    /// Whether the ambient occlusion of the view's targets is drawn this frame
    /// (AmbientOcclusionPass): the lighting reads it only then.
    bool ambientOcclusion = false;
    /// The fog between the viewer and what it sees; none unless it has a density.
    FogSettings fog;
    /// The grid that what lights throw into the fog is worked out for (VolumetricFogPass):
    /// none where no light lights it.
    FogVolumeGrid fogVolume;
    /// What the targets were cleared to. The lighting has to know it where it draws
    /// fog over pixels in which nothing else is: fog mixes with what is behind it.
    Vec3 backgroundColor{0.0f};
};

/// The shadows of a view, as its passes read them.
struct ViewShadows {
    /// Where the shadow maps are and what they show; the lights of the view refer to
    /// its views by index. Null when nothing casts a shadow.
    const ShadowPlan* plan = nullptr;
    /// The texture the plan's maps were drawn into (ShadowAtlas::texture()). Never
    /// null: a view without shadows still binds an atlas, and reads none of it.
    nvrhi::ITexture* atlas = nullptr;
    /// How far the filter that softens the edges reaches, in texels (ShadowSettings).
    uint32_t filterRadius = 1;
};

/// The environment of a view, as its passes read it.
struct ViewEnvironment {
    /// What lights the view from all around (EnvironmentMaps::environment()), or
    /// EnvironmentMaps::none(). Never null: a view without an environment still binds
    /// one, and gets no light from it.
    const GpuEnvironment* environment = nullptr;
    /// Scales its light: what it lights, and what is seen of it.
    float intensity = 0.0f;
    /// Turns it about the vertical axis, counter-clockwise seen from above, in radians.
    float rotation = 0.0f;
    /// Whether it is drawn as the sky, where nothing else is.
    bool visible = false;
    /// The environment probes that light the view in its place where their boxes are
    /// (EnvironmentProbes::viewList()): the highest priority first, at most
    /// kMaxViewProbes; the buffer of their harmonics (EnvironmentProbes::harmonics()) and
    /// the array of their reflections (EnvironmentProbes::radiance()), either of which
    /// may be null when there are none. Without harmonics the view has no probes.
    std::span<const GpuEnvironmentProbe> probes;
    nvrhi::IBuffer* probeHarmonics = nullptr;
    nvrhi::ITexture* probeRadiance = nullptr;
};

class ViewBindings {
public:
    /// An empty object; assign one made by create() before use.
    ViewBindings() = default;

    [[nodiscard]] static Result<ViewBindings> create(nvrhi::IDevice* device);

    /// Makes `view`, `lights`, `shadows` and `environment` what passes recorded from
    /// here on are told. `lights` must have been built for the camera of `view`, and
    /// `shadows` planned for them; `cookies` is the texture array their cookies are
    /// layers of (RenderResources::cookies()), never null. `fogVolume` is what lights throw
    /// into the fog, worked out for the grid `view.fogVolume` (SceneTargets::fogVolume()),
    /// or null: then the shaders are told that no light lights the fog, and read nothing
    /// of it. The command list must be open.
    void update(nvrhi::ICommandList* commandList, const SceneView& view, const LightClusters& lights,
                nvrhi::ITexture* cookies, const ViewShadows& shadows, const ViewEnvironment& environment,
                nvrhi::ITexture* fogVolume = nullptr);

    /// The layout of bindings(), for pipelines: register space 0, visible to every
    /// shader stage.
    [[nodiscard]] nvrhi::IBindingLayout* layout() const { return m_layout; }

    /// The set as it is after the last update().
    [[nodiscard]] nvrhi::IBindingSet* bindings() const { return m_bindings; }

    /// The buffer of the constants alone (b0), as the last update() wrote them: for a pass
    /// that cannot bind the whole set because it writes something the set holds - the
    /// fog's light (VolumetricFogPass).
    [[nodiscard]] nvrhi::IBuffer* constants() const { return m_constants; }

private:
    /// Makes room in `buffer` for `byteSize` bytes of elements `stride` bytes large,
    /// replacing it when it is too small. Returns false when that fails.
    [[nodiscard]] bool reserve(nvrhi::BufferHandle& buffer, size_t byteSize, uint32_t stride, const char* name);

    /// Writes the probes of `environment` that fit - and, the first time, the cube of
    /// reflections that stands in for none. Returns how many the shaders are to read:
    /// none without the harmonics they are lit by.
    [[nodiscard]] uint32_t writeProbes(nvrhi::ICommandList* commandList, const ViewEnvironment& environment);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::BufferHandle m_constants;
    nvrhi::BufferHandle m_lights;
    nvrhi::BufferHandle m_clusterGrid;
    nvrhi::BufferHandle m_lightIndices;
    nvrhi::BufferHandle m_shadowViews;
    nvrhi::BufferHandle m_probes;
    /// What is bound at t10 when a view has no probes: nine float4 nobody reads.
    nvrhi::BufferHandle m_noProbeHarmonics;
    /// The buffer of harmonics the binding set holds.
    nvrhi::BufferHandle m_probeHarmonics;
    /// What is bound at t11 when a view has no reflections of probes: a cube of a texel.
    nvrhi::TextureHandle m_noProbeRadiance;
    bool m_noProbeRadianceWritten = false;
    /// The array of reflections the binding set holds.
    nvrhi::TextureHandle m_probeRadiance;
    nvrhi::SamplerHandle m_shadowSampler;
    nvrhi::SamplerHandle m_environmentSampler;
    nvrhi::SamplerHandle m_skySampler;
    nvrhi::SamplerHandle m_cookieSampler;
    nvrhi::SamplerHandle m_fogVolumeSampler;
    /// What is bound at t8 when no light lights the fog: a single texel of nothing.
    nvrhi::TextureHandle m_noFogVolume;
    /// Whether m_noFogVolume has been given its texel, which takes a command list.
    bool m_noFogVolumeWritten = false;
    /// The textures the binding set holds; a view may come with others.
    nvrhi::TextureHandle m_atlas;
    nvrhi::TextureHandle m_environmentRadiance;
    nvrhi::TextureHandle m_environmentSky;
    nvrhi::TextureHandle m_cookies;
    nvrhi::TextureHandle m_fogVolume;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingSetHandle m_bindings;
    /// True when a buffer was replaced and the binding set has to follow.
    bool m_bindingsOutdated = true;
};

} // namespace cordite
