#include "cordite/renderer/ScreenReflectionPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/GBufferBindings.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// Mirrors ScreenReflectionConstants in shaders/common/ScreenReflections.slang.
struct ScreenReflectionConstants {
    float intensity;
    float maxRoughness;
    float maxDistance;
    float thickness;
    uint32_t divisor;
    uint32_t steps;
    float padding0;
    float padding1;
};

static_assert(sizeof(ScreenReflectionConstants) == 32, "must match the cbuffer layout in ScreenReflections.slang");

/// The most steps a reflection is followed in, whatever it is asked: each is a read of the
/// depth buffer, for every pixel.
constexpr uint32_t kMaxSteps = 256;

constexpr const char* kCompositePath = "passes/ScreenReflectionComposite.slang";
constexpr const char* kCompositeEntryPoint = "screenReflectionCompositePixel";

/// Whether `settings` draw anything, with numbers the shaders can work with.
bool drawsAnything(const ScreenReflectionSettings& settings) {
    return settings.intensity > 0.0f && settings.maxRoughness > 0.0f && settings.maxDistance > 0.0f &&
           std::isfinite(settings.maxDistance) && settings.thickness > 0.0f && std::isfinite(settings.thickness) &&
           settings.steps > 0;
}

} // namespace

ScreenReflectionPass::ScreenReflectionPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_gbufferLayout(std::move(parts.gbufferLayout)),
      m_compositeLayout(std::move(parts.compositeLayout)),
      m_trace(std::move(parts.trace)),
      m_composite(std::move(parts.composite)),
      m_showOnly(std::move(parts.showOnly)) {}

Result<ScreenReflectionPass> ScreenReflectionPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                          nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr,
                   "the reflections from the screen need a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the reflections from the screen need a device and a view layout");
    }
    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(ScreenReflectionConstants), "ScreenReflectionConstants", kMaxViewsInFlight));
    nvrhi::BindingLayoutHandle gbufferLayout = createGBufferLayout(device, true);
    nvrhi::BindingLayoutDesc compositeDesc;
    compositeDesc.visibility = nvrhi::ShaderType::Pixel;
    compositeDesc.registerSpace = 2;
    compositeDesc.registerSpaceIsDescriptorSet = true;
    compositeDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    nvrhi::BindingLayoutHandle compositeLayout = device->createBindingLayout(compositeDesc);
    if (!constants || !gbufferLayout || !compositeLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the reflections from the screen");
    }

    FullscreenPassDesc trace;
    trace.debugName = "Screen reflections";
    trace.pixelShader = {.path = "passes/ScreenReflectionTrace.slang",
                         .entryPoint = "screenReflectionTracePixel",
                         .stage = ShaderStage::Pixel};
    trace.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), gbufferLayout};
    FullscreenPassDesc composite;
    composite.debugName = "Screen reflections composite";
    composite.pixelShader = {.path = kCompositePath, .entryPoint = kCompositeEntryPoint, .stage = ShaderStage::Pixel};
    composite.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), gbufferLayout, compositeLayout};
    FullscreenPassDesc showOnly = composite;
    showOnly.debugName = "Screen reflections only";
    showOnly.pixelShader.defines = {{"SCREEN_REFLECTIONS_ONLY", "1"}};
    CORDITE_TRY_ASSIGN(FullscreenPass tracePass, FullscreenPass::create(device, shaders, std::move(trace)));
    CORDITE_TRY_ASSIGN(FullscreenPass compositePass, FullscreenPass::create(device, shaders, std::move(composite)));
    CORDITE_TRY_ASSIGN(FullscreenPass showOnlyPass, FullscreenPass::create(device, shaders, std::move(showOnly)));
    return ScreenReflectionPass(Parts{.device = device,
                                      .constants = std::move(constants),
                                      .gbufferLayout = std::move(gbufferLayout),
                                      .compositeLayout = std::move(compositeLayout),
                                      .trace = std::move(tracePass),
                                      .composite = std::move(compositePass),
                                      .showOnly = std::move(showOnlyPass)});
}

uint32_t ScreenReflectionPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                    nvrhi::IBindingSet* viewBindings, const ScreenReflectionSettings& settings,
                                    bool showOnly) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "the reflections from the screen need a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return 0;
    }
    const auto drawNothing = [&] {
        if (showOnly) {
            // Nothing is put in place: what the debug view shows is black.
            commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                           nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        }
        return 0u;
    };
    if (!drawsAnything(settings)) {
        return drawNothing();
    }
    const auto fail = [&](const std::string& why) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no reflections from the screen are drawn: {}", why);
            m_reportedFailure = true;
        }
        return drawNothing();
    };

    const uint32_t divisor = std::clamp(settings.divisor, 1u, 4u);
    if (const Status prepared = targets.prepareScreenReflections(m_device, divisor); !prepared) {
        return fail(prepared.error().describe());
    }
    // Binding sets are cheap, and the command list keeps them alive until it has run.
    const nvrhi::BindingSetHandle gbuffer = createGBufferBindings(m_device, m_gbufferLayout, targets, m_constants);
    nvrhi::BindingSetDesc compositeDesc;
    compositeDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, targets.reflectionSource()),
                              nvrhi::BindingSetItem::Texture_SRV(1, targets.reflectionHits())};
    const nvrhi::BindingSetHandle composite = m_device->createBindingSet(compositeDesc, m_compositeLayout);
    if (!gbuffer || !composite) {
        return fail("their textures cannot be bound");
    }

    // The scene color is drawn into, and what the reflections meet is read from a copy.
    commandList->copyTexture(targets.reflectionSource(), nvrhi::TextureSlice(), targets.sceneColor(),
                             nvrhi::TextureSlice());
    const ScreenReflectionConstants constants{.intensity = std::min(settings.intensity, 1.0f),
                                              .maxRoughness = std::min(settings.maxRoughness, 1.0f),
                                              .maxDistance = settings.maxDistance,
                                              .thickness = settings.thickness,
                                              .divisor = divisor,
                                              .steps = std::min(settings.steps, kMaxSteps),
                                              .padding0 = 0.0f,
                                              .padding1 = 0.0f};
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    const std::array<nvrhi::IBindingSet*, 2> traceSets = {viewBindings, gbuffer.Get()};
    m_trace.draw(commandList, targets.reflectionHitsFramebuffer(), traceSets);
    const std::array<nvrhi::IBindingSet*, 3> compositeSets = {viewBindings, gbuffer.Get(), composite.Get()};
    (showOnly ? m_showOnly : m_composite).draw(commandList, targets.lightingFramebuffer(), compositeSets);
    return 2;
}

} // namespace cordite
