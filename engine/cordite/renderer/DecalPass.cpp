#include "cordite/renderer/DecalPass.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Frustum.h"
#include "cordite/math/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// What the vertex stage is given of a decal (DecalVertexInput in shaders/passes/Decal.slang).
struct DecalInstance {
    /// The top three rows of the matrix that places its box.
    std::array<Vec4, 3> box;
    /// Those of the inverse of its world matrix.
    std::array<Vec4, 3> decal;
    /// x: how much of it is laid on; y and z: the cosines at which a surface that turns
    /// away gets all of it, and none; w: the alpha mode of its material.
    Vec4 parameters;
};

static_assert(sizeof(DecalInstance) == 7 * sizeof(Vec4), "must match the instance attributes of Decal.slang");

/// The register space of the decal's own set: the view's is 0, the material's 1.
constexpr uint32_t kRegisterSpace = 2;

/// The top three rows of a matrix whose last row is (0, 0, 0, 1).
std::array<Vec4, 3> rowsOf(const Mat4& matrix) {
    const Mat4 transposed = glm::transpose(matrix);
    return {transposed[0], transposed[1], transposed[2]};
}

/// The matrix that draws the box of a decal whose world matrix is `worldFromDecal`. One
/// that mirrors would turn the cube inside out - its far faces would face the camera, and
/// be culled - so it is mirrored back along X, which leaves the same box.
Mat4 boxMatrix(const Mat4& worldFromDecal) {
    if (glm::determinant(Mat3(worldFromDecal)) >= 0.0f) {
        return worldFromDecal;
    }
    return worldFromDecal * glm::scale(Mat4(1.0f), Vec3(-1.0f, 1.0f, 1.0f));
}

/// The number the shader knows an alpha mode by.
float alphaModeNumber(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Opaque:
            return 0.0f;
        case AlphaMode::Masked:
            return 1.0f;
        case AlphaMode::Blended:
            return 2.0f;
    }
    return 0.0f;
}

} // namespace

DecalPass::DecalPass(Parts parts)
    : m_device(parts.device),
      m_vertexShader(std::move(parts.vertexShader)),
      m_pixelShader(std::move(parts.pixelShader)),
      m_inputLayout(std::move(parts.inputLayout)),
      m_viewLayout(std::move(parts.viewLayout)),
      m_materialLayout(std::move(parts.materialLayout)),
      m_layout(std::move(parts.layout)),
      m_instances(std::move(parts.instances)) {}

Result<DecalPass> DecalPass::create(nvrhi::IDevice* device, ShaderManager& shaders, nvrhi::IBindingLayout* viewLayout,
                                    nvrhi::IBindingLayout* materialLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr && materialLayout != nullptr,
                   "the decals need a device, the view layout and the material layout");
    if (device == nullptr || viewLayout == nullptr || materialLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument,
                         "the decals need a device, the view layout and the material layout");
    }
    CORDITE_TRY_ASSIGN(
        nvrhi::ShaderHandle vertexShader,
        shaders.get({.path = "passes/Decal.slang", .entryPoint = "decalVertex", .stage = ShaderStage::Vertex}));
    CORDITE_TRY_ASSIGN(
        nvrhi::ShaderHandle pixelShader,
        shaders.get({.path = "passes/Decal.slang", .entryPoint = "decalPixel", .stage = ShaderStage::Pixel}));

    // The position of a vertex of the cube, from buffer 0, then the decal, from buffer 1.
    // The rows the shader numbers - BOX0 to BOX2 - are one attribute of as many elements,
    // as the mesh passes have theirs (MeshPass.cpp says why).
    const auto instanceAttribute = [](const char* name, uint32_t firstRow, uint32_t rows) {
        return nvrhi::VertexAttributeDesc()
            .setName(name)
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setArraySize(rows)
            .setBufferIndex(1)
            .setOffset(firstRow * static_cast<uint32_t>(sizeof(Vec4)))
            .setElementStride(sizeof(DecalInstance))
            .setIsInstanced(true);
    };
    const std::array<nvrhi::VertexAttributeDesc, 4> attributes = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setBufferIndex(0)
            .setOffset(static_cast<uint32_t>(offsetof(MeshVertex, position)))
            .setElementStride(sizeof(MeshVertex)),
        instanceAttribute("BOX", 0, 3),
        instanceAttribute("DECAL", 3, 3),
        instanceAttribute("PARAMETERS", 6, 1),
    };
    nvrhi::InputLayoutHandle inputLayout =
        device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), vertexShader);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);

    nvrhi::BufferHandle instances = device->createBuffer(nvrhi::BufferDesc()
                                                             .setByteSize(sizeof(DecalInstance) * kMaxDecals)
                                                             .setIsVertexBuffer(true)
                                                             .setInitialState(nvrhi::ResourceStates::VertexBuffer)
                                                             .setKeepInitialState(true)
                                                             .setDebugName("Decal instances"));
    if (!inputLayout || !layout || !instances) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the decals");
    }
    return DecalPass(Parts{.device = device,
                           .vertexShader = std::move(vertexShader),
                           .pixelShader = std::move(pixelShader),
                           .inputLayout = std::move(inputLayout),
                           .viewLayout = viewLayout,
                           .materialLayout = materialLayout,
                           .layout = std::move(layout),
                           .instances = std::move(instances)});
}

nvrhi::IGraphicsPipeline* DecalPass::pipelineFor(const nvrhi::FramebufferInfo& framebuffer, bool writesNormals) {
    for (const Pipeline& made : m_pipelines) {
        if (made.framebuffer == framebuffer && made.writesNormals == writesNormals) {
            return made.pipeline;
        }
    }
    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = m_pixelShader;
    desc.bindingLayouts = {m_viewLayout, m_materialLayout, m_layout};
    // The box from inside - its back faces - so that a camera inside it sees it too; and
    // over everything, since what it lands on decides each pixel.
    desc.renderState.rasterState.frontCounterClockwise = true;
    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front;
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.depthWriteEnable = false;
    // Over the color and the numbers by how much of the decal there is, keeping what alpha
    // holds there (the lighting channels, the occlusion); the normal as the shader makes
    // it, or not at all; light added.
    const nvrhi::BlendState::RenderTarget over = nvrhi::BlendState::RenderTarget()
                                                     .enableBlend()
                                                     .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                                                     .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                                                     .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
                                                     .setDestBlendAlpha(nvrhi::BlendFactor::One);
    desc.renderState.blendState.targets[static_cast<size_t>(GBufferTarget::BaseColor)] = over;
    desc.renderState.blendState.targets[static_cast<size_t>(GBufferTarget::Normal)] =
        nvrhi::BlendState::RenderTarget().setColorWriteMask(writesNormals ? nvrhi::ColorMask::All
                                                                          : nvrhi::ColorMask(0));
    desc.renderState.blendState.targets[static_cast<size_t>(GBufferTarget::Parameters)] = over;
    desc.renderState.blendState.targets[static_cast<size_t>(GBufferTarget::Emissive)] =
        nvrhi::BlendState::RenderTarget()
            .enableBlend()
            .setSrcBlend(nvrhi::BlendFactor::One)
            .setDestBlend(nvrhi::BlendFactor::One)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
    const nvrhi::GraphicsPipelineHandle pipeline = m_device->createGraphicsPipeline(desc, framebuffer);
    if (!pipeline) {
        return nullptr;
    }
    m_pipelines.push_back({.framebuffer = framebuffer, .writesNormals = writesNormals, .pipeline = pipeline});
    return pipeline;
}

void DecalPass::prepare(RenderResources& resources, std::span<const SceneDecal> decals) {
    if (decals.empty()) {
        return;
    }
    // Asked for to be made now; draw() asks again, and has the answers at hand.
    static_cast<void>(resources.mesh(builtinMeshId(BuiltinMesh::Cube)));
    for (const SceneDecal& decal : decals.first(std::min<size_t>(decals.size(), kMaxDecals))) {
        // What cannot be loaded is reported here, once, and left out by draw().
        static_cast<void>(resources.loadedMaterial(decal.material));
    }
}

DecalDrawStatistics DecalPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                    nvrhi::IBindingSet* viewBindings, const CameraView& view,
                                    RenderResources& resources, std::span<const SceneDecal> decals) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "decals need a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return {};
    }

    // What is drawn: the decals whose box is in view and whose material can be used.
    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection);
    m_drawn.clear();
    for (const SceneDecal& decal : decals.first(std::min<size_t>(decals.size(), kMaxDecals))) {
        if (!frustum.intersects(decal.bounds)) {
            continue;
        }
        if (const GpuMaterial* material = resources.loadedMaterial(decal.material)) {
            m_drawn.push_back({.decal = &decal, .material = material});
        }
    }
    if (m_drawn.empty()) {
        return {};
    }

    const auto fail = [this](std::string_view why) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no decals are drawn: {}", why);
            m_reportedFailure = true;
        }
        return DecalDrawStatistics{};
    };
    const GpuMesh* cube = resources.mesh(builtinMeshId(BuiltinMesh::Cube));
    if (cube == nullptr || cube->submeshes.empty()) {
        return fail("the cube they are drawn with is not there");
    }
    if (const Status prepared = targets.prepareDecalNormals(m_device); !prepared) {
        return fail(prepared.error().describe());
    }
    const nvrhi::FramebufferInfoEx& info = targets.decalFramebuffer()->getFramebufferInfo();
    nvrhi::IGraphicsPipeline* bending = pipelineFor(info, true);
    nvrhi::IGraphicsPipeline* keeping = pipelineFor(info, false);
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, targets.decalNormals()),
                        nvrhi::BindingSetItem::Texture_SRV(1, targets.depth())};
    const nvrhi::BindingSetHandle set = m_device->createBindingSet(setDesc, m_layout);
    if (bending == nullptr || keeping == nullptr || !set) {
        return fail("their pipelines or their textures cannot be made");
    }

    const auto count = static_cast<uint32_t>(m_drawn.size());
    std::vector<DecalInstance> instances(count);
    for (uint32_t index = 0; index < count; ++index) {
        const SceneDecal& decal = *m_drawn[index].decal;
        const float full = std::cos(glm::radians(decal.fadeAngle - kDecalFadeWidth));
        const float none = std::cos(glm::radians(decal.fadeAngle));
        instances[index] = {.box = rowsOf(boxMatrix(decal.worldFromDecal)),
                            .decal = rowsOf(decal.decalFromWorld),
                            .parameters =
                                Vec4(decal.opacity, full, none, alphaModeNumber(m_drawn[index].material->alphaMode))};
    }
    commandList->writeBuffer(m_instances, instances.data(), sizeof(DecalInstance) * instances.size());

    // The normals as the base pass left them, which every decal reads while the target is
    // drawn into; and again wherever a decal comes to a part of the world that one since
    // then has bent them in.
    DecalDrawStatistics statistics{.decals = count};
    const auto copyNormals = [&] {
        ++statistics.normalCopies;
        commandList->copyTexture(targets.decalNormals(), nvrhi::TextureSlice(), targets.gbuffer(GBufferTarget::Normal),
                                 nvrhi::TextureSlice());
        m_bent.clear();
    };
    copyNormals();
    const Submesh& box = cube->submeshes.front();
    for (uint32_t index = 0; index < count; ++index) {
        const DrawnDecal& drawn = m_drawn[index];
        if (std::ranges::any_of(m_bent, [&](const Aabb& bent) { return bent.intersects(drawn.decal->bounds); })) {
            copyNormals();
        }
        nvrhi::GraphicsState state;
        state.pipeline = drawn.material->hasNormalTexture ? bending : keeping;
        state.framebuffer = targets.decalFramebuffer();
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
        state.bindings = {viewBindings, drawn.material->bindings.Get(), set.Get()};
        state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(cube->vertexBuffer).setSlot(0).setOffset(0),
                               nvrhi::VertexBufferBinding().setBuffer(m_instances).setSlot(1).setOffset(0)};
        state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(cube->indexBuffer).setFormat(cube->indexFormat);
        commandList->setGraphicsState(state);
        commandList->drawIndexed(nvrhi::DrawArguments()
                                     .setVertexCount(box.indexCount)
                                     .setInstanceCount(1)
                                     .setStartIndexLocation(box.firstIndex)
                                     .setStartInstanceLocation(index));
        if (drawn.material->hasNormalTexture) {
            m_bent.push_back(drawn.decal->bounds);
        }
    }
    return statistics;
}

} // namespace cordite
