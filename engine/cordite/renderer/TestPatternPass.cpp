#include "cordite/renderer/TestPatternPass.h"

#include "cordite/core/Assert.h"

#include <nvrhi/utils.h>

#include <array>
#include <utility>

namespace cordite {
namespace {

/// Mirrors TestPatternConstants in shaders/passes/TestPattern.slang.
struct TestPatternConstants {
    Vec4 tint;
    Vec2 resolution;
    float time;
    float padding;
};

static_assert(sizeof(TestPatternConstants) == 32, "must match the cbuffer layout in TestPattern.slang");

} // namespace

TestPatternPass::TestPatternPass(nvrhi::BufferHandle constantBuffer, nvrhi::BindingSetHandle bindingSet,
                                 FullscreenPass pass)
    : m_constantBuffer(std::move(constantBuffer)), m_bindingSet(std::move(bindingSet)), m_pass(std::move(pass)) {}

Result<TestPatternPass> TestPatternPass::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    // A volatile constant buffer is rewritten every frame without a GPU-side copy.
    constexpr uint32_t kMaxVersions = 16;
    nvrhi::BufferHandle constantBuffer = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(TestPatternConstants), "TestPatternConstants", kMaxVersions));
    if (!constantBuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the test pattern constant buffer");
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constantBuffer)};
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingSetHandle bindingSet;
    if (!nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::Pixel, 0, bindingSetDesc, bindingLayout,
                                                 bindingSet)) {
        return makeError(ErrorCode::DeviceError, "cannot create the test pattern bindings");
    }

    FullscreenPassDesc desc;
    desc.debugName = "TestPattern";
    desc.pixelShader.path = "passes/TestPattern.slang";
    desc.pixelShader.entryPoint = "testPatternPixel";
    desc.pixelShader.stage = ShaderStage::Pixel;
    desc.bindingLayouts = {bindingLayout};
    CORDITE_TRY_ASSIGN(FullscreenPass pass, FullscreenPass::create(device, shaders, std::move(desc)));

    return TestPatternPass(std::move(constantBuffer), std::move(bindingSet), std::move(pass));
}

void TestPatternPass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                           const TestPatternParameters& parameters) {
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr, "draw needs a command list and a framebuffer");
    if (commandList == nullptr || framebuffer == nullptr) {
        return;
    }

    const nvrhi::FramebufferInfoEx& info = framebuffer->getFramebufferInfo();

    TestPatternConstants constants{};
    constants.tint = Vec4(parameters.tint, 1.0f);
    constants.resolution = Vec2(static_cast<float>(info.width), static_cast<float>(info.height));
    constants.time = parameters.timeSeconds;
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    const std::array<nvrhi::IBindingSet*, 1> bindingSets = {m_bindingSet.Get()};
    m_pass.draw(commandList, framebuffer, bindingSets);
}

} // namespace cordite
