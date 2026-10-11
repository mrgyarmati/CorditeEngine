#pragma once

/// @file
/// Draws Dear ImGui's output with NVRHI. Internal to the ui module: application code
/// talks to UiSystem, which owns one of these.
///
/// It implements ImGui's renderer backend contract: it creates, updates and destroys the
/// textures ImGui asks for (the font atlas grows on demand), and turns each ImDrawCmd
/// into a scissored, textured draw call.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto renderer, UiRenderer::create(device, shaders));
/// ImGui::Render();
/// renderer->render(commandList, framebuffer, ImGui::GetDrawData());
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/renderer/ShaderManager.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {

class UiRenderer {
public:
    /// Registers the renderer with the current ImGui context, which must exist.
    [[nodiscard]] static Result<std::unique_ptr<UiRenderer>> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Releases the textures ImGui owns. The ImGui context must still exist.
    ~UiRenderer();
    CORDITE_NON_MOVABLE(UiRenderer);

    /// Draws `drawData` over the framebuffer's first color attachment. The command list
    /// must be open.
    void render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, ImDrawData* drawData);

    /// The identifier under which ImGui widgets refer to an engine texture.
    [[nodiscard]] static ImTextureID textureId(nvrhi::ITexture* texture);

private:
    UiRenderer() = default;

    void updateTexture(nvrhi::ICommandList* commandList, ImTextureData* texture);
    void destroyTexture(ImTextureData* texture);
    void drawCommands(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, const ImDrawData& drawData);
    [[nodiscard]] bool uploadGeometry(nvrhi::ICommandList* commandList, const ImDrawData& drawData);
    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo);
    [[nodiscard]] nvrhi::IBindingSet* bindingSetFor(nvrhi::ITexture* texture);

    struct CachedBindingSet {
        nvrhi::BindingSetHandle bindingSet;
        uint64_t lastUsedFrame = 0;
    };

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_pixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::SamplerHandle m_sampler;
    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::BufferHandle m_vertexBuffer;
    nvrhi::BufferHandle m_indexBuffer;
    std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_pipelines;

    /// The textures ImGui asked for. ImGui refers to them by raw pointer.
    std::vector<nvrhi::TextureHandle> m_ownedTextures;
    std::unordered_map<nvrhi::ITexture*, CachedBindingSet> m_bindingSets;
    uint64_t m_frame = 0;

    // Reused between frames to avoid reallocating.
    std::vector<ImDrawVert> m_vertices;
    std::vector<ImDrawIdx> m_indices;
};

} // namespace cordite
