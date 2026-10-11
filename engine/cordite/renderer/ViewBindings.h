#pragma once

/// @file
/// What the passes of a scene are told about the view they draw.
///
/// The camera, the size of the target and the lighting are the same for every pass of a
/// view, so they live in one constant buffer that all of those passes bind as their
/// first binding set (register space 0; `ViewConstants` in shaders/common/View.slang).
///
/// @code
/// CORDITE_TRY_ASSIGN(ViewBindings view, ViewBindings::create(device));
///
/// view.update(commandList, {.camera = cameraView, .targetSize = targets.size()});
/// depthPass.draw(commandList, targets.depthFramebuffer(), batches, view.bindings(), instances);
/// lightingPass.draw(commandList, targets, view.bindings());
/// @endcode
///
/// update() may be called any number of times in a command list: what is recorded after
/// a call sees that call's values, what was recorded before keeps its own.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

/// A view of a scene, as its passes need to know it. All colors are linear.
struct SceneView {
    CameraView camera;
    /// The size of the target in pixels.
    UVec2 targetSize{1, 1};

    /// The fixed lighting that stands in until the engine has lights: a key light, given
    /// by the direction from a surface towards it and by its color and strength, and
    /// ambient light from a sky above and a ground below.
    Vec3 keyLightDirection{0.0f, 1.0f, 0.0f};
    Vec3 keyLightColor{1.0f};
    Vec3 skyColor{0.0f};
    Vec3 groundColor{0.0f};
};

class ViewBindings {
public:
    /// An empty object; assign one made by create() before use.
    ViewBindings() = default;

    [[nodiscard]] static Result<ViewBindings> create(nvrhi::IDevice* device);

    /// Makes `view` what passes recorded from here on are told. The command list must
    /// be open.
    void update(nvrhi::ICommandList* commandList, const SceneView& view);

    /// The layout of bindings(), for pipelines: register space 0, the constants at b0,
    /// visible to every shader stage.
    [[nodiscard]] nvrhi::IBindingLayout* layout() const { return m_layout; }

    [[nodiscard]] nvrhi::IBindingSet* bindings() const { return m_bindings; }

private:
    nvrhi::BufferHandle m_constants;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BindingSetHandle m_bindings;
};

} // namespace cordite
