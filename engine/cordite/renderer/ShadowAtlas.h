#pragma once

/// @file
/// The texture all shadow maps of a view are drawn into, and the pass that draws them.
///
/// @code
/// CORDITE_TRY_ASSIGN(ShadowAtlas atlas,
///                    ShadowAtlas::create(device, shaders, resources.materialLayout(), palettes.layout()));
///
/// CORDITE_TRY(atlas.resize(settings.atlasSize));
/// atlas.clear(commandList);
/// for (const ShadowView& shadow : plan.views) {
///     drawList.build(commandList, castersSeenBy(shadow), Vec3(0.0f), kWorldForward);
///     atlas.draw(commandList, shadow, drawList.solidBatches(), drawList.instanceBuffer(), palettes.bindings());
/// }
/// // Afterwards the passes of the view read atlas.texture() through ViewBindings.
/// @endcode
///
/// A shadow map holds depth only, 0 at the light and 1 at the far end of what it sees.
/// It is drawn by the same vertex shader as everything else, with the light's view in
/// place of the camera's; solid surfaces need no pixel shader, masked ones cut their
/// holes. What lets other things show through casts no shadow and is not drawn.

#include "cordite/core/Result.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/MeshPass.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/renderer/ShadowPlan.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>

namespace cordite {

class ShadowAtlas {
public:
    /// Loads the shaders and makes an atlas of the smallest size, so that there is
    /// always a texture to bind. `materialLayout` is RenderResources::materialLayout();
    /// the device and the layout must outlive the atlas.
    [[nodiscard]] static Result<ShadowAtlas> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                    nvrhi::IBindingLayout* materialLayout,
                                                    nvrhi::IBindingLayout* jointLayout);

    /// The atlas: a depth texture, size() texels a side. Replaced by resize().
    [[nodiscard]] nvrhi::ITexture* texture() const { return m_texture; }

    [[nodiscard]] uint32_t size() const { return m_size; }

    /// The memory the atlas takes on the GPU, in bytes: four for each texel.
    [[nodiscard]] uint64_t byteSize() const { return static_cast<uint64_t>(m_size) * m_size * 4; }

    /// Makes the atlas `size` texels a side, between kMinShadowTileSize and 16384. Does
    /// nothing when it is that size already; otherwise the texture is replaced, and the
    /// old one stays alive until the GPU has finished with it. On failure the atlas is
    /// as it was.
    [[nodiscard]] Status resize(uint32_t size);

    /// Empties the atlas: every texel as far away as a light can see. The command list
    /// must be open.
    void clear(nvrhi::ICommandList* commandList);

    /// Draws `batches` as `view` sees them, into its tile. The command list must be
    /// open; `instanceBuffer` is the one of the MeshDrawList the batches come from. The
    /// tile must lie inside the atlas.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, const ShadowView& view,
                            std::span<const MeshBatch> batches, nvrhi::IBuffer* instanceBuffer,
                            nvrhi::IBindingSet* jointBindings);

private:
    ShadowAtlas(nvrhi::IDevice* device, nvrhi::BufferHandle constants, nvrhi::BindingSetHandle bindings, MeshPass pass);

    nvrhi::IDevice* m_device;
    /// The view of the light whose map is being drawn, for the vertex shader.
    nvrhi::BufferHandle m_constants;
    nvrhi::BindingSetHandle m_bindings;
    MeshPass m_pass;
    nvrhi::TextureHandle m_texture;
    nvrhi::FramebufferHandle m_framebuffer;
    uint32_t m_size = 0;
};

} // namespace cordite
