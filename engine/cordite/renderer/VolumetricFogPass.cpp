#include "cordite/renderer/VolumetricFogPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <array>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The register space of the passes' own sets: the view's bindings are in space 0.
constexpr uint32_t kRegisterSpace = 1;

/// The threads of a group of the first pass, a box each, and of the second, a column each.
/// Matches the [numthreads] of the shaders.
constexpr uint32_t kScatteringGroup = 4;
constexpr uint32_t kIntegrationGroup = 8;

nvrhi::BindingLayoutHandle createLayout(nvrhi::IDevice* device, uint32_t registerSpace,
                                        std::vector<nvrhi::BindingLayoutItem> bindings) {
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Compute;
    desc.registerSpace = registerSpace;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindings = std::move(bindings);
    return device->createBindingLayout(desc);
}

} // namespace

VolumetricFogPass::VolumetricFogPass(Parts parts)
    : m_device(parts.device),
      m_scatteringLayout(std::move(parts.scatteringLayout)),
      m_constantsLayout(std::move(parts.constantsLayout)),
      m_integrationLayout(std::move(parts.integrationLayout)),
      m_scattering(std::move(parts.scattering)),
      m_integration(std::move(parts.integration)) {}

Result<VolumetricFogPass> VolumetricFogPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                    nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr, "the fog's light needs a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the fog's light needs a device and the view layout");
    }
    nvrhi::BindingLayoutHandle scatteringLayout =
        createLayout(device, kRegisterSpace, {nvrhi::BindingLayoutItem::Texture_UAV(0)});
    nvrhi::BindingLayoutHandle constantsLayout =
        createLayout(device, 0, {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)});
    nvrhi::BindingLayoutHandle integrationLayout = createLayout(
        device, kRegisterSpace, {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_UAV(0)});
    if (!scatteringLayout || !constantsLayout || !integrationLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the bindings of the fog's light");
    }

    CORDITE_TRY_ASSIGN(
        ComputePass scattering,
        ComputePass::create(device, shaders,
                            {.debugName = "Fog scattering",
                             .shader = {.path = "passes/FogScattering.slang",
                                        .entryPoint = "fogScatteringCompute",
                                        .stage = ShaderStage::Compute},
                             .bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), scatteringLayout}}));
    CORDITE_TRY_ASSIGN(ComputePass integration,
                       ComputePass::create(device, shaders,
                                           {.debugName = "Fog integration",
                                            .shader = {.path = "passes/FogIntegration.slang",
                                                       .entryPoint = "fogIntegrationCompute",
                                                       .stage = ShaderStage::Compute},
                                            .bindingLayouts = {constantsLayout, integrationLayout}}));
    return VolumetricFogPass(Parts{.device = device,
                                   .scatteringLayout = std::move(scatteringLayout),
                                   .constantsLayout = std::move(constantsLayout),
                                   .integrationLayout = std::move(integrationLayout),
                                   .scattering = std::move(scattering),
                                   .integration = std::move(integration)});
}

uint32_t VolumetricFogPass::draw(nvrhi::ICommandList* commandList, const SceneTargets& targets,
                                 nvrhi::IBindingSet* viewBindings, nvrhi::IBuffer* viewConstants) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr && viewConstants != nullptr,
                   "the fog's light needs a command list, valid targets and the view");
    nvrhi::ITexture* scatteringVolume = targets.fogScattering();
    nvrhi::ITexture* sumsVolume = targets.fogVolume();
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr || viewConstants == nullptr ||
        scatteringVolume == nullptr || sumsVolume == nullptr) {
        return 0;
    }

    // Binding sets are cheap, and the command list keeps them alive until it has run.
    nvrhi::BindingSetDesc scatteringDesc;
    scatteringDesc.bindings = {nvrhi::BindingSetItem::Texture_UAV(0, scatteringVolume)};
    nvrhi::BindingSetDesc constantsDesc;
    constantsDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, viewConstants)};
    nvrhi::BindingSetDesc integrationDesc;
    integrationDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, scatteringVolume),
                                nvrhi::BindingSetItem::Texture_UAV(0, sumsVolume)};
    const nvrhi::BindingSetHandle scatteringSet = m_device->createBindingSet(scatteringDesc, m_scatteringLayout);
    const nvrhi::BindingSetHandle constantsSet = m_device->createBindingSet(constantsDesc, m_constantsLayout);
    const nvrhi::BindingSetHandle integrationSet = m_device->createBindingSet(integrationDesc, m_integrationLayout);
    if (!scatteringSet || !constantsSet || !integrationSet) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "the fog is lit by no light: the volumes of its light cannot be bound");
            m_reportedFailure = true;
        }
        return 0;
    }

    const nvrhi::TextureDesc& grid = sumsVolume->getDesc();
    const std::array<nvrhi::IBindingSet*, 2> scatteringSets = {viewBindings, scatteringSet.Get()};
    m_scattering.dispatch(commandList, scatteringSets,
                          UVec3(groupsFor(grid.width, kScatteringGroup), groupsFor(grid.height, kScatteringGroup),
                                groupsFor(grid.depth, kScatteringGroup)));
    const std::array<nvrhi::IBindingSet*, 2> integrationSets = {constantsSet.Get(), integrationSet.Get()};
    m_integration.dispatch(
        commandList, integrationSets,
        UVec3(groupsFor(grid.width, kIntegrationGroup), groupsFor(grid.height, kIntegrationGroup), 1));
    return 2;
}

} // namespace cordite
