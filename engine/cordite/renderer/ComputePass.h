#pragma once

/// @file
/// Building block for passes that compute rather than draw: one compute shader, run over a
/// grid of thread groups.
///
/// @code
/// ComputePassDesc desc;
/// desc.debugName = "Fog scattering";
/// desc.shader = {.path = "passes/FogScattering.slang",
///                .entryPoint = "fogScatteringCompute",
///                .stage = ShaderStage::Compute};
/// desc.bindingLayouts = {viewLayout, layout};
/// CORDITE_TRY_ASSIGN(ComputePass pass, ComputePass::create(device, shaders, std::move(desc)));
///
/// const std::array<nvrhi::IBindingSet*, 2> bindingSets = {viewBindings, bindingSet.Get()};
/// pass.dispatch(commandList, bindingSets, UVec3(groupsFor(columns, 4), groupsFor(rows, 4), groupsFor(slices, 4)));
/// @endcode
///
/// A compute pipeline depends on nothing but its shader and its layouts, so it is made once,
/// with the pass. What the shader writes, it writes through its binding sets, and NVRHI puts
/// the barriers between a dispatch and whatever reads what it wrote.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cordite {

struct ComputePassDesc {
    /// Shown in graphics debuggers and error messages.
    std::string debugName = "Compute";
    /// A request of ShaderStage::Compute.
    ShaderRequest shader;
    /// Layouts of the binding sets passed to dispatch(), in the same order.
    std::vector<nvrhi::BindingLayoutHandle> bindingLayouts;
};

class ComputePass {
public:
    /// Loads the shader and makes the pipeline. Fails when the request is not of a compute
    /// shader, when the shader cannot be compiled, and when the pipeline cannot be made.
    [[nodiscard]] static Result<ComputePass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                    ComputePassDesc desc);

    /// Runs the shader in `groups` thread groups: so many across, down and deep. The
    /// command list must be open, and there must be a binding set for each layout; a count
    /// of none in any direction runs nothing.
    void dispatch(nvrhi::ICommandList* commandList, std::span<nvrhi::IBindingSet* const> bindingSets, UVec3 groups);

private:
    ComputePass() = default;

    std::string m_debugName;
    size_t m_layoutCount = 0;
    nvrhi::ComputePipelineHandle m_pipeline;
};

/// How many thread groups of `groupSize` threads cover `count` items: count / groupSize,
/// rounded up. A group size of none covers nothing.
[[nodiscard]] constexpr uint32_t groupsFor(uint32_t count, uint32_t groupSize) {
    return groupSize == 0 ? 0 : count / groupSize + (count % groupSize != 0 ? 1 : 0);
}

} // namespace cordite
