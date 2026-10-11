#include "cordite/renderer/MeshPass.h"

#include "cordite/core/Assert.h"

#include <array>
#include <cstddef>
#include <utility>

namespace cordite {
namespace {

/// The one vertex shader of every mesh pass; see the comment in the file.
constexpr const char* kVertexShaderPath = "passes/MeshVertex.slang";
constexpr const char* kVertexShaderEntryPoint = "meshVertex";

} // namespace

Result<MeshPass> MeshPass::create(nvrhi::IDevice* device, ShaderManager& shaders, nvrhi::IBindingLayout* viewLayout,
                                  nvrhi::IBindingLayout* materialLayout, MeshPassDesc desc) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr && materialLayout != nullptr,
                   "a mesh pass needs a device and the layouts of both binding sets");

    MeshPass pass;
    pass.m_device = device;
    pass.m_viewLayout = viewLayout;
    pass.m_materialLayout = materialLayout;

    CORDITE_TRY_ASSIGN(
        pass.m_vertexShader,
        shaders.get({.path = kVertexShaderPath, .entryPoint = kVertexShaderEntryPoint, .stage = ShaderStage::Vertex}));
    if (!desc.pixelShader.path.empty()) {
        CORDITE_TRY_ASSIGN(pass.m_pixelShader, shaders.get(desc.pixelShader));
    }
    if (!desc.maskedPixelShader.path.empty()) {
        CORDITE_TRY_ASSIGN(pass.m_maskedPixelShader, shaders.get(desc.maskedPixelShader));
    }

    // The order matches the members of MeshVertexInput in the shader: the vertex first,
    // from buffer 0, then the instance, from buffer 1.
    const auto vertexAttribute = [](const char* name, nvrhi::Format format, size_t offset) {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(format)
            .setBufferIndex(0)
            .setOffset(static_cast<uint32_t>(offset))
            .setElementStride(sizeof(MeshVertex));
    };
    const auto instanceAttribute = [](const char* name, uint32_t row) {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setBufferIndex(1)
            .setOffset(row * static_cast<uint32_t>(sizeof(Vec4)))
            .setElementStride(kMeshInstanceStride)
            .setIsInstanced(true);
    };
    const std::array<nvrhi::VertexAttributeDesc, 8> attributes = {
        vertexAttribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, position)),
        vertexAttribute("NORMAL", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, normal)),
        vertexAttribute("TANGENT", nvrhi::Format::RGBA32_FLOAT, offsetof(MeshVertex, tangent)),
        vertexAttribute("TEXCOORD0", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv0)),
        vertexAttribute("TEXCOORD1", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv1)),
        instanceAttribute("WORLD0", 0),
        instanceAttribute("WORLD1", 1),
        instanceAttribute("WORLD2", 2),
    };
    pass.m_inputLayout =
        device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), pass.m_vertexShader);
    if (!pass.m_inputLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the vertex layout of the pass '{}'", desc.debugName);
    }

    pass.m_desc = std::move(desc);
    return pass;
}

nvrhi::IGraphicsPipeline* MeshPass::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo, uint32_t variant) {
    // Whether a batch is blended is the pass's business, not the pipeline's, and so is
    // "masked" where masked materials get no shader of their own.
    uint32_t relevant = kMeshVariantDoubleSided | kMeshVariantMirrored;
    if (m_maskedPixelShader) {
        relevant |= kMeshVariantMasked;
    }
    const uint32_t key = variant & relevant;

    for (const Pipeline& pipeline : m_pipelines) {
        if (pipeline.variant == key && pipeline.framebufferInfo == framebufferInfo) {
            return pipeline.handle;
        }
    }

    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = (key & kMeshVariantMasked) != 0 ? m_maskedPixelShader : m_pixelShader;
    desc.bindingLayouts = {m_viewLayout, m_materialLayout};

    // Front faces wind counter-clockwise; mirroring an instance reverses its triangles.
    nvrhi::RasterState& raster = desc.renderState.rasterState;
    raster.frontCounterClockwise = (key & kMeshVariantMirrored) == 0;
    raster.cullMode = (key & kMeshVariantDoubleSided) != 0 || m_desc.wireframe ? nvrhi::RasterCullMode::None
                                                                               : nvrhi::RasterCullMode::Back;
    raster.fillMode = m_desc.wireframe ? nvrhi::RasterFillMode::Wireframe : nvrhi::RasterFillMode::Solid;

    // Reversed depth: nearer fragments have larger values.
    nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
    depth.depthTestEnable = m_desc.depth != MeshPassDepth::None;
    depth.depthWriteEnable = m_desc.depth == MeshPassDepth::TestAndWrite;
    depth.depthFunc =
        m_desc.depth == MeshPassDepth::Equal ? nvrhi::ComparisonFunc::Equal : nvrhi::ComparisonFunc::GreaterOrEqual;

    if (m_desc.blend != MeshPassBlend::None) {
        nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
        blend.blendEnable = true;
        const bool additive = m_desc.blend == MeshPassBlend::Additive;
        blend.srcBlend = additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::SrcAlpha;
        blend.destBlend = additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha = nvrhi::BlendFactor::One;
        blend.destBlendAlpha = additive ? nvrhi::BlendFactor::One : nvrhi::BlendFactor::InvSrcAlpha;
    }

    const nvrhi::GraphicsPipelineHandle handle = m_device->createGraphicsPipeline(desc, framebufferInfo);
    CORDITE_VERIFY(handle, "cannot create a pipeline of the pass '{}' (variant {})", m_desc.debugName, key);
    // Remembered even when it failed, so that the failure is reported once.
    m_pipelines.push_back({framebufferInfo, key, handle});
    return handle;
}

MeshPassStatistics MeshPass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                                  std::span<const MeshBatch> batches, nvrhi::IBindingSet* viewBindings,
                                  nvrhi::IBuffer* instanceBuffer) {
    MeshPassStatistics statistics;
    if (batches.empty()) {
        return statistics;
    }
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr && viewBindings != nullptr &&
                       instanceBuffer != nullptr,
                   "the pass '{}' needs a command list, a framebuffer, the view and the instances", m_desc.debugName);
    if (commandList == nullptr || framebuffer == nullptr || viewBindings == nullptr || instanceBuffer == nullptr) {
        return statistics;
    }
    const nvrhi::FramebufferInfoEx& framebufferInfo = framebuffer->getFramebufferInfo();
    const bool needsDepth = m_desc.depth != MeshPassDepth::None;
    CORDITE_ASSERT(!needsDepth || framebufferInfo.depthFormat != nvrhi::Format::UNKNOWN,
                   "the pass '{}' needs a depth buffer", m_desc.debugName);
    if (needsDepth && framebufferInfo.depthFormat == nvrhi::Format::UNKNOWN) {
        return statistics;
    }

    for (const MeshBatch& batch : batches) {
        nvrhi::IGraphicsPipeline* pipeline = pipelineFor(framebufferInfo, batch.variant);
        if (pipeline == nullptr) {
            continue;
        }
        const GpuMesh& mesh = *batch.mesh;
        const Submesh& submesh = mesh.submeshes[batch.submesh];

        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffer;
        state.viewport.addViewportAndScissorRect(framebufferInfo.getViewport());
        state.bindings = {viewBindings, batch.material->bindings.Get()};
        state.vertexBuffers = {
            nvrhi::VertexBufferBinding().setBuffer(mesh.vertexBuffer).setSlot(0).setOffset(0),
            nvrhi::VertexBufferBinding().setBuffer(instanceBuffer).setSlot(1).setOffset(0),
        };
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(mesh.indexBuffer).setFormat(mesh.indexFormat);
        commandList->setGraphicsState(state);

        commandList->drawIndexed(nvrhi::DrawArguments()
                                     .setVertexCount(submesh.indexCount)
                                     .setInstanceCount(batch.instanceCount)
                                     .setStartIndexLocation(submesh.firstIndex)
                                     .setStartInstanceLocation(batch.firstInstance));

        ++statistics.drawCalls;
        statistics.instances += batch.instanceCount;
        statistics.triangles += static_cast<uint64_t>(submesh.indexCount / 3) * batch.instanceCount;
    }
    return statistics;
}

} // namespace cordite
