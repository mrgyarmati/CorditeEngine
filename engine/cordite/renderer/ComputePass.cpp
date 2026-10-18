#include "cordite/renderer/ComputePass.h"

#include "cordite/core/Assert.h"

#include <utility>

namespace cordite {

Result<ComputePass> ComputePass::create(nvrhi::IDevice* device, ShaderManager& shaders, ComputePassDesc desc) {
    CORDITE_ASSERT(device != nullptr, "compute pass '{}' needs a device", desc.debugName);
    if (device == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "compute pass '{}' needs a device", desc.debugName);
    }
    if (desc.shader.stage != ShaderStage::Compute) {
        return makeError(ErrorCode::InvalidArgument, "compute pass '{}' is given {}, which is no compute shader",
                         desc.debugName, desc.shader.describe());
    }
    CORDITE_TRY_ASSIGN(const nvrhi::ShaderHandle shader, shaders.get(desc.shader));

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = shader;
    for (const nvrhi::BindingLayoutHandle& layout : desc.bindingLayouts) {
        pipelineDesc.bindingLayouts.push_back(layout);
    }
    nvrhi::ComputePipelineHandle pipeline = device->createComputePipeline(pipelineDesc);
    if (!pipeline) {
        return makeError(ErrorCode::DeviceError, "cannot create the pipeline of compute pass '{}'", desc.debugName);
    }

    ComputePass pass;
    pass.m_layoutCount = desc.bindingLayouts.size();
    pass.m_debugName = std::move(desc.debugName);
    pass.m_pipeline = std::move(pipeline);
    return pass;
}

void ComputePass::dispatch(nvrhi::ICommandList* commandList, std::span<nvrhi::IBindingSet* const> bindingSets,
                           UVec3 groups) {
    CORDITE_ASSERT(commandList != nullptr, "compute pass '{}' needs a command list", m_debugName);
    CORDITE_ASSERT(bindingSets.size() == m_layoutCount, "compute pass '{}' expects {} binding sets, got {}",
                   m_debugName, m_layoutCount, bindingSets.size());
    if (commandList == nullptr || !m_pipeline || bindingSets.size() != m_layoutCount || groups.x == 0 ||
        groups.y == 0 || groups.z == 0) {
        return;
    }

    nvrhi::ComputeState state;
    state.pipeline = m_pipeline;
    for (nvrhi::IBindingSet* bindingSet : bindingSets) {
        state.bindings.push_back(bindingSet);
    }
    commandList->setComputeState(state);
    commandList->dispatch(groups.x, groups.y, groups.z);
}

} // namespace cordite
