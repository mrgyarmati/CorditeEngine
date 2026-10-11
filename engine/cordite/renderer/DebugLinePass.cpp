#include "cordite/renderer/DebugLinePass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace cordite {
namespace {

/// Mirrors DebugLineConstants in shaders/passes/DebugLines.slang.
struct DebugLineConstants {
    Mat4 viewProjection;
    Vec3 fadeOrigin;
    float fadeStart;
    float fadeEnd;
    float depthBias;
    std::array<float, 2> padding;
};

static_assert(sizeof(DebugLineConstants) == 96, "must match the cbuffer layout in DebugLines.slang");
static_assert(sizeof(DebugVertex) == 16, "must match the vertex attributes declared below");

/// Room for a grid, a few gizmos and some markers before the buffer has to grow.
constexpr size_t kInitialVertexCapacity = 8192;

} // namespace

Result<DebugLinePass> DebugLinePass::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    DebugLinePass pass;
    pass.m_device = device;

    CORDITE_TRY_ASSIGN(pass.m_vertexShader, shaders.get({.path = "passes/DebugLines.slang",
                                                         .entryPoint = "debugLineVertex",
                                                         .stage = ShaderStage::Vertex}));
    CORDITE_TRY_ASSIGN(
        pass.m_pixelShader,
        shaders.get({.path = "passes/DebugLines.slang", .entryPoint = "debugLinePixel", .stage = ShaderStage::Pixel}));

    // The order matches the members of DebugLineVertex in the shader.
    const std::array<nvrhi::VertexAttributeDesc, 2> attributes = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(offsetof(DebugVertex, position))
            .setElementStride(sizeof(DebugVertex)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setOffset(offsetof(DebugVertex, color))
            .setElementStride(sizeof(DebugVertex)),
    };
    pass.m_inputLayout =
        device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), pass.m_vertexShader);

    // Each draw() call rewrites the constants, so the buffer needs as many versions as
    // there can be calls in flight.
    constexpr uint32_t kMaxVersions = 64;
    pass.m_constantBuffer = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(DebugLineConstants), "DebugLineConstants", kMaxVersions));
    if (!pass.m_inputLayout || !pass.m_constantBuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the debug line resources");
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, pass.m_constantBuffer)};
    if (!nvrhi::utils::CreateBindingSetAndLayout(device, nvrhi::ShaderType::All, 0, bindingSetDesc,
                                                 pass.m_bindingLayout, pass.m_bindingSet)) {
        return makeError(ErrorCode::DeviceError, "cannot create the debug line bindings");
    }
    return pass;
}

nvrhi::IGraphicsPipeline* DebugLinePass::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo, bool depthTest) {
    for (const Pipeline& pipeline : m_pipelines) {
        if (pipeline.depthTest == depthTest && pipeline.framebufferInfo == framebufferInfo) {
            return pipeline.handle;
        }
    }

    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::LineList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = m_pixelShader;
    desc.bindingLayouts = {m_bindingLayout};
    desc.renderState.rasterState.setCullNone();

    // Reversed-Z: nearer fragments have larger depth values.
    nvrhi::DepthStencilState& depthState = desc.renderState.depthStencilState;
    depthState.depthTestEnable = depthTest;
    depthState.depthWriteEnable = false;
    depthState.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

    nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
    blend.blendEnable = true;
    blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;

    const nvrhi::GraphicsPipelineHandle handle = m_device->createGraphicsPipeline(desc, framebufferInfo);
    CORDITE_VERIFY(handle, "cannot create the debug line pipeline");
    m_pipelines.push_back({framebufferInfo, depthTest, handle});
    return handle;
}

void DebugLinePass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                         std::span<const DebugVertex> vertices, const DebugLineParameters& parameters) {
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr, "draw needs a command list and a framebuffer");
    CORDITE_ASSERT(vertices.size() % 2 == 0, "debug lines need two vertices each, got {} vertices", vertices.size());
    if (commandList == nullptr || framebuffer == nullptr || vertices.size() < 2) {
        return;
    }
    // Draw whole lines only, whatever was passed in.
    const size_t vertexCount = vertices.size() - vertices.size() % 2;

    const size_t capacity =
        m_vertexBuffer ? static_cast<size_t>(m_vertexBuffer->getDesc().byteSize) / sizeof(DebugVertex) : 0;
    if (vertexCount > capacity) {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize =
            std::max({kInitialVertexCapacity, vertexCount, capacity + capacity / 2}) * sizeof(DebugVertex);
        bufferDesc.debugName = "Debug line vertices";
        bufferDesc.isVertexBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        bufferDesc.keepInitialState = true;
        // The previous buffer stays alive until the draws that used it have finished.
        m_vertexBuffer = m_device->createBuffer(bufferDesc);
        if (!m_vertexBuffer) {
            CORDITE_LOG_ERROR("Renderer", "cannot allocate a buffer for {} debug line vertices", vertexCount);
            return;
        }
    }

    const nvrhi::FramebufferInfoEx& framebufferInfo = framebuffer->getFramebufferInfo();
    const bool hasDepth = framebufferInfo.depthFormat != nvrhi::Format::UNKNOWN;
    nvrhi::IGraphicsPipeline* pipeline = pipelineFor(framebufferInfo, parameters.depthTest && hasDepth);
    if (pipeline == nullptr) {
        return;
    }

    DebugLineConstants constants{};
    constants.viewProjection = parameters.viewProjection;
    constants.fadeOrigin = parameters.fadeOrigin;
    constants.fadeStart = parameters.fadeStart;
    // smoothstep is undefined when its edges coincide; keep the end beyond the start.
    constants.fadeEnd = std::max(parameters.fadeEnd, parameters.fadeStart * 1.0001f + 1.0e-4f);
    constants.depthBias = std::max(parameters.depthBias, 0.0f);
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    commandList->writeBuffer(m_vertexBuffer, vertices.data(), vertexCount * sizeof(DebugVertex));

    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = framebuffer;
    state.viewport.addViewportAndScissorRect(framebufferInfo.getViewport());
    state.bindings = {m_bindingSet.Get()};
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_vertexBuffer).setSlot(0).setOffset(0)};

    commandList->setGraphicsState(state);
    commandList->draw(nvrhi::DrawArguments().setVertexCount(static_cast<uint32_t>(vertexCount)));
}

} // namespace cordite
