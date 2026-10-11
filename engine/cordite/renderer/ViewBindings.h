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
/// | s0       | The sampler that compares a depth with the atlas                  |
/// | s1       | The sampler of the cube map                                       |
/// | s2       | The sampler of the sky: it wraps around from left to right        |
///
/// @code
/// CORDITE_TRY_ASSIGN(ViewBindings view, ViewBindings::create(device));
///
/// view.update(commandList, {.camera = cameraView, .targetSize = targets.size()}, lightClusters,
///             {.plan = &shadowPlan, .atlas = shadowAtlas.texture(), .filterRadius = 1},
///             {.environment = &environments.none()});
/// depthPass.draw(commandList, targets.depthFramebuffer(), batches, view.bindings(), instances);
/// lightingPass.draw(commandList, targets, view.bindings());
/// @endcode
///
/// update() may be called any number of times in a command list: what is recorded after
/// a call sees that call's values, what was recorded before keeps its own. Ask for
/// bindings() after update(), not before: the set is replaced when a buffer has to grow
/// or the atlas or the environment is another texture.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentMaps.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/ShadowPlan.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>

namespace cordite {

/// A view of a scene, as its passes need to know it. All colors are linear.
struct SceneView {
    CameraView camera;
    /// The size of the target in pixels.
    UVec2 targetSize{1, 1};
    /// Ambient light arriving from above and from below.
    Vec3 skyColor{0.0f};
    Vec3 groundColor{0.0f};
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
};

class ViewBindings {
public:
    /// An empty object; assign one made by create() before use.
    ViewBindings() = default;

    [[nodiscard]] static Result<ViewBindings> create(nvrhi::IDevice* device);

    /// Makes `view`, `lights`, `shadows` and `environment` what passes recorded from
    /// here on are told. `lights` must have been built for the camera of `view`, and
    /// `shadows` planned for them. The command list must be open.
    void update(nvrhi::ICommandList* commandList, const SceneView& view, const LightClusters& lights,
                const ViewShadows& shadows, const ViewEnvironment& environment);

    /// The layout of bindings(), for pipelines: register space 0, visible to every
    /// shader stage.
    [[nodiscard]] nvrhi::IBindingLayout* layout() const { return m_layout; }

    /// The set as it is after the last update().
    [[nodiscard]] nvrhi::IBindingSet* bindings() const { return m_bindings; }

private:
    /// Makes room in `buffer` for `byteSize` bytes of elements `stride` bytes large,
    /// replacing it when it is too small. Returns false when that fails.
    [[nodiscard]] bool reserve(nvrhi::BufferHandle& buffer, size_t byteSize, uint32_t stride, const char* name);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::BufferHandle m_constants;
    nvrhi::BufferHandle m_lights;
    nvrhi::BufferHandle m_clusterGrid;
    nvrhi::BufferHandle m_lightIndices;
    nvrhi::BufferHandle m_shadowViews;
    nvrhi::SamplerHandle m_shadowSampler;
    nvrhi::SamplerHandle m_environmentSampler;
    nvrhi::SamplerHandle m_skySampler;
    /// The atlas and the environment the binding set holds; a view may come with
    /// others.
    nvrhi::TextureHandle m_atlas;
    nvrhi::TextureHandle m_environmentRadiance;
    nvrhi::TextureHandle m_environmentSky;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingSetHandle m_bindings;
    /// True when a buffer was replaced and the binding set has to follow.
    bool m_bindingsOutdated = true;
};

} // namespace cordite
