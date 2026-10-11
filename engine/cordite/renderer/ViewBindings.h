#pragma once

/// @file
/// What the passes of a scene are told about the view they draw, and about its lights.
///
/// The camera, the size of the target and the lights are the same for every pass of a
/// view, so they live in one binding set that all of those passes bind first (register
/// space 0):
///
/// | Register | Holds                                                             |
/// |----------|-------------------------------------------------------------------|
/// | b0       | `ViewConstants` (shaders/common/View.slang)                       |
/// | t0       | The lights: directional ones first (shaders/common/Lights.slang)  |
/// | t1       | The cluster grid: where each cluster's list is                    |
/// | t2       | The lists: indices into the lights                                |
///
/// @code
/// CORDITE_TRY_ASSIGN(ViewBindings view, ViewBindings::create(device));
///
/// view.update(commandList, {.camera = cameraView, .targetSize = targets.size()}, lightClusters);
/// depthPass.draw(commandList, targets.depthFramebuffer(), batches, view.bindings(), instances);
/// lightingPass.draw(commandList, targets, view.bindings());
/// @endcode
///
/// update() may be called any number of times in a command list: what is recorded after
/// a call sees that call's values, what was recorded before keeps its own. Ask for
/// bindings() after update(), not before: the set is replaced when a buffer has to grow.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"

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

class ViewBindings {
public:
    /// An empty object; assign one made by create() before use.
    ViewBindings() = default;

    [[nodiscard]] static Result<ViewBindings> create(nvrhi::IDevice* device);

    /// Makes `view` and `lights` what passes recorded from here on are told. `lights`
    /// must have been built for the camera of `view`. The command list must be open.
    void update(nvrhi::ICommandList* commandList, const SceneView& view, const LightClusters& lights);

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
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingSetHandle m_bindings;
    /// True when a buffer was replaced and the binding set has to follow.
    bool m_bindingsOutdated = true;
};

} // namespace cordite
