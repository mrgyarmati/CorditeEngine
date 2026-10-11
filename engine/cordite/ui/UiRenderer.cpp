#include "cordite/ui/UiRenderer.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Math.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>

namespace cordite {
namespace {

/// Mirrors UiConstants in shaders/ui/ImGui.slang.
struct UiConstants {
    Vec2 positionScale;
    Vec2 positionOffset;
};

static_assert(sizeof(UiConstants) == 16, "must match the cbuffer layout in ImGui.slang");
static_assert(sizeof(ImDrawIdx) == 2, "the index buffer is bound as 16-bit");

/// Buffers start big enough for a typical editor frame and grow by half when exceeded.
constexpr size_t kInitialVertexCapacity = 16384;
constexpr size_t kInitialIndexCapacity = 32768;

size_t grownCapacity(size_t current, size_t required) {
    return std::max(required, current + current / 2);
}

/// What ImGui calls when a widget asks the renderer to restore its default state after
/// a custom draw callback. render() sets the full state for every draw, so there is
/// nothing to restore.
void resetRenderState(const ImDrawList* /*drawList*/, const ImDrawCmd* /*command*/) {}

nvrhi::ITexture* textureFromId(ImTextureID id) {
    // An ImTextureID is an integer wide enough for a pointer; this is the documented way
    // to carry a renderer's texture object through ImGui.
    return std::bit_cast<nvrhi::ITexture*>(static_cast<uintptr_t>(id));
}

} // namespace

ImTextureID UiRenderer::textureId(nvrhi::ITexture* texture) {
    return static_cast<ImTextureID>(std::bit_cast<uintptr_t>(texture));
}

Result<std::unique_ptr<UiRenderer>> UiRenderer::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    CORDITE_ASSERT(ImGui::GetCurrentContext() != nullptr, "the ImGui context must exist before its renderer");

    std::unique_ptr<UiRenderer> renderer(new UiRenderer());
    renderer->m_device = device;

    CORDITE_TRY_ASSIGN(renderer->m_vertexShader,
                       shaders.get({.path = "ui/ImGui.slang", .entryPoint = "uiVertex", .stage = ShaderStage::Vertex}));
    CORDITE_TRY_ASSIGN(renderer->m_pixelShader,
                       shaders.get({.path = "ui/ImGui.slang", .entryPoint = "uiPixel", .stage = ShaderStage::Pixel}));

    // The order matches the members of UiVertex in the shader.
    const std::array<nvrhi::VertexAttributeDesc, 3> attributes = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(ImDrawVert, pos))
            .setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(ImDrawVert, uv))
            .setElementStride(sizeof(ImDrawVert)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setOffset(offsetof(ImDrawVert, col))
            .setElementStride(sizeof(ImDrawVert)),
    };
    renderer->m_inputLayout = device->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()),
                                                        renderer->m_vertexShader);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    renderer->m_bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    renderer->m_sampler = device->createSampler(samplerDesc);

    constexpr uint32_t kMaxVersions = 16;
    renderer->m_constantBuffer = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(UiConstants), "UiConstants", kMaxVersions));

    if (!renderer->m_inputLayout || !renderer->m_bindingLayout || !renderer->m_sampler || !renderer->m_constantBuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the user interface rendering resources");
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "cordite_nvrhi";
    // Draw commands may address vertices beyond what a 16-bit index reaches, and textures
    // (the font atlas above all) are created and updated on request.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = &resetRenderState;
    return renderer;
}

UiRenderer::~UiRenderer() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        // A texture shared with another context is still in use there.
        if (texture->RefCount == 1 && texture->Status != ImTextureStatus_Destroyed) {
            destroyTexture(texture);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = nullptr;
}

void UiRenderer::updateTexture(nvrhi::ICommandList* commandList, ImTextureData* texture) {
    if (texture->Status == ImTextureStatus_WantCreate) {
        CORDITE_ASSERT(texture->Format == ImTextureFormat_RGBA32, "ImGui textures are expected to be RGBA");

        nvrhi::TextureDesc desc;
        desc.width = static_cast<uint32_t>(texture->Width);
        desc.height = static_cast<uint32_t>(texture->Height);
        desc.format = nvrhi::Format::RGBA8_UNORM;
        desc.debugName = "ImGui texture";
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;

        nvrhi::TextureHandle created = m_device->createTexture(desc);
        if (!created) {
            // Leave the request pending: ImGui asks again next frame, and what refers to
            // the texture until then is skipped by render().
            CORDITE_LOG_ERROR("Ui", "cannot create a {}x{} interface texture", texture->Width, texture->Height);
            return;
        }
        texture->SetTexID(textureId(created.Get()));
        m_ownedTextures.push_back(std::move(created));
    }

    if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
        // NVRHI uploads whole mip levels, so an update re-sends the texture rather than
        // only the changed rectangle. Updates happen when new glyphs are first shown.
        const auto rowPitch = static_cast<size_t>(texture->GetPitch());
        commandList->writeTexture(textureFromId(texture->GetTexID()), 0, 0, texture->GetPixels(), rowPitch);
        texture->SetStatus(ImTextureStatus_OK);
    }

    // ImGui waits for the texture to be unused before it asks; one more frame makes sure
    // no draw command recorded earlier in this frame still refers to it.
    if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 0) {
        destroyTexture(texture);
    }
}

void UiRenderer::destroyTexture(ImTextureData* texture) {
    nvrhi::ITexture* handle = textureFromId(texture->GetTexID());
    m_bindingSets.erase(handle);
    std::erase_if(m_ownedTextures, [handle](const nvrhi::TextureHandle& owned) { return owned.Get() == handle; });

    texture->SetTexID(ImTextureID_Invalid);
    texture->SetStatus(ImTextureStatus_Destroyed);
}

bool UiRenderer::uploadGeometry(nvrhi::ICommandList* commandList, const ImDrawData& drawData) {
    m_vertices.clear();
    m_indices.clear();
    m_vertices.reserve(static_cast<size_t>(drawData.TotalVtxCount));
    m_indices.reserve(static_cast<size_t>(drawData.TotalIdxCount));
    for (const ImDrawList* drawList : drawData.CmdLists) {
        m_vertices.insert(m_vertices.end(), drawList->VtxBuffer.begin(), drawList->VtxBuffer.end());
        m_indices.insert(m_indices.end(), drawList->IdxBuffer.begin(), drawList->IdxBuffer.end());
    }

    const auto ensureCapacity = [this](nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize,
                                       size_t initialCapacity, bool isVertexBuffer) {
        const size_t currentCapacity = buffer ? static_cast<size_t>(buffer->getDesc().byteSize) / elementSize : 0;
        if (elementCount <= currentCapacity) {
            return;
        }

        nvrhi::BufferDesc desc;
        desc.byteSize = std::max(initialCapacity, grownCapacity(currentCapacity, elementCount)) * elementSize;
        desc.debugName = isVertexBuffer ? "ImGui vertices" : "ImGui indices";
        desc.isVertexBuffer = isVertexBuffer;
        desc.isIndexBuffer = !isVertexBuffer;
        desc.initialState = isVertexBuffer ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
        desc.keepInitialState = true;
        // The previous buffer stays alive until the frames that used it have finished.
        buffer = m_device->createBuffer(desc);
    };
    ensureCapacity(m_vertexBuffer, m_vertices.size(), sizeof(ImDrawVert), kInitialVertexCapacity, true);
    ensureCapacity(m_indexBuffer, m_indices.size(), sizeof(ImDrawIdx), kInitialIndexCapacity, false);
    if (!m_vertexBuffer || !m_indexBuffer) {
        CORDITE_LOG_ERROR("Ui", "cannot allocate buffers for {} interface vertices", m_vertices.size());
        return false;
    }

    commandList->writeBuffer(m_vertexBuffer, m_vertices.data(), m_vertices.size() * sizeof(ImDrawVert));
    commandList->writeBuffer(m_indexBuffer, m_indices.data(), m_indices.size() * sizeof(ImDrawIdx));
    return true;
}

nvrhi::IGraphicsPipeline* UiRenderer::pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo) {
    for (const auto& [info, pipeline] : m_pipelines) {
        if (info == framebufferInfo) {
            return pipeline;
        }
    }

    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = m_inputLayout;
    desc.VS = m_vertexShader;
    desc.PS = m_pixelShader;
    desc.bindingLayouts = {m_bindingLayout};
    desc.renderState.rasterState.setCullNone();
    desc.renderState.rasterState.scissorEnable = true;
    desc.renderState.depthStencilState.disableDepthTest().disableDepthWrite();

    // Standard "over" blending. The alpha channel accumulates coverage, so a transparent
    // target ends up with correct alpha.
    nvrhi::BlendState::RenderTarget& blend = desc.renderState.blendState.targets[0];
    blend.blendEnable = true;
    blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;

    const nvrhi::GraphicsPipelineHandle pipeline = m_device->createGraphicsPipeline(desc, framebufferInfo);
    CORDITE_VERIFY(pipeline, "cannot create the user interface pipeline");
    m_pipelines.emplace_back(framebufferInfo, pipeline);
    return pipeline;
}

nvrhi::IBindingSet* UiRenderer::bindingSetFor(nvrhi::ITexture* texture) {
    CachedBindingSet& cached = m_bindingSets[texture];
    if (!cached.bindingSet) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
            nvrhi::BindingSetItem::Texture_SRV(0, texture),
            nvrhi::BindingSetItem::Sampler(0, m_sampler),
        };
        cached.bindingSet = m_device->createBindingSet(desc, m_bindingLayout);
    }
    cached.lastUsedFrame = m_frame;
    return cached.bindingSet;
}

void UiRenderer::render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, ImDrawData* drawData) {
    CORDITE_ASSERT(commandList != nullptr && framebuffer != nullptr && drawData != nullptr,
                   "rendering the interface needs a command list, a framebuffer and draw data");
    if (commandList == nullptr || framebuffer == nullptr || drawData == nullptr) {
        return;
    }
    ++m_frame;

    // Texture requests are honored even when nothing is visible, or they would pile up.
    if (drawData->Textures != nullptr) {
        for (ImTextureData* texture : *drawData->Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                updateTexture(commandList, texture);
            }
        }
    }

    drawCommands(commandList, framebuffer, *drawData);

    // A binding set keeps its texture alive. Dropping the ones this frame did not use lets
    // a replaced texture (a resized viewport, say) go as soon as the GPU is done with it;
    // the command list holds on to everything it recorded until then.
    std::erase_if(m_bindingSets, [this](const auto& entry) { return entry.second.lastUsedFrame != m_frame; });
}

void UiRenderer::drawCommands(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                              const ImDrawData& drawData) {
    const Vec2 displaySize(drawData.DisplaySize.x, drawData.DisplaySize.y);
    const Vec2 framebufferScale(drawData.FramebufferScale.x, drawData.FramebufferScale.y);
    const Vec2 targetSize = displaySize * framebufferScale;
    if (targetSize.x < 1.0f || targetSize.y < 1.0f || drawData.TotalVtxCount == 0) {
        return;
    }
    if (!uploadGeometry(commandList, drawData)) {
        return;
    }

    // Clip space has +Y up and UI coordinates +Y down, hence the negative Y scale.
    UiConstants constants{};
    constants.positionScale = Vec2(2.0f / displaySize.x, -2.0f / displaySize.y);
    constants.positionOffset = Vec2(-1.0f - drawData.DisplayPos.x * constants.positionScale.x,
                                    1.0f - drawData.DisplayPos.y * constants.positionScale.y);
    commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    nvrhi::GraphicsState state;
    state.pipeline = pipelineFor(framebuffer->getFramebufferInfo());
    state.framebuffer = framebuffer;
    state.vertexBuffers = {nvrhi::VertexBufferBinding().setBuffer(m_vertexBuffer).setSlot(0).setOffset(0)};
    state.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(m_indexBuffer).setFormat(nvrhi::Format::R16_UINT);
    if (state.pipeline == nullptr) {
        return;
    }

    const nvrhi::Viewport viewport(targetSize.x, targetSize.y);
    uint32_t vertexBase = 0;
    uint32_t indexBase = 0;
    for (const ImDrawList* drawList : drawData.CmdLists) {
        for (const ImDrawCmd& command : drawList->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                command.UserCallback(drawList, &command);
                continue;
            }

            // Clip rectangles are in UI coordinates; scissors are in target pixels.
            const Vec2 clipMin = glm::max(
                (Vec2(command.ClipRect.x, command.ClipRect.y) - Vec2(drawData.DisplayPos.x, drawData.DisplayPos.y)) *
                    framebufferScale,
                Vec2(0.0f));
            const Vec2 clipMax = glm::min(
                (Vec2(command.ClipRect.z, command.ClipRect.w) - Vec2(drawData.DisplayPos.x, drawData.DisplayPos.y)) *
                    framebufferScale,
                targetSize);
            nvrhi::ITexture* texture = textureFromId(command.GetTexID());
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y || texture == nullptr) {
                continue;
            }

            state.viewport = nvrhi::ViewportState().addViewport(viewport).addScissorRect(
                nvrhi::Rect(static_cast<int>(clipMin.x), static_cast<int>(clipMax.x), static_cast<int>(clipMin.y),
                            static_cast<int>(clipMax.y)));
            state.bindings = {bindingSetFor(texture)};
            commandList->setGraphicsState(state);
            commandList->drawIndexed(nvrhi::DrawArguments()
                                         .setVertexCount(command.ElemCount)
                                         .setStartIndexLocation(indexBase + command.IdxOffset)
                                         .setStartVertexLocation(vertexBase + command.VtxOffset));
        }
        vertexBase += static_cast<uint32_t>(drawList->VtxBuffer.Size);
        indexBase += static_cast<uint32_t>(drawList->IdxBuffer.Size);
    }
}

} // namespace cordite
