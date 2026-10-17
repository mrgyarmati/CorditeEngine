#include "cordite/renderer/GBufferBindings.h"

#include "cordite/core/Assert.h"

namespace cordite {
namespace {

/// The register space of the G-buffer's binding set; the view's bindings are in space 0.
constexpr uint32_t kGBufferRegisterSpace = 1;

/// The G-buffer targets, the depth, the ambient occlusion and the motion vectors.
constexpr uint32_t kTextureCount = static_cast<uint32_t>(kGBufferTargetCount) + 3;

} // namespace

nvrhi::BindingLayoutHandle createGBufferLayout(nvrhi::IDevice* device, bool withConstants) {
    CORDITE_ASSERT(device != nullptr, "a G-buffer layout needs a device");

    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Pixel;
    desc.registerSpace = kGBufferRegisterSpace;
    desc.registerSpaceIsDescriptorSet = true;
    if (withConstants) {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    }
    for (uint32_t slot = 0; slot < kTextureCount; ++slot) {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
    }
    return device->createBindingLayout(desc);
}

nvrhi::BindingSetHandle createGBufferBindings(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
                                              const SceneTargets& targets, nvrhi::IBuffer* constants) {
    CORDITE_ASSERT(device != nullptr && layout != nullptr && targets.isValid(),
                   "G-buffer bindings need a device, a layout and valid targets");

    nvrhi::BindingSetDesc desc;
    if (constants != nullptr) {
        desc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(0, constants));
    }
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(0, targets.gbuffer(GBufferTarget::BaseColor)));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(1, targets.gbuffer(GBufferTarget::Normal)));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(2, targets.gbuffer(GBufferTarget::Parameters)));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(3, targets.gbuffer(GBufferTarget::Emissive)));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(4, targets.depth()));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(5, targets.ambientOcclusion()));
    desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(6, targets.motion()));
    return device->createBindingSet(desc, layout);
}

} // namespace cordite
