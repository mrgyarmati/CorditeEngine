#include "cordite/renderer/DisplayEncodePass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <array>
#include <utility>

namespace cordite {

DisplayEncodePass::DisplayEncodePass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle bindingLayout,
                                     FullscreenPass pass)
    : m_device(device), m_bindingLayout(std::move(bindingLayout)), m_pass(std::move(pass)) {}

Result<DisplayEncodePass> DisplayEncodePass::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0)};
    nvrhi::BindingLayoutHandle bindingLayout = device->createBindingLayout(layoutDesc);
    if (!bindingLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the display encoding pass resources");
    }

    FullscreenPassDesc desc;
    desc.debugName = "Display encode";
    desc.pixelShader.path = "passes/DisplayEncode.slang";
    desc.pixelShader.entryPoint = "displayEncodePixel";
    desc.pixelShader.stage = ShaderStage::Pixel;
    desc.bindingLayouts = {bindingLayout};
    CORDITE_TRY_ASSIGN(FullscreenPass pass, FullscreenPass::create(device, shaders, std::move(desc)));
    return DisplayEncodePass(device, std::move(bindingLayout), std::move(pass));
}

void DisplayEncodePass::draw(nvrhi::ICommandList* commandList, nvrhi::ITexture* sceneColor,
                             nvrhi::IFramebuffer* destination) {
    CORDITE_ASSERT(sceneColor != nullptr && destination != nullptr, "encoding needs a source and a destination");
    if (sceneColor == nullptr || destination == nullptr) {
        return;
    }
    // Binding sets are cheap, and NVRHI keeps this one alive until the command list has
    // executed, so one is created per call rather than cached per source texture.
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, sceneColor)};
    const nvrhi::BindingSetHandle bindingSet = m_device->createBindingSet(bindingSetDesc, m_bindingLayout);
    if (!bindingSet) {
        CORDITE_LOG_ERROR("Renderer", "cannot bind the scene color for display encoding; the image is not updated");
        return;
    }

    const std::array<nvrhi::IBindingSet*, 1> bindingSets = {bindingSet.Get()};
    m_pass.draw(commandList, destination, bindingSets);
}

} // namespace cordite
