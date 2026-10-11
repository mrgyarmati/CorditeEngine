#include "cordite/renderer/FullscreenPass.h"

#include "cordite/core/Assert.h"

namespace cordite {

Result<FullscreenPass> FullscreenPass::create(nvrhi::IDevice* device, ShaderManager& shaders, FullscreenPassDesc desc) {
    FullscreenPass pass;
    pass.m_device = device;

    ShaderRequest vertexRequest;
    vertexRequest.path = "common/Fullscreen.slang";
    vertexRequest.entryPoint = "fullscreenVertex";
    vertexRequest.stage = ShaderStage::Vertex;
    CORDITE_TRY_ASSIGN(pass.m_vertexShader, shaders.get(vertexRequest));
    CORDITE_TRY_ASSIGN(pass.m_pixelShader, shaders.get(desc.pixelShader));

    pass.m_desc = std::move(desc);
    return pass;
}

nvrhi::IGraphicsPipeline* FullscreenPass::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo) {
    for (const auto& [info, pipeline] : m_pipelines) {
        if (info == framebufferInfo) {
            return pipeline;
        }
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = m_vertexShader;
    pipelineDesc.PS = m_pixelShader;
    for (const nvrhi::BindingLayoutHandle& layout : m_desc.bindingLayouts) {
        pipelineDesc.bindingLayouts.push_back(layout);
    }
    pipelineDesc.renderState.rasterState.setCullNone();
    pipelineDesc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();
    pipelineDesc.renderState.blendState.targets[0] = m_desc.blend;

    const nvrhi::GraphicsPipelineHandle pipeline = m_device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    CORDITE_VERIFY(pipeline, "cannot create the pipeline for fullscreen pass '{}'", m_desc.debugName);
    m_pipelines.emplace_back(framebufferInfo, pipeline);
    return pipeline;
}

void FullscreenPass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                          std::span<nvrhi::IBindingSet* const> bindingSets) {
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr, "draw needs a command list and a framebuffer");
    CORDITE_ASSERT(bindingSets.size() == m_desc.bindingLayouts.size(), "pass '{}' expects {} binding sets, got {}",
                   m_desc.debugName, m_desc.bindingLayouts.size(), bindingSets.size());

    const nvrhi::FramebufferInfoEx& framebufferInfo = framebuffer->getFramebufferInfo();
    nvrhi::IGraphicsPipeline* pipeline = pipelineFor(framebufferInfo);
    if (pipeline == nullptr) {
        return;
    }

    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = framebuffer;
    state.viewport.addViewportAndScissorRect(framebufferInfo.getViewport());
    for (nvrhi::IBindingSet* bindingSet : bindingSets) {
        state.bindings.push_back(bindingSet);
    }

    commandList->setGraphicsState(state);
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}

} // namespace cordite
