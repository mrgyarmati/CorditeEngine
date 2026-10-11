#include "cordite/renderer/DeferredLightingPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/GBufferBindings.h"

#include <array>
#include <utility>

namespace cordite {
namespace {

constexpr const char* kShaderPath = "passes/DeferredLighting.slang";
constexpr const char* kEntryPoint = "deferredLightingPixel";

Result<FullscreenPass> createPass(nvrhi::IDevice* device, ShaderManager& shaders, std::string debugName,
                                  ShaderRequest pixelShader, nvrhi::IBindingLayout* viewLayout,
                                  nvrhi::IBindingLayout* gbufferLayout) {
    FullscreenPassDesc desc;
    desc.debugName = std::move(debugName);
    desc.pixelShader = std::move(pixelShader);
    desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), nvrhi::BindingLayoutHandle(gbufferLayout)};
    return FullscreenPass::create(device, shaders, std::move(desc));
}

} // namespace

DeferredLightingPass::DeferredLightingPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle gbufferLayout,
                                           FullscreenPass pass, FullscreenPass lightingOnlyPass)
    : m_device(device),
      m_gbufferLayout(std::move(gbufferLayout)),
      m_pass(std::move(pass)),
      m_lightingOnlyPass(std::move(lightingOnlyPass)) {}

Result<DeferredLightingPass> DeferredLightingPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                          nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr, "the lighting pass needs a device and the view layout");
    nvrhi::BindingLayoutHandle gbufferLayout = createGBufferLayout(device, false);
    if (!gbufferLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the bindings of the lighting pass");
    }

    CORDITE_TRY_ASSIGN(FullscreenPass pass,
                       createPass(device, shaders, "Deferred lighting",
                                  {.path = kShaderPath, .entryPoint = kEntryPoint, .stage = ShaderStage::Pixel},
                                  viewLayout, gbufferLayout));
    CORDITE_TRY_ASSIGN(FullscreenPass lightingOnlyPass, createPass(device, shaders, "Lighting only",
                                                                   {.path = kShaderPath,
                                                                    .entryPoint = kEntryPoint,
                                                                    .stage = ShaderStage::Pixel,
                                                                    .defines = {{"LIGHTING_ONLY", "1"}}},
                                                                   viewLayout, gbufferLayout));
    return DeferredLightingPass(device, std::move(gbufferLayout), std::move(pass), std::move(lightingOnlyPass));
}

void DeferredLightingPass::draw(nvrhi::ICommandList* commandList, const SceneTargets& targets,
                                nvrhi::IBindingSet* viewBindings, bool lightingOnly) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "lighting needs a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return;
    }
    const nvrhi::BindingSetHandle gbuffer = createGBufferBindings(m_device, m_gbufferLayout, targets, nullptr);
    if (!gbuffer) {
        CORDITE_LOG_ERROR("Renderer", "cannot bind the G-buffer for lighting; the scene is not lit");
        return;
    }
    const std::array<nvrhi::IBindingSet*, 2> bindingSets = {viewBindings, gbuffer.Get()};
    (lightingOnly ? m_lightingOnlyPass : m_pass).draw(commandList, targets.lightingFramebuffer(), bindingSets);
}

} // namespace cordite
