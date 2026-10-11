#include "cordite/renderer/MeshForwardPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <tuple>

namespace cordite {
namespace {

/// Mirrors ViewConstants in shaders/passes/MeshForward.slang.
struct ViewConstants {
    Mat4 viewProjection;
    Vec3 cameraPosition;
    /// 1 for a camera without perspective, 0 for one with.
    float orthographic;
    Vec3 cameraForward;
    float padding0;
    Vec3 keyLightDirection;
    float padding1;
    Vec3 keyLightColor;
    float padding2;
    Vec3 skyColor;
    float padding3;
    Vec3 groundColor;
    float padding4;
};

static_assert(sizeof(ViewConstants) == 160, "must match the cbuffer layout in MeshForward.slang");

/// What differs between two draws of the same mesh: the rows of the 3x4 matrix that
/// places it in the world. Read by the vertex shader as per-instance attributes.
struct InstanceData {
    Vec4 row0;
    Vec4 row1;
    Vec4 row2;
};

static_assert(sizeof(InstanceData) == 48, "must match the instance attributes declared below");

/// Room for a small scene before the instance buffer has to grow.
constexpr size_t kInitialInstanceCapacity = 1024;

// A pipeline variant is a combination of these.
constexpr uint32_t kVariantMasked = 1u << 0;
constexpr uint32_t kVariantBlended = 1u << 1;
constexpr uint32_t kVariantDoubleSided = 1u << 2;
/// The instance is mirrored, which turns its triangles the other way round.
constexpr uint32_t kVariantMirrored = 1u << 3;

uint32_t variantOf(const MeshDraw& draw) {
    uint32_t variant = 0;
    if (draw.material->alphaMode == AlphaMode::Masked) {
        variant |= kVariantMasked;
    } else if (draw.material->alphaMode == AlphaMode::Blended) {
        variant |= kVariantBlended;
    }
    if (draw.material->doubleSided) {
        variant |= kVariantDoubleSided;
    }
    if (glm::determinant(Mat3(draw.localToWorld)) < 0.0f) {
        variant |= kVariantMirrored;
    }
    return variant;
}

bool isDrawable(const MeshDraw& draw) {
    return draw.mesh != nullptr && draw.material != nullptr && draw.submesh < draw.mesh->submeshes.size() &&
           draw.mesh->submeshes[draw.submesh].indexCount > 0 && draw.mesh->indexBuffer != nullptr;
}

/// A draw with what it is sorted and batched by.
struct SortedDraw {
    const MeshDraw* draw = nullptr;
    uint32_t variant = 0;
    /// Distance along the view direction, for blended surfaces.
    float depth = 0.0f;

    /// Draws that agree in this can share a draw call.
    [[nodiscard]] auto batchKey() const {
        return std::make_tuple(variant, draw->material->serial, draw->mesh->serial, draw->submesh);
    }
};

} // namespace

Result<MeshForwardPass> MeshForwardPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                nvrhi::IBindingLayout* materialLayout) {
    CORDITE_ASSERT(device != nullptr && materialLayout != nullptr, "the pass needs a device and the material layout");

    MeshForwardPass pass;
    pass.m_device = device;
    pass.m_materialLayout = materialLayout;

    constexpr const char* kShaderPath = "passes/MeshForward.slang";
    CORDITE_TRY_ASSIGN(
        pass.m_vertexShader,
        shaders.get({.path = kShaderPath, .entryPoint = "meshForwardVertex", .stage = ShaderStage::Vertex}));
    CORDITE_TRY_ASSIGN(
        pass.m_pixelShader,
        shaders.get({.path = kShaderPath, .entryPoint = "meshForwardPixel", .stage = ShaderStage::Pixel}));
    CORDITE_TRY_ASSIGN(pass.m_alphaTestPixelShader, shaders.get({.path = kShaderPath,
                                                                 .entryPoint = "meshForwardPixel",
                                                                 .stage = ShaderStage::Pixel,
                                                                 .defines = {{"ALPHA_TEST", "1"}}}));

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
    const auto instanceAttribute = [](const char* name, size_t offset) {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setBufferIndex(1)
            .setOffset(static_cast<uint32_t>(offset))
            .setElementStride(sizeof(InstanceData))
            .setIsInstanced(true);
    };
    const std::array<nvrhi::VertexAttributeDesc, 8> attributes = {
        vertexAttribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, position)),
        vertexAttribute("NORMAL", nvrhi::Format::RGB32_FLOAT, offsetof(MeshVertex, normal)),
        vertexAttribute("TANGENT", nvrhi::Format::RGBA32_FLOAT, offsetof(MeshVertex, tangent)),
        vertexAttribute("TEXCOORD0", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv0)),
        vertexAttribute("TEXCOORD1", nvrhi::Format::RG32_FLOAT, offsetof(MeshVertex, uv1)),
        instanceAttribute("WORLD0", offsetof(InstanceData, row0)),
        instanceAttribute("WORLD1", offsetof(InstanceData, row1)),
        instanceAttribute("WORLD2", offsetof(InstanceData, row2)),
    };
    pass.m_inputLayout =
        device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), pass.m_vertexShader);

    // Rewritten by every draw() call, so it needs as many versions as there can be
    // calls in flight: one per view per frame, and a few frames.
    constexpr uint32_t kMaxVersions = 64;
    pass.m_viewConstants = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ViewConstants), "ViewConstants", kMaxVersions));

    // The view's bindings are register space 0; the material's are space 1.
    nvrhi::BindingLayoutDesc viewLayoutDesc;
    viewLayoutDesc.visibility = nvrhi::ShaderType::All;
    viewLayoutDesc.registerSpace = 0;
    viewLayoutDesc.registerSpaceIsDescriptorSet = true;
    viewLayoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
    pass.m_viewLayout = device->createBindingLayout(viewLayoutDesc);
    if (!pass.m_inputLayout || !pass.m_viewConstants || !pass.m_viewLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the mesh pass resources");
    }

    nvrhi::BindingSetDesc viewSetDesc;
    viewSetDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, pass.m_viewConstants)};
    pass.m_viewBindings = device->createBindingSet(viewSetDesc, pass.m_viewLayout);
    if (!pass.m_viewBindings) {
        return makeError(ErrorCode::DeviceError, "cannot create the mesh pass bindings");
    }
    return pass;
}

nvrhi::IGraphicsPipeline* MeshForwardPass::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo,
                                                       uint32_t variant) {
    for (const Pipeline& pipeline : m_pipelines) {
        if (pipeline.variant == variant && pipeline.framebufferInfo == framebufferInfo) {
            return pipeline.handle;
        }
    }

    const bool blended = (variant & kVariantBlended) != 0;

    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = (variant & kVariantMasked) != 0 ? m_alphaTestPixelShader : m_pixelShader;
    desc.bindingLayouts = {m_viewLayout, m_materialLayout};

    // Front faces wind counter-clockwise; mirroring an instance reverses its triangles.
    nvrhi::RasterState& raster = desc.renderState.rasterState;
    raster.frontCounterClockwise = (variant & kVariantMirrored) == 0;
    raster.cullMode = (variant & kVariantDoubleSided) != 0 ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;

    // Reversed-Z: nearer fragments have larger depth values. Blended surfaces are hidden
    // by what is in front of them but do not hide what is behind.
    nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
    depth.depthTestEnable = true;
    depth.depthWriteEnable = !blended;
    depth.depthFunc = nvrhi::ComparisonFunc::GreaterOrEqual;

    if (blended) {
        nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
        blend.blendEnable = true;
        blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
        blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
        blend.srcBlendAlpha = nvrhi::BlendFactor::One;
        blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    }

    const nvrhi::GraphicsPipelineHandle handle = m_device->createGraphicsPipeline(desc, framebufferInfo);
    CORDITE_VERIFY(handle, "cannot create a mesh pipeline (variant {})", variant);
    m_pipelines.push_back({framebufferInfo, variant, handle});
    return handle;
}

MeshPassStatistics MeshForwardPass::draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                                         std::span<const MeshDraw> draws, const MeshPassView& view) {
    MeshPassStatistics statistics;
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr, "draw needs a command list and a framebuffer");
    if (commandList == nullptr || framebuffer == nullptr) {
        return statistics;
    }
    const nvrhi::FramebufferInfoEx& framebufferInfo = framebuffer->getFramebufferInfo();
    CORDITE_ASSERT(framebufferInfo.depthFormat != nvrhi::Format::UNKNOWN, "meshes need a depth buffer");
    if (framebufferInfo.depthFormat == nvrhi::Format::UNKNOWN) {
        return statistics;
    }

    std::vector<SortedDraw> sorted;
    sorted.reserve(draws.size());
    for (const MeshDraw& draw : draws) {
        if (isDrawable(draw)) {
            // A placement that is not a number must not reach the sort: an order that
            // contradicts itself is undefined behaviour there.
            const float depth = glm::dot(Vec3(draw.localToWorld[3]) - view.cameraPosition, view.cameraForward);
            sorted.push_back({&draw, variantOf(draw), std::isfinite(depth) ? depth : 0.0f});
        }
    }
    if (sorted.empty()) {
        return statistics;
    }

    // Solid surfaces first, grouped so that equal draws are neighbours and can be
    // instanced; then blended ones from far to near. The sort is stable, so that draws
    // that compare equal keep the order they were given in - every run draws alike.
    std::ranges::stable_sort(sorted, [](const SortedDraw& a, const SortedDraw& b) {
        const bool aBlended = (a.variant & kVariantBlended) != 0;
        const bool bBlended = (b.variant & kVariantBlended) != 0;
        if (aBlended != bBlended) {
            return !aBlended;
        }
        return aBlended ? a.depth > b.depth : a.batchKey() < b.batchKey();
    });

    std::vector<InstanceData> instances;
    instances.reserve(sorted.size());
    for (const SortedDraw& entry : sorted) {
        const Mat4& matrix = entry.draw->localToWorld;
        // glm stores columns; the shader wants rows.
        instances.push_back({Vec4(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]),
                             Vec4(matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]),
                             Vec4(matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2])});
    }

    const size_t capacity =
        m_instanceBuffer ? static_cast<size_t>(m_instanceBuffer->getDesc().byteSize) / sizeof(InstanceData) : 0;
    if (instances.size() > capacity) {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize =
            std::max({kInitialInstanceCapacity, instances.size(), capacity + capacity / 2}) * sizeof(InstanceData);
        bufferDesc.debugName = "Mesh instances";
        bufferDesc.isVertexBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        bufferDesc.keepInitialState = true;
        // The previous buffer stays alive until the draws that used it have finished.
        m_instanceBuffer = m_device->createBuffer(bufferDesc);
        if (!m_instanceBuffer) {
            CORDITE_LOG_ERROR("Renderer", "cannot allocate a buffer for {} mesh instances", instances.size());
            return statistics;
        }
    }

    ViewConstants constants{};
    constants.viewProjection = view.viewProjection;
    constants.cameraPosition = view.cameraPosition;
    constants.orthographic = view.orthographic ? 1.0f : 0.0f;
    constants.cameraForward = normalizeOr(view.cameraForward, kWorldForward);
    constants.keyLightDirection = normalizeOr(view.keyLightDirection, kWorldUp);
    constants.keyLightColor = view.keyLightColor;
    constants.skyColor = view.skyColor;
    constants.groundColor = view.groundColor;
    commandList->writeBuffer(m_viewConstants, &constants, sizeof(constants));
    commandList->writeBuffer(m_instanceBuffer, instances.data(), instances.size() * sizeof(InstanceData));

    for (size_t first = 0; first < sorted.size();) {
        const SortedDraw& entry = sorted[first];
        // Equal draws that follow each other share a draw call. Among solid surfaces
        // that is all equal draws; blended ones stay in their order of distance.
        size_t end = first + 1;
        while (end < sorted.size() && sorted[end].batchKey() == entry.batchKey()) {
            ++end;
        }

        nvrhi::IGraphicsPipeline* pipeline = pipelineFor(framebufferInfo, entry.variant);
        if (pipeline != nullptr) {
            const GpuMesh& mesh = *entry.draw->mesh;
            const Submesh& submesh = mesh.submeshes[entry.draw->submesh];

            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(framebufferInfo.getViewport());
            state.bindings = {m_viewBindings.Get(), entry.draw->material->bindings.Get()};
            state.vertexBuffers = {
                nvrhi::VertexBufferBinding().setBuffer(mesh.vertexBuffer).setSlot(0).setOffset(0),
                nvrhi::VertexBufferBinding().setBuffer(m_instanceBuffer).setSlot(1).setOffset(0),
            };
            state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(mesh.indexBuffer).setFormat(mesh.indexFormat);
            commandList->setGraphicsState(state);

            const auto instanceCount = static_cast<uint32_t>(end - first);
            commandList->drawIndexed(nvrhi::DrawArguments()
                                         .setVertexCount(submesh.indexCount)
                                         .setInstanceCount(instanceCount)
                                         .setStartIndexLocation(submesh.firstIndex)
                                         .setStartInstanceLocation(static_cast<uint32_t>(first)));

            ++statistics.drawCalls;
            statistics.instances += instanceCount;
            statistics.triangles += static_cast<uint64_t>(submesh.indexCount / 3) * instanceCount;
        }
        first = end;
    }
    return statistics;
}

} // namespace cordite
