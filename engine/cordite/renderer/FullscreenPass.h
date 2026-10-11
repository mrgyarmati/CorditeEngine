#pragma once

/// @file
/// Building block for screen-space passes: draws one triangle that covers the render
/// target with a given pixel shader.
///
/// @code
/// FullscreenPassDesc desc;
/// desc.debugName = "Vignette";
/// desc.pixelShader = {.path = "passes/Vignette.slang", .entryPoint = "vignettePixel", .stage = ShaderStage::Pixel};
/// desc.bindingLayouts = {layout};
/// CORDITE_TRY_ASSIGN(FullscreenPass pass, FullscreenPass::create(device, shaders, std::move(desc)));
///
/// const std::array<nvrhi::IBindingSet*, 1> bindingSets = {bindingSet.Get()};
/// pass.draw(commandList, framebuffer, bindingSets);
/// @endcode
///
/// Graphics pipelines depend on the formats of the framebuffer they render to, so the
/// pass creates one per distinct framebuffer layout on first use and reuses it.

#include "cordite/core/Result.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cordite {

struct FullscreenPassDesc {
    /// Shown in graphics debuggers and error messages.
    std::string debugName = "Fullscreen";
    ShaderRequest pixelShader;
    /// Layouts of the binding sets passed to draw(), in the same order.
    std::vector<nvrhi::BindingLayoutHandle> bindingLayouts;
    /// Blending of the first render target. The default overwrites it.
    nvrhi::BlendState::RenderTarget blend;
};

class FullscreenPass {
public:
    /// Loads the shaders. Fails when they cannot be compiled.
    [[nodiscard]] static Result<FullscreenPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                       FullscreenPassDesc desc);

    /// Draws over the whole framebuffer. The command list must be open. Depth is neither
    /// tested nor written.
    void draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
              std::span<nvrhi::IBindingSet* const> bindingSets);

private:
    FullscreenPass() = default;

    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo);

    nvrhi::IDevice* m_device = nullptr;
    FullscreenPassDesc m_desc;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_pixelShader;
    std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_pipelines;
};

} // namespace cordite
