#include "cordite/renderer/ShadowAtlas.h"

#include "cordite/core/Assert.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <utility>

namespace cordite {
namespace {

/// The largest texture dimension every supported GPU can create.
constexpr uint32_t kMaxAtlasSize = 16384;

/// The constants are rewritten for every shadow map of every view that is still on its
/// way through the GPU, so that a view which has constants of its own also has them
/// for all the shadow maps it can have. A megabyte.
constexpr uint32_t kMaxVersions = kMaxViewsInFlight * kMaxShadowViews;

/// Depth as a shadow map holds it: 0 at the light, 1 as far as it sees.
constexpr float kFarthest = 1.0f;

} // namespace

ShadowAtlas::ShadowAtlas(nvrhi::IDevice* device, nvrhi::BufferHandle constants, nvrhi::BindingSetHandle bindings,
                         MeshPass pass)
    : m_device(device), m_constants(std::move(constants)), m_bindings(std::move(bindings)), m_pass(std::move(pass)) {}

Result<ShadowAtlas> ShadowAtlas::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                        nvrhi::IBindingLayout* materialLayout, nvrhi::IBindingLayout* jointLayout) {
    CORDITE_ASSERT(device != nullptr && materialLayout != nullptr && jointLayout != nullptr,
                   "a shadow atlas needs a device and the layouts of materials and joints");

    // A view of its own for the pass: the constants alone. The binding set the passes
    // of a camera's view use holds the atlas as something to read, which it cannot be
    // while it is being drawn into.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
    const nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
    nvrhi::BufferHandle constants = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ViewConstants), "Shadow view constants", kMaxVersions));
    if (!layout || !constants) {
        return makeError(ErrorCode::DeviceError, "cannot create the constants of the shadow pass");
    }
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, constants)};
    nvrhi::BindingSetHandle bindings = device->createBindingSet(setDesc, layout);
    if (!bindings) {
        return makeError(ErrorCode::DeviceError, "cannot create the bindings of the shadow pass");
    }

    MeshPassDesc passDesc;
    passDesc.debugName = "Shadow pass";
    // Depth only: a solid surface needs no pixel shader, a masked one cuts its holes.
    passDesc.maskedPixelShader = {
        .path = "passes/MeshDepth.slang", .entryPoint = "meshDepthMaskedPixel", .stage = ShaderStage::Pixel};
    passDesc.depth = MeshPassDepth::TestAndWrite;
    passDesc.conventionalDepth = true;
    CORDITE_TRY_ASSIGN(MeshPass pass,
                       MeshPass::create(device, shaders,
                                        {.view = layout, .material = materialLayout, .joints = jointLayout},
                                        std::move(passDesc)));

    ShadowAtlas atlas(device, std::move(constants), std::move(bindings), std::move(pass));
    CORDITE_TRY(atlas.resize(kMinShadowTileSize));
    return atlas;
}

Status ShadowAtlas::resize(uint32_t size) {
    if (size == m_size && m_texture) {
        return {};
    }
    if (size < kMinShadowTileSize || size > kMaxAtlasSize) {
        return makeError(ErrorCode::InvalidArgument, "a shadow atlas is between {} and {} texels a side, not {}",
                         kMinShadowTileSize, kMaxAtlasSize, size);
    }

    nvrhi::TextureDesc desc;
    desc.width = size;
    desc.height = size;
    desc.format = nvrhi::Format::D32;
    desc.debugName = "Shadow atlas";
    desc.isRenderTarget = true;
    // Read by the passes of a view most of the time; drawn into at the start of it.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.clearValue = nvrhi::Color(kFarthest);
    desc.useClearValue = true;
    nvrhi::TextureHandle texture = m_device->createTexture(desc);
    if (!texture) {
        return makeError(ErrorCode::DeviceError, "cannot create a shadow atlas of {0}x{0} texels", size);
    }
    nvrhi::FramebufferHandle framebuffer =
        m_device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(texture));
    if (!framebuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the framebuffer of the shadow atlas");
    }
    m_texture = std::move(texture);
    m_framebuffer = std::move(framebuffer);
    m_size = size;
    return {};
}

void ShadowAtlas::clear(nvrhi::ICommandList* commandList) {
    CORDITE_ASSERT(commandList != nullptr && m_texture != nullptr, "clear needs a command list and an atlas");
    if (commandList == nullptr || m_texture == nullptr) {
        return;
    }
    commandList->clearDepthStencilTexture(m_texture, nvrhi::AllSubresources, true, kFarthest, false, 0);
}

MeshPassStatistics ShadowAtlas::draw(nvrhi::ICommandList* commandList, const ShadowView& view,
                                     std::span<const MeshBatch> batches, nvrhi::IBuffer* instanceBuffer,
                                     nvrhi::IBindingSet* jointBindings) {
    if (batches.empty()) {
        return {};
    }
    // Written so that no sum can wrap around.
    const bool fits = view.tileSize > 0 && view.tileSize <= m_size && view.tileOrigin.x <= m_size - view.tileSize &&
                      view.tileOrigin.y <= m_size - view.tileSize;
    CORDITE_ASSERT(commandList != nullptr && fits, "a shadow map needs a command list and a tile inside the atlas");
    if (commandList == nullptr || !fits) {
        return {};
    }

    // The vertex shader needs to know where the light looks from; nothing else of a
    // view matters to a pass that writes depth alone.
    ViewConstants constants{};
    constants.viewProjection = view.viewProjection;
    constants.previousViewProjection = view.viewProjection;
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    const auto left = static_cast<float>(view.tileOrigin.x);
    const auto top = static_cast<float>(view.tileOrigin.y);
    const auto side = static_cast<float>(view.tileSize);
    return m_pass.draw(commandList, m_framebuffer, batches,
                       {.view = m_bindings, .instances = instanceBuffer, .joints = jointBindings},
                       nvrhi::Viewport(left, left + side, top, top + side, 0.0f, 1.0f));
}

} // namespace cordite
