#include "cordite/renderer/AmbientOcclusionPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// Mirrors AmbientOcclusionConstants in shaders/common/AmbientOcclusion.slang.
struct AmbientOcclusionConstants {
    float radius;
    float intensity;
    float bias;
    uint32_t divisor;
    uint32_t sliceCount;
    uint32_t stepCount;
    float padding0;
    float padding1;
};

static_assert(sizeof(AmbientOcclusionConstants) == 32, "must match the cbuffer layout in AmbientOcclusion.slang");

/// The register space of the set: the view's bindings are in space 0.
constexpr uint32_t kRegisterSpace = 1;

/// The most a pixel is asked to do, whatever the settings say.
constexpr uint32_t kMaxSlices = 8;
constexpr uint32_t kMaxSteps = 16;

/// `value` between `low` and `high`, or `fallback` when it is no number.
float within(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

} // namespace

AmbientOcclusionPass::AmbientOcclusionPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_layout(std::move(parts.layout)),
      m_trace(std::move(parts.trace)),
      m_resolve(std::move(parts.resolve)) {}

Result<AmbientOcclusionPass> AmbientOcclusionPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                          nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr,
                   "the ambient occlusion pass needs a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the ambient occlusion pass needs a device and the view layout");
    }

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(AmbientOcclusionConstants), "AmbientOcclusionConstants", kMaxViewsInFlight));
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
    if (!constants || !layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the ambient occlusion pass");
    }

    const auto pass = [&](const char* name, const char* path, const char* entryPoint) {
        FullscreenPassDesc desc;
        desc.debugName = name;
        desc.pixelShader = {.path = path, .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
        desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), layout};
        return FullscreenPass::create(device, shaders, std::move(desc));
    };
    CORDITE_TRY_ASSIGN(FullscreenPass trace,
                       pass("Ambient occlusion", "passes/AmbientOcclusion.slang", "ambientOcclusionPixel"));
    CORDITE_TRY_ASSIGN(FullscreenPass resolve, pass("Ambient occlusion resolve", "passes/AmbientOcclusionResolve.slang",
                                                    "ambientOcclusionResolvePixel"));

    return AmbientOcclusionPass(Parts{.device = device,
                                      .constants = std::move(constants),
                                      .layout = std::move(layout),
                                      .trace = std::move(trace),
                                      .resolve = std::move(resolve)});
}

nvrhi::BindingSetHandle AmbientOcclusionPass::bindingsFor(nvrhi::ITexture* first, nvrhi::ITexture* second) const {
    nvrhi::BindingSetDesc desc;
    desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                     nvrhi::BindingSetItem::Texture_SRV(0, first), nvrhi::BindingSetItem::Texture_SRV(1, second)};
    return m_device->createBindingSet(desc, m_layout);
}

uint32_t AmbientOcclusionPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                    nvrhi::IBindingSet* viewBindings, const AmbientOcclusionSettings& settings) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "ambient occlusion needs a command list, valid targets and the view");
    const float intensity = within(settings.intensity, 0.0f, 8.0f, 0.0f);
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr || intensity <= 0.0f) {
        return 0;
    }

    const uint32_t divisor = std::clamp(settings.divisor, 1u, 4u);
    if (const Status prepared = targets.prepareTracedOcclusion(m_device, divisor); !prepared) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no ambient occlusion is drawn: {}", prepared.error().describe());
            m_reportedFailure = true;
        }
        return 0;
    }
    // Binding sets are cheap, and the command list keeps them alive until it has run.
    const nvrhi::BindingSetHandle traceBindings = bindingsFor(targets.depth(), targets.gbuffer(GBufferTarget::Normal));
    const nvrhi::BindingSetHandle resolveBindings = bindingsFor(targets.tracedOcclusion(), targets.depth());
    if (!traceBindings || !resolveBindings) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no ambient occlusion is drawn: its textures cannot be bound");
            m_reportedFailure = true;
        }
        return 0;
    }

    AmbientOcclusionConstants constants{};
    constants.radius = within(settings.radius, 1.0e-3f, 100.0f, 0.5f);
    constants.intensity = intensity;
    constants.bias = within(settings.bias, 0.0f, kHalfPi, 0.0f);
    constants.divisor = divisor;
    constants.sliceCount = std::clamp(settings.slices, 1u, kMaxSlices);
    constants.stepCount = std::clamp(settings.steps, 1u, kMaxSteps);
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    const std::array<nvrhi::IBindingSet*, 2> traceSets = {viewBindings, traceBindings.Get()};
    m_trace.draw(commandList, targets.tracedOcclusionFramebuffer(), traceSets);
    const std::array<nvrhi::IBindingSet*, 2> resolveSets = {viewBindings, resolveBindings.Get()};
    m_resolve.draw(commandList, targets.ambientOcclusionFramebuffer(), resolveSets);
    return 2;
}

} // namespace cordite
