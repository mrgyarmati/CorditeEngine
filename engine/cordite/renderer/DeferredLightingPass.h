#pragma once

/// @file
/// Deferred lighting: lights every pixel of the G-buffer once.
///
/// @code
/// CORDITE_TRY_ASSIGN(DeferredLightingPass lighting, DeferredLightingPass::create(device, shaders, view.layout()));
///
/// // After the base pass has filled the G-buffer of `targets`:
/// lighting.draw(commandList, targets, view.bindings());
/// @endcode
///
/// The pass writes linear light into the scene color of the targets and leaves pixels
/// alone in which nothing was drawn, so the background stays what it was cleared to.
/// The lights are those of the view (see ViewBindings): its directional lights, the
/// point and spot lights of the cluster each pixel is in, and ambient light from a sky
/// and a ground.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

class DeferredLightingPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<DeferredLightingPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             nvrhi::IBindingLayout* viewLayout);

    /// Lights the G-buffer of `targets` into their scene color. With `lightingOnly`,
    /// every surface is lit as if it were plain matte grey: the light without the
    /// materials. The command list must be open.
    void draw(nvrhi::ICommandList* commandList, const SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
              bool lightingOnly = false);

private:
    DeferredLightingPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle gbufferLayout, FullscreenPass pass,
                         FullscreenPass lightingOnlyPass);

    nvrhi::IDevice* m_device;
    nvrhi::BindingLayoutHandle m_gbufferLayout;
    FullscreenPass m_pass;
    FullscreenPass m_lightingOnlyPass;
};

} // namespace cordite
