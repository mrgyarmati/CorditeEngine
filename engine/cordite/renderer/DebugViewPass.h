#pragma once

/// @file
/// Shows one thing the G-buffer holds instead of the lit picture.
///
/// @code
/// CORDITE_TRY_ASSIGN(DebugViewPass debugView, DebugViewPass::create(device, shaders, view.layout()));
///
/// // After the base pass has filled the G-buffer of `targets`, in place of the lighting:
/// debugView.draw(commandList, targets, view.bindings(), RenderDebugView::Roughness);
/// @endcode
///
/// The value is written where the lit color would go - into the scene color of the
/// targets - so that everything after this pass works as usual. Numbers that are not
/// colors are written so that the display encoding shows them as they are: a roughness
/// of 0.5 comes out as mid-grey. Pixels in which nothing was drawn keep the background.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

class DebugViewPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<DebugViewPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                      nvrhi::IBindingLayout* viewLayout);

    /// Writes what `view` shows into the scene color of `targets`. `view` must be one
    /// for which isDrawnFromGBuffer() is true. The command list must be open.
    void draw(nvrhi::ICommandList* commandList, const SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
              RenderDebugView view);

private:
    DebugViewPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle gbufferLayout, nvrhi::BufferHandle constants,
                  FullscreenPass pass);

    nvrhi::IDevice* m_device;
    nvrhi::BindingLayoutHandle m_gbufferLayout;
    nvrhi::BufferHandle m_constants;
    FullscreenPass m_pass;
};

} // namespace cordite
