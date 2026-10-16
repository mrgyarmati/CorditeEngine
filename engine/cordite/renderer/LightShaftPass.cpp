#include "cordite/renderer/LightShaftPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/LightShafts.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

/// Mirrors LightShaftConstants in shaders/common/LightShafts.slang.
struct LightShaftConstants {
    Vec3 sunColor;
    float threshold;
    Vec3 towardsSun;
    float intensity;
    Vec2 sunPoint;
    float stepShare;
    float padding;
};

static_assert(sizeof(LightShaftConstants) == 48, "must match the cbuffer layout in LightShafts.slang");

/// The register space of the set: the view's bindings are in space 0.
constexpr uint32_t kRegisterSpace = 1;

/// The steps of the three passes of the blur, as shares of the length of the shafts: each
/// eight times the one before, so that eight taps of each make 512 over the whole length.
constexpr std::array<float, 3> kBlurSteps = {1.0f / 512.0f, 1.0f / 64.0f, 1.0f / 8.0f};

/// The constants are written once for the source and the composite, and once more for
/// each pass of the blur, whose steps differ: four versions of the buffer for every view on
/// its way through the GPU, where the other passes need one.
constexpr uint32_t kWritesPerView = 1 + static_cast<uint32_t>(kBlurSteps.size());

} // namespace

LightShaftPass::LightShaftPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_sampler(std::move(parts.sampler)),
      m_layout(std::move(parts.layout)),
      m_source(std::move(parts.source)),
      m_blur(std::move(parts.blur)),
      m_composite(std::move(parts.composite)) {}

Result<LightShaftPass> LightShaftPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                              nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr, "the light shafts need a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the light shafts need a device and the view layout");
    }

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(LightShaftConstants), "LightShaftConstants", kMaxViewsInFlight * kWritesPerView));
    nvrhi::SamplerHandle sampler = device->createSampler(
        nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                           nvrhi::BindingLayoutItem::Sampler(0)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
    if (!constants || !sampler || !layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the light shafts");
    }

    const auto pass = [&](const char* name, const char* entryPoint, const nvrhi::BlendState::RenderTarget& blend) {
        FullscreenPassDesc desc;
        desc.debugName = name;
        desc.pixelShader = {.path = "passes/LightShafts.slang", .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
        desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), layout};
        desc.blend = blend;
        return FullscreenPass::create(device, shaders, std::move(desc));
    };
    CORDITE_TRY_ASSIGN(FullscreenPass source, pass("Light shaft source", "lightShaftSourcePixel", {}));
    CORDITE_TRY_ASSIGN(FullscreenPass blur, pass("Light shaft blur", "lightShaftBlurPixel", {}));
    // Added to the light; the alpha of the picture stays as it is.
    const nvrhi::BlendState::RenderTarget added = nvrhi::BlendState::RenderTarget()
                                                      .enableBlend()
                                                      .setSrcBlend(nvrhi::BlendFactor::One)
                                                      .setDestBlend(nvrhi::BlendFactor::One)
                                                      .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                                                      .setDestBlendAlpha(nvrhi::BlendFactor::One);
    CORDITE_TRY_ASSIGN(FullscreenPass composite, pass("Light shaft composite", "lightShaftCompositePixel", added));

    return LightShaftPass(Parts{.device = device,
                                .constants = std::move(constants),
                                .sampler = std::move(sampler),
                                .layout = std::move(layout),
                                .source = std::move(source),
                                .blur = std::move(blur),
                                .composite = std::move(composite)});
}

nvrhi::BindingSetHandle LightShaftPass::bindingsFor(nvrhi::ITexture* source, nvrhi::ITexture* depth) const {
    nvrhi::BindingSetDesc desc;
    desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                     nvrhi::BindingSetItem::Texture_SRV(0, source), nvrhi::BindingSetItem::Texture_SRV(1, depth),
                     nvrhi::BindingSetItem::Sampler(0, m_sampler)};
    return m_device->createBindingSet(desc, m_layout);
}

uint32_t LightShaftPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                              const PostProcessSettings& settings, const LightShaftFrame& frame) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "light shafts need a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return 0;
    }
    // A sun behind the view, or a view in which every ray is parallel: nothing to draw.
    const float fade = lightShaftFade(frame.view, frame.sun.towards);
    const std::optional<Vec2> point = sunScreenPoint(frame.view, frame.sun.towards);
    if (!(fade > 0.0f) || !point) {
        return 0;
    }
    const auto fail = [this](std::string_view why) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no light shafts are drawn: {}", why);
            m_reportedFailure = true;
        }
        return 0u;
    };
    if (const Status prepared = targets.prepareLightShafts(m_device); !prepared) {
        return fail(prepared.error().describe());
    }
    // Binding sets are cheap, and the command list keeps them alive until it has run. The
    // blur goes back and forth between the two images.
    nvrhi::ITexture* depth = targets.depth();
    const std::array<nvrhi::BindingSetHandle, 3> reads = {bindingsFor(targets.sceneColor(), depth),
                                                          bindingsFor(targets.lightShaftImage(0), depth),
                                                          bindingsFor(targets.lightShaftImage(1), depth)};
    if (!reads[0] || !reads[1] || !reads[2]) {
        return fail("their textures cannot be bound");
    }

    const PostProcessSettings look = sanitized(settings);
    LightShaftConstants constants{.sunColor = frame.sun.color,
                                  .threshold = look.lightShaftThreshold,
                                  .towardsSun = frame.sun.towards,
                                  .intensity = look.lightShaftIntensity * fade,
                                  .sunPoint = *point,
                                  .stepShare = 0.0f,
                                  .padding = 0.0f};
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));
    std::array<nvrhi::IBindingSet*, 2> sets = {viewBindings, reads[0].Get()};
    m_source.draw(commandList, targets.lightShaftFramebuffer(0), sets);

    // From the first image into the second, back, and into the second again.
    uint32_t from = 0;
    for (const float step : kBlurSteps) {
        constants.stepShare = step * look.lightShaftLength;
        commandList->writeBuffer(m_constants, &constants, sizeof(constants));
        sets[1] = reads[from + 1].Get();
        m_blur.draw(commandList, targets.lightShaftFramebuffer(1 - from), sets);
        from = 1 - from;
    }

    if (frame.showShafts) {
        commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
    }
    sets[1] = reads[from + 1].Get();
    m_composite.draw(commandList, targets.lightingFramebuffer(), sets);
    return 5;
}

} // namespace cordite
