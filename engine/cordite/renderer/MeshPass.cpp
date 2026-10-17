#include "cordite/renderer/MeshPass.h"

#include "cordite/core/Assert.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The one vertex shader of every mesh pass; see the comment in the file.
constexpr const char* kVertexShaderPath = "passes/MeshVertex.slang";
constexpr const char* kVertexShaderEntryPoint = "meshVertex";

} // namespace

Result<MeshPass> MeshPass::create(nvrhi::IDevice* device, ShaderManager& shaders, const MeshPassLayouts& layouts,
                                  MeshPassDesc desc) {
    CORDITE_ASSERT(device != nullptr && layouts.view != nullptr && layouts.material != nullptr &&
                       layouts.joints != nullptr,
                   "a mesh pass needs a device and the layouts of its three binding sets");
    if (device == nullptr || layouts.view == nullptr || layouts.material == nullptr || layouts.joints == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the pass '{}' was given no device or not all of its layouts",
                         desc.debugName);
    }

    MeshPass pass;
    pass.m_device = device;
    pass.m_viewLayout = layouts.view;
    pass.m_materialLayout = layouts.material;
    pass.m_jointLayout = layouts.joints;

    // A pass that works out where its vertices were has every shader compiled for it:
    // the varyings of its stages are not those of the others.
    const auto withMotion = [&](ShaderRequest request) {
        if (desc.motion) {
            request.defines.push_back(ShaderDefine{.name = "MOTION"});
        }
        return request;
    };
    CORDITE_TRY_ASSIGN(pass.m_vertexShader, shaders.get(withMotion({.path = kVertexShaderPath,
                                                                    .entryPoint = kVertexShaderEntryPoint,
                                                                    .stage = ShaderStage::Vertex})));
    CORDITE_TRY_ASSIGN(pass.m_skinnedVertexShader, shaders.get(withMotion({.path = kVertexShaderPath,
                                                                           .entryPoint = kVertexShaderEntryPoint,
                                                                           .stage = ShaderStage::Vertex,
                                                                           .defines = {{"SKINNED", "1"}}})));
    if (!desc.pixelShader.path.empty()) {
        CORDITE_TRY_ASSIGN(pass.m_pixelShader, shaders.get(withMotion(desc.pixelShader)));
    }
    if (!desc.maskedPixelShader.path.empty()) {
        CORDITE_TRY_ASSIGN(pass.m_maskedPixelShader, shaders.get(withMotion(desc.maskedPixelShader)));
    }

    // The order matches the members of MeshVertexInput in the shader: the vertex first,
    // from buffer 0, then the instance, from buffer 1 - with MOTION, where it was as well -
    // then, for a skinned mesh, how the vertex follows its joints, from buffer 2.
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
    const auto skinAttribute = [](const char* name, nvrhi::Format format, size_t offset) {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(format)
            .setBufferIndex(2)
            .setOffset(static_cast<uint32_t>(offset))
            .setElementStride(sizeof(MeshSkinVertex));
    };
    std::vector<nvrhi::VertexAttributeDesc> attributes = {
        vertexAttribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, position)),
        vertexAttribute("NORMAL", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, normal)),
        vertexAttribute("TANGENT", nvrhi::Format::RGBA32_FLOAT, offsetof(MeshVertex, tangent)),
        vertexAttribute("TEXCOORD0", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv0)),
        vertexAttribute("TEXCOORD1", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv1)),
        instanceAttribute("WORLD0", 0),
        instanceAttribute("WORLD1", 1),
        instanceAttribute("WORLD2", 2),
        instanceAttribute("PARAMETERS", 3),
    };
    if (desc.motion) {
        attributes.push_back(instanceAttribute("PREVIOUSWORLD0", 4));
        attributes.push_back(instanceAttribute("PREVIOUSWORLD1", 5));
        attributes.push_back(instanceAttribute("PREVIOUSWORLD2", 6));
    }
    // A mesh that is not skinned has all but the two of the skin, which come last.
    const auto rigidCount = static_cast<uint32_t>(attributes.size());
    attributes.push_back(skinAttribute("JOINTS", nvrhi::Format::RGBA16_UINT, offsetof(MeshSkinVertex, joints)));
    attributes.push_back(skinAttribute("WEIGHTS", nvrhi::Format::RGBA32_FLOAT, offsetof(MeshSkinVertex, weights)));
    pass.m_inputLayout = device->createInputLayout(attributes.data(), rigidCount, pass.m_vertexShader);
    pass.m_skinnedInputLayout = device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()),
                                                          pass.m_skinnedVertexShader);
    if (!pass.m_inputLayout || !pass.m_skinnedInputLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the vertex layout of the pass '{}'", desc.debugName);
    }

    pass.m_desc = std::move(desc);
    return pass;
}

nvrhi::IGraphicsPipeline* MeshPass::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo, uint32_t variant) {
    // Whether a batch is blended is the pass's business, not the pipeline's, and so is
    // "masked" where masked materials get no shader of their own.
    uint32_t relevant = kMeshVariantDoubleSided | kMeshVariantMirrored | kMeshVariantSkinned;
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
    const bool skinned = (key & kMeshVariantSkinned) != 0;
    desc.inputLayout = skinned ? m_skinnedInputLayout : m_inputLayout;
    desc.VS = skinned ? m_skinnedVertexShader : m_vertexShader;
    desc.PS = (key & kMeshVariantMasked) != 0 ? m_maskedPixelShader : m_pixelShader;
    desc.bindingLayouts = {m_viewLayout, m_materialLayout};
    if (skinned) {
        desc.bindingLayouts.push_back(m_jointLayout);
    }

    // Front faces wind counter-clockwise; mirroring an instance reverses its triangles.
    nvrhi::RasterState& raster = desc.renderState.rasterState;
    raster.frontCounterClockwise = (key & kMeshVariantMirrored) == 0;
    raster.cullMode = (key & kMeshVariantDoubleSided) != 0 || m_desc.wireframe ? nvrhi::RasterCullMode::None
                                                                               : nvrhi::RasterCullMode::Back;
    raster.fillMode = m_desc.wireframe ? nvrhi::RasterFillMode::Wireframe : nvrhi::RasterFillMode::Solid;

    // Reversed depth: nearer fragments have larger values - except in a shadow map.
    nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
    depth.depthTestEnable = m_desc.depth != MeshPassDepth::None;
    depth.depthWriteEnable = m_desc.depth == MeshPassDepth::TestAndWrite;
    if (m_desc.depth == MeshPassDepth::Equal) {
        depth.depthFunc = nvrhi::ComparisonFunc::Equal;
    } else {
        depth.depthFunc =
            m_desc.conventionalDepth ? nvrhi::ComparisonFunc::LessOrEqual : nvrhi::ComparisonFunc::GreaterOrEqual;
    }

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
                                  std::span<const MeshBatch> batches, const MeshPassBindings& bindings) {
    CORDITE_ASSERT(framebuffer != nullptr || batches.empty(), "the pass '{}' needs a framebuffer", m_desc.debugName);
    if (framebuffer == nullptr) {
        return {};
    }
    return draw(commandList, framebuffer, batches, bindings, framebuffer->getFramebufferInfo().getViewport());
}

MeshPassStatistics MeshPass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                                  std::span<const MeshBatch> batches, const MeshPassBindings& bindings,
                                  const nvrhi::Viewport& viewport) {
    MeshPassStatistics statistics;
    if (batches.empty()) {
        return statistics;
    }
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr && bindings.view != nullptr &&
                       bindings.instances != nullptr,
                   "the pass '{}' needs a command list, a framebuffer, the view and the instances", m_desc.debugName);
    if (commandList == nullptr || framebuffer == nullptr || bindings.view == nullptr || bindings.instances == nullptr) {
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
        const GpuMesh& mesh = *batch.mesh;
        const bool skinned = (batch.variant & kMeshVariantSkinned) != 0;
        // What a skinned batch cannot be drawn without. A list marks a batch skinned
        // only for a mesh that is; the joint matrices are the caller's to give.
        const bool canSkin = mesh.skinBuffer != nullptr && bindings.joints != nullptr;
        CORDITE_ASSERT(!skinned || canSkin, "the pass '{}' was given a skinned batch without its joints",
                       m_desc.debugName);
        if (skinned && !canSkin) {
            continue;
        }
        nvrhi::IGraphicsPipeline* pipeline = pipelineFor(framebufferInfo, batch.variant);
        if (pipeline == nullptr) {
            continue;
        }
        const IndexRange range = mesh.rangeOf(batch.submesh, batch.lod);

        nvrhi::GraphicsState state;
        state.pipeline = pipeline;
        state.framebuffer = framebuffer;
        state.viewport.addViewportAndScissorRect(viewport);
        state.bindings = {bindings.view, batch.material->bindings.Get()};
        state.vertexBuffers = {
            nvrhi::VertexBufferBinding().setBuffer(mesh.vertexBuffer).setSlot(0).setOffset(0),
            nvrhi::VertexBufferBinding().setBuffer(bindings.instances).setSlot(1).setOffset(0),
        };
        if (skinned) {
            state.bindings.push_back(bindings.joints);
            state.vertexBuffers.push_back(
                nvrhi::VertexBufferBinding().setBuffer(mesh.skinBuffer).setSlot(2).setOffset(0));
        }
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(mesh.indexBuffer).setFormat(mesh.indexFormat);
        commandList->setGraphicsState(state);

        commandList->drawIndexed(nvrhi::DrawArguments()
                                     .setVertexCount(range.indexCount)
                                     .setInstanceCount(batch.instanceCount)
                                     .setStartIndexLocation(range.firstIndex)
                                     .setStartInstanceLocation(batch.firstInstance));

        ++statistics.drawCalls;
        statistics.instances += batch.instanceCount;
        statistics.triangles += static_cast<uint64_t>(range.indexCount / 3) * batch.instanceCount;
    }
    return statistics;
}

} // namespace cordite
