#include "cordite/renderer/DepthOfFieldPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <array>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

/// Mirrors DepthOfFieldConstants in shaders/common/DepthOfField.slang.
struct DepthOfFieldConstants {
    float focusDistance;
    float focusRange;
    float focusTransition;
    float nearBlur;
    float farBlur;
    float blurRadius;
    uint32_t tapCount;
    float showFocus;
};

static_assert(sizeof(DepthOfFieldConstants) == 32, "must match the cbuffer layout in DepthOfField.slang");

/// The register space of the set: the view's bindings are in space 0.
constexpr uint32_t kRegisterSpace = 1;

} // namespace

DepthOfFieldPass::DepthOfFieldPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_layout(std::move(parts.layout)),
      m_prefilter(std::move(parts.prefilter)),
      m_gather(std::move(parts.gather)),
      m_composite(std::move(parts.composite)) {}

Result<DepthOfFieldPass> DepthOfFieldPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                  nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr, "the depth of field needs a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the depth of field needs a device and the view layout");
    }

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(DepthOfFieldConstants), "DepthOfFieldConstants", kMaxViewsInFlight));
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                           nvrhi::BindingLayoutItem::Texture_SRV(2)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
    if (!constants || !layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the depth of field");
    }

    const auto pass = [&](const char* name, const char* entryPoint, const nvrhi::BlendState::RenderTarget& blend) {
        FullscreenPassDesc desc;
        desc.debugName = name;
        desc.pixelShader = {.path = "passes/DepthOfField.slang", .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
        desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), layout};
        desc.blend = blend;
        return FullscreenPass::create(device, shaders, std::move(desc));
    };
    CORDITE_TRY_ASSIGN(FullscreenPass prefilter, pass("Depth of field prefilter", "depthOfFieldPrefilterPixel", {}));
    CORDITE_TRY_ASSIGN(FullscreenPass gather, pass("Depth of field gather", "depthOfFieldGatherPixel", {}));
    // Over the picture as much as alpha says; the alpha of the picture stays as it is.
    const nvrhi::BlendState::RenderTarget over = nvrhi::BlendState::RenderTarget()
                                                     .enableBlend()
                                                     .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                                                     .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                                                     .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                                                     .setDestBlendAlpha(nvrhi::BlendFactor::One);
    CORDITE_TRY_ASSIGN(FullscreenPass composite, pass("Depth of field composite", "depthOfFieldCompositePixel", over));

    return DepthOfFieldPass(Parts{.device = device,
                                  .constants = std::move(constants),
                                  .layout = std::move(layout),
                                  .prefilter = std::move(prefilter),
                                  .gather = std::move(gather),
                                  .composite = std::move(composite)});
}

nvrhi::BindingSetHandle DepthOfFieldPass::bindingsFor(nvrhi::ITexture* source, nvrhi::ITexture* depth,
                                                      nvrhi::ITexture* prefiltered) const {
    nvrhi::BindingSetDesc desc;
    desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                     nvrhi::BindingSetItem::Texture_SRV(0, source), nvrhi::BindingSetItem::Texture_SRV(1, depth),
                     nvrhi::BindingSetItem::Texture_SRV(2, prefiltered)};
    return m_device->createBindingSet(desc, m_layout);
}

uint32_t DepthOfFieldPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                nvrhi::IBindingSet* viewBindings, const PostProcessSettings& settings,
                                const DepthOfFieldFrame& frame) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "depth of field needs a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return 0;
    }
    const auto fail = [this](std::string_view why) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no depth of field is drawn: {}", why);
            m_reportedFailure = true;
        }
        return 0u;
    };
    if (const Status prepared = targets.prepareDepthOfField(m_device); !prepared) {
        return fail(prepared.error().describe());
    }
    // Binding sets are cheap, and the command list keeps them alive until it has run.
    // Every set holds three textures; a pass reads none of the image it draws into, and
    // the first two passes do not read the third.
    const nvrhi::BindingSetHandle prefilterBindings =
        bindingsFor(targets.sceneColor(), targets.depth(), targets.depthOfFieldBlur());
    const nvrhi::BindingSetHandle gatherBindings =
        bindingsFor(targets.depthOfFieldSource(), targets.depth(), targets.depthOfFieldSource());
    const nvrhi::BindingSetHandle compositeBindings =
        bindingsFor(targets.depthOfFieldBlur(), targets.depth(), targets.depthOfFieldSource());
    if (!prefilterBindings || !gatherBindings || !compositeBindings) {
        return fail("its textures cannot be bound");
    }

    const PostProcessSettings look = sanitized(settings);
    const DepthOfFieldConstants constants{
        .focusDistance = look.focusDistance,
        .focusRange = look.focusRange,
        .focusTransition = look.focusTransition,
        .nearBlur = look.nearBlur,
        .farBlur = look.farBlur,
        // A share of the height of the picture, in texels of the half-sized images.
        .blurRadius = look.blurSize * static_cast<float>(targets.size().y) * 0.5f,
        .tapCount = postProcessSettingsFor(frame.quality).depthOfFieldTaps,
        .showFocus = frame.showFocus ? 1.0f : 0.0f,
    };
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    const std::array<nvrhi::IBindingSet*, 2> prefilterSets = {viewBindings, prefilterBindings.Get()};
    m_prefilter.draw(commandList, targets.depthOfFieldSourceFramebuffer(), prefilterSets);
    const std::array<nvrhi::IBindingSet*, 2> gatherSets = {viewBindings, gatherBindings.Get()};
    m_gather.draw(commandList, targets.depthOfFieldBlurFramebuffer(), gatherSets);
    const std::array<nvrhi::IBindingSet*, 2> compositeSets = {viewBindings, compositeBindings.Get()};
    m_composite.draw(commandList, targets.lightingFramebuffer(), compositeSets);
    return 3;
}

} // namespace cordite
