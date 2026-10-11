#include "cordite/renderer/BlitPass.h"

#include "cordite/core/Assert.h"

#include <array>
#include <utility>

namespace cordite {

BlitPass::BlitPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle bindingLayout, nvrhi::SamplerHandle sampler,
                   FullscreenPass pass)
    : m_device(device),
      m_bindingLayout(std::move(bindingLayout)),
      m_sampler(std::move(sampler)),
      m_pass(std::move(pass)) {}

Result<BlitPass> BlitPass::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    nvrhi::BindingLayoutHandle bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    nvrhi::SamplerHandle sampler = device->createSampler(samplerDesc);

    if (!bindingLayout || !sampler) {
        return makeError(ErrorCode::DeviceError, "cannot create the blit pass resources");
    }

    FullscreenPassDesc desc;
    desc.debugName = "Blit";
    desc.pixelShader.path = "passes/Blit.slang";
    desc.pixelShader.entryPoint = "blitPixel";
    desc.pixelShader.stage = ShaderStage::Pixel;
    desc.bindingLayouts = {bindingLayout};
    CORDITE_TRY_ASSIGN(FullscreenPass pass, FullscreenPass::create(device, shaders, std::move(desc)));

    return BlitPass(device, std::move(bindingLayout), std::move(sampler), std::move(pass));
}

void BlitPass::draw(nvrhi::ICommandList* commandList, nvrhi::ITexture* source, nvrhi::IFramebuffer* destination) {
    CORDITE_ASSERT(source != nullptr && destination != nullptr, "blit needs a source and a destination");
    if (source == nullptr || destination == nullptr) {
        return;
    }

    // Binding sets are cheap, and NVRHI keeps this one alive until the command list has
    // executed, so one is created per call rather than cached per source texture.
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        nvrhi::BindingSetItem::Texture_SRV(0, source),
        nvrhi::BindingSetItem::Sampler(0, m_sampler),
    };
    const nvrhi::BindingSetHandle bindingSet = m_device->createBindingSet(bindingSetDesc, m_bindingLayout);

    const std::array<nvrhi::IBindingSet*, 1> bindingSets = {bindingSet.Get()};
    m_pass.draw(commandList, destination, bindingSets);
}

} // namespace cordite
