#pragma once

/// @file
/// Copies a texture onto a framebuffer, scaling with bilinear filtering.
///
/// Used to put a finished offscreen image on the back buffer.
///
/// @code
/// CORDITE_TRY_ASSIGN(BlitPass blit, BlitPass::create(device, shaders));
/// blit.draw(commandList, sceneColor, backBufferFramebuffer);
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

class BlitPass {
public:
    [[nodiscard]] static Result<BlitPass> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Draws `source` over the whole of `destination`. The command list must be open.
    void draw(nvrhi::ICommandList* commandList, nvrhi::ITexture* source, nvrhi::IFramebuffer* destination);

private:
    BlitPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle bindingLayout, nvrhi::SamplerHandle sampler,
             FullscreenPass pass);

    nvrhi::IDevice* m_device;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::SamplerHandle m_sampler;
    FullscreenPass m_pass;
};

} // namespace cordite
