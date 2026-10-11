#pragma once

/// @file
/// The GPU's copies of meshes, textures and materials.
///
/// The asset manager loads assets into memory; RenderResources turns them into buffers,
/// textures and binding sets the first time something is drawn with them, and keeps
/// those for as long as the assets do not change.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto resources, RenderResources::create(device, assets));
///
/// resources->beginFrame();
/// const GpuMesh* mesh = resources->mesh(renderer.mesh);                 // null: nothing to draw
/// const GpuMaterial& material = resources->material(renderer.materials[0]);
/// resources->flushUploads();                                            // before the draws are executed
///
/// drawIndexed(mesh->vertexBuffer, mesh->indexBuffer, material.bindings);
/// @endcode
///
/// Textures have the format of their images, and all their mip levels. An image of
/// floating-point numbers - light, from an `.hdr` file - is kept as half floats, which
/// every GPU can filter and which take half the memory; what is more than a half float
/// holds (65504) is cut off there.
///
/// An asset that cannot be loaded is reported in the log once, not once per frame: a
/// mesh is then left out and a material or texture replaced by the default, so a broken
/// asset never stops a scene from rendering.
///
/// Not thread-safe; use from the render thread.

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Mipmaps.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

struct GpuMesh {
    /// MeshVertex after MeshVertex.
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    /// R16_UINT for a mesh of up to 65535 vertices, otherwise R32_UINT.
    nvrhi::Format indexFormat = nvrhi::Format::R32_UINT;
    std::vector<Submesh> submeshes;
    /// Encloses the mesh, in its own space.
    Aabb bounds;
    /// Counts up with every mesh created. Passes sort by it rather than by address, so
    /// that two runs of a program issue their draw calls in the same order.
    uint32_t serial = 0;
};

struct GpuMaterial {
    /// The material's constants, textures and sampler, in RenderResources::materialLayout().
    nvrhi::BindingSetHandle bindings;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;
    /// Counts up with every material created; see GpuMesh::serial.
    uint32_t serial = 0;
};

struct RenderResourcesDesc {
    /// A texture larger than this on either side is uploaded without its largest mip
    /// levels, which bounds what one texture can cost in memory. Zero disables the limit.
    uint32_t maxTextureDimension = 4096;

    /// How strongly textures are filtered where they are seen at a slant; 1 turns
    /// anisotropic filtering off, 16 is the most GPUs offer.
    float maxAnisotropy = 8.0f;
};

/// What is on the GPU at the moment.
struct RenderResourceStatistics {
    uint32_t meshCount = 0;
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    /// Bytes of vertex and index buffers.
    uint64_t meshBytes = 0;
    /// Bytes of texture data, all mip levels included.
    uint64_t textureBytes = 0;
};

class RenderResources {
public:
    /// Both the device and the asset manager must outlive the object.
    [[nodiscard]] static Result<std::unique_ptr<RenderResources>> create(nvrhi::IDevice* device, AssetManager& assets,
                                                                         const RenderResourcesDesc& desc = {});
    ~RenderResources();
    CORDITE_NON_MOVABLE(RenderResources);

    /// Call once per frame before asking for anything. When the asset manager has been
    /// refreshed since the last call, everything is dropped and rebuilt on demand.
    void beginFrame();

    /// The mesh for an asset id, or null when the id is nil or the mesh cannot be
    /// loaded. The pointer is valid until the next beginFrame().
    [[nodiscard]] const GpuMesh* mesh(const Uuid& id);

    /// The material for an asset id; the default material when the id is nil or the
    /// material cannot be loaded. The reference is valid until the next beginFrame().
    [[nodiscard]] const GpuMaterial& material(const Uuid& id);

    /// The texture of an image asset as the numbers it holds, with no color space
    /// undone: for what is not a material's, such as a lookup table of colors. Null
    /// when the id is nil or the image cannot be loaded, which is reported once. The
    /// pointer is valid until the next beginFrame().
    [[nodiscard]] nvrhi::ITexture* dataTexture(const Uuid& id) { return texture(id, ColorSpace::Linear, nullptr); }

    /// The texture of an image asset as colors, for what is not a material's, such as
    /// an environment: an image of 8-bit numbers is taken as sRGB, one of floating-point
    /// numbers as the light it holds. Null when the id is nil or the image cannot be
    /// loaded, which is reported once. The pointer is valid until the next beginFrame().
    [[nodiscard]] nvrhi::ITexture* colorTexture(const Uuid& id) { return texture(id, ColorSpace::Srgb, nullptr); }

    /// Sends everything created since the last call to the GPU. Must happen before a
    /// command list that draws with it is executed.
    void flushUploads();

    /// The layout of GpuMaterial::bindings: register space 1, with the constants at b0,
    /// the textures at t0 to t4 and the sampler at s0 (see shaders/materials/Material.slang).
    [[nodiscard]] nvrhi::IBindingLayout* materialLayout() const { return m_materialLayout; }

    [[nodiscard]] const RenderResourceStatistics& statistics() const { return m_statistics; }

private:
    RenderResources(nvrhi::IDevice* device, AssetManager& assets, const RenderResourcesDesc& desc);

    struct TextureKey {
        Uuid id;
        ColorSpace colorSpace = ColorSpace::Linear;
        bool operator==(const TextureKey&) const = default;
    };

    struct TextureKeyHash {
        size_t operator()(const TextureKey& key) const noexcept;
    };

    [[nodiscard]] Status initialize();
    void clear();
    [[nodiscard]] nvrhi::ICommandList* uploadCommandList();
    [[nodiscard]] Result<GpuMesh> createMesh(const MeshData& mesh);
    [[nodiscard]] Result<nvrhi::TextureHandle> createTexture(const Image& image, ColorSpace colorSpace,
                                                             const std::string& name);
    [[nodiscard]] Result<GpuMaterial> createMaterial(const MaterialData& material, const std::string& name);
    /// The texture for a material slot: the asset's, or `fallback` when the id is nil or
    /// the texture cannot be loaded.
    [[nodiscard]] nvrhi::ITexture* texture(const Uuid& id, ColorSpace colorSpace, nvrhi::ITexture* fallback);
    [[nodiscard]] Result<nvrhi::ISampler*> sampler(const MaterialData& material);
    [[nodiscard]] Result<nvrhi::TextureHandle> createSolidTexture(const Vec4& color, const std::string& name);
    /// The normal map of a material that has none: one texel that leaves the surface
    /// normal exactly as it is.
    [[nodiscard]] Result<nvrhi::TextureHandle> createFlatNormalTexture();

    nvrhi::IDevice* m_device;
    AssetManager& m_assets;
    RenderResourcesDesc m_desc;
    uint64_t m_assetRevision = 0;

    nvrhi::BindingLayoutHandle m_materialLayout;
    nvrhi::CommandListHandle m_uploadCommandList;
    nvrhi::TextureHandle m_whiteTexture;
    nvrhi::TextureHandle m_flatNormalTexture;
    GpuMaterial m_defaultMaterial;

    std::unordered_map<Uuid, GpuMesh> m_meshes;
    std::unordered_map<Uuid, GpuMaterial> m_materials;
    std::unordered_map<TextureKey, nvrhi::TextureHandle, TextureKeyHash> m_textures;
    /// One per combination of wrap modes and filter that a material has asked for.
    std::unordered_map<uint32_t, nvrhi::SamplerHandle> m_samplers;
    /// Assets that could not be loaded, so that each is tried and reported only once.
    std::unordered_set<Uuid> m_failedMeshes;
    RenderResourceStatistics m_statistics;
    /// The share of the defaults that are never dropped: the fallback textures and material.
    RenderResourceStatistics m_permanentStatistics;
    uint32_t m_nextSerial = 0;
};

} // namespace cordite
