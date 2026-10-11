#pragma once

/// @file
/// Turns linear scene color into the display-ready image.
///
/// @code
/// CORDITE_TRY_ASSIGN(DisplayEncodePass encode, DisplayEncodePass::create(device, shaders));
/// encode.draw(commandList, targets.sceneColor(), targets.displayFramebuffer());
/// @endcode
///
/// The result is sRGB-encoded and limited to the range of a display; brighter values
/// clip. This is where exposure, tone mapping and color grading will happen.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

class DisplayEncodePass {
public:
    [[nodiscard]] static Result<DisplayEncodePass> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Encodes `sceneColor` into `destination`, which must have the same size. The
    /// command list must be open.
    void draw(nvrhi::ICommandList* commandList, nvrhi::ITexture* sceneColor, nvrhi::IFramebuffer* destination);

private:
    DisplayEncodePass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle bindingLayout, FullscreenPass pass);

    nvrhi::IDevice* m_device;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    FullscreenPass m_pass;
};

} // namespace cordite
