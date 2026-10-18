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
/// Textures have the format of their images, and their mip levels from the first whose
/// larger side is within the limit (setTextureLimit(), by the quality of the program) on -
/// as many levels more as the budget of all textures asks (TextureBudget.h). An image of
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
#include "cordite/assets/RenderTarget.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/renderer/TextureBudget.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

/// A coarser level of a mesh on the GPU (MeshLod): where each submesh's triangles are at
/// that level, in the index buffer of the mesh, and how far it is from the mesh.
struct GpuMeshLod {
    std::vector<IndexRange> submeshes;
    float error = 0.0f;
};

struct GpuMesh {
    /// MeshVertex after MeshVertex.
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    /// R16_UINT for a mesh of up to 65535 vertices, otherwise R32_UINT.
    nvrhi::Format indexFormat = nvrhi::Format::R32_UINT;
    /// For a skinned mesh, how each vertex follows its joints: MeshSkinVertex after
    /// MeshSkinVertex, one for every vertex. Null for a mesh that is not skinned.
    nvrhi::BufferHandle skinBuffer;
    std::vector<Submesh> submeshes;
    /// Encloses the mesh as it was modelled, in its own space.
    Aabb bounds;
    /// For a skinned mesh, jointBounds() of it: for every joint it uses, a box around
    /// what follows that joint. Its size is the number of joints a pose must have for
    /// the mesh to be drawn with it.
    std::vector<Aabb> jointBounds;
    /// Counts up with every mesh created. Passes sort by it rather than by address, so
    /// that two runs of a program issue their draw calls in the same order.
    uint32_t serial = 0;

    /// The coarser levels of the mesh, in order (MeshData::lods). Their indices follow
    /// those of the mesh in `indexBuffer`.
    std::vector<GpuMeshLod> lods;
    /// The errors of `lods`, in order: what selectLod() chooses by (LevelOfDetail.h).
    std::vector<float> lodErrors;

    [[nodiscard]] bool isSkinned() const { return skinBuffer != nullptr; }

    /// The levels a draw can ask for: the mesh itself and its coarser ones.
    [[nodiscard]] uint32_t levelCount() const { return static_cast<uint32_t>(lods.size()) + 1; }

    /// Where the triangles of submesh `submesh` are in `indexBuffer` at level `lod`: 0 is
    /// the mesh itself. A level beyond the last is the last.
    [[nodiscard]] IndexRange rangeOf(uint32_t submesh, uint32_t lod) const;
};

struct GpuMaterial {
    /// The material's constants, textures and sampler, in RenderResources::materialLayout().
    nvrhi::BindingSetHandle bindings;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool doubleSided = false;
    /// Whether the material has a normal texture. A decal of one that has none leaves the
    /// normals it lands on as they are.
    bool hasNormalTexture = false;
    /// The least roughness a texel of the material can have: its roughness - or nothing,
    /// when its metallic-roughness texture can make it less. Reflections from the screen
    /// are drawn only for a view in which something can be smooth enough to take them.
    float leastRoughness = 1.0f;
    /// Counts up with every material created; see GpuMesh::serial.
    uint32_t serial = 0;
};

struct RenderResourcesDesc {
    /// A texture larger than this on either side is uploaded without its largest mip
    /// levels, which bounds what one texture can cost in memory: the limit the resources
    /// start with (RenderResources::setTextureLimit()). Zero disables the limit.
    uint32_t maxTextureDimension = 4096;

    /// The most the textures of the assets may take together, in bytes; 0 for no budget.
    /// When the textures known so far would take more, every one of them drops the same
    /// number of levels more - the fewest at which they fit (TextureBudget.h), which the
    /// log says whenever it grows. What is read texel by texel - a lookup table of colors -
    /// is neither made smaller nor counted, and neither is what cannot be made.
    uint64_t textureBudgetBytes = 0;

    /// How strongly textures are filtered where they are seen at a slant; 1 turns
    /// anisotropic filtering off, 16 is the most GPUs offer.
    float maxAnisotropy = 8.0f;
};

/// What is on the GPU at the moment.
struct RenderResourceStatistics {
    uint32_t meshCount = 0;
    uint32_t textureCount = 0;
    uint32_t materialCount = 0;
    /// Bytes of vertex and index buffers, and of what skinned meshes have beside them.
    uint64_t meshBytes = 0;
    /// Bytes of texture data, all mip levels included.
    uint64_t textureBytes = 0;
    /// The largest side a texture keeps (RenderResources::setTextureLimit(); 0 for no
    /// limit), and how many levels more every texture drops to stay within the budget.
    uint32_t textureLimit = 0;
    uint32_t textureLevelBias = 0;
    /// The budget of all textures (RenderResourcesDesc::textureBudgetBytes; 0 for none).
    uint64_t textureBudgetBytes = 0;
};

class RenderResources {
public:
    /// Both the device and the asset manager must outlive the object.
    [[nodiscard]] static Result<std::unique_ptr<RenderResources>> create(nvrhi::IDevice* device, AssetManager& assets,
                                                                         const RenderResourcesDesc& desc = {});
    ~RenderResources();
    CORDITE_NON_MOVABLE(RenderResources);

    /// Call once per frame before asking for anything. When the asset manager has been
    /// refreshed since the last call - or the textures are to be another size: another
    /// limit, or a texture added that took them past their budget - everything is dropped
    /// and rebuilt on demand.
    void beginFrame();

    /// Counts up whenever everything is dropped to be made anew, for either reason of
    /// beginFrame(): what was made of the textures - the picture of a probe - is to be made
    /// anew as well.
    [[nodiscard]] uint64_t revision() const { return m_revision; }

    /// Has every texture keep at most `limit` texels on its larger side (0 for no limit),
    /// from the next beginFrame() on - not in the middle of a frame, while what was drawn
    /// with the textures of now may still be shown.
    void setTextureLimit(uint32_t limit) { m_requestedTextureLimit = limit; }

    /// The limit the textures keep from the next beginFrame() on (setTextureLimit()); the
    /// statistics say what the textures of now were made with.
    [[nodiscard]] uint32_t textureLimit() const { return m_requestedTextureLimit; }

    /// The mesh for an asset id, or null when the id is nil or the mesh cannot be
    /// loaded. The pointer is valid until the next beginFrame().
    [[nodiscard]] const GpuMesh* mesh(const Uuid& id);

    /// The material for an asset id; the default material when the id is nil or the
    /// material cannot be loaded. The reference is valid until the next beginFrame().
    [[nodiscard]] const GpuMaterial& material(const Uuid& id);

    /// The material for an asset id, or null when the id is nil or the material cannot be
    /// loaded - which material() reports, once: for what is nothing without its material,
    /// as a decal. The pointer is valid until the next beginFrame().
    [[nodiscard]] const GpuMaterial* loadedMaterial(const Uuid& id);

    /// The texture of an image asset as the numbers it holds, with no color space
    /// undone, at its own size whatever the limit and the budget: for what is not a
    /// material's and is read texel by texel, such as a lookup table of colors, whose
    /// structure would not survive being made smaller. Null when the id is nil or the
    /// image cannot be loaded, which is reported once. The pointer is valid until the
    /// next beginFrame().
    [[nodiscard]] nvrhi::ITexture* dataTexture(const Uuid& id) {
        return texture(id, ColorSpace::Linear, nullptr, TextureSize::AsItIs);
    }

    /// The texture of an image asset as colors, for what is not a material's, such as
    /// an environment: an image of 8-bit numbers is taken as sRGB, one of floating-point
    /// numbers as the light it holds. Null when the id is nil or the image cannot be
    /// loaded, which is reported once. The pointer is valid until the next beginFrame().
    [[nodiscard]] nvrhi::ITexture* colorTexture(const Uuid& id) { return texture(id, ColorSpace::Srgb, nullptr); }

    /// The texture of a render target asset (RenderTarget.h), which a SceneCapture draws
    /// into: as large as the asset says, in sRGB colors - a finished picture - and black
    /// until something is drawn into it. One for each asset; a material that names the
    /// asset as a color texture shows this one. Null when the id names no render target
    /// that can be loaded, which is reported once. The pointer is valid until the next
    /// beginFrame().
    [[nodiscard]] nvrhi::ITexture* renderTarget(const Uuid& id);

    /// The texture array the cookies of lights are layers of (LightCookies.h), as views
    /// bind it: layers of kLightCookieSize pixels square with all their levels, in the
    /// colors of a display - a cookie filters light by its colors - as many as cookies are
    /// in use, rounded up to a power of two, kMaxLightCookies at the most; and a single
    /// white texel while there is no cookie, which costs nothing.
    /// The pointer is valid until the next beginFrame().
    [[nodiscard]] nvrhi::ITexture* cookies() const { return m_cookies ? m_cookies.Get() : m_noCookies.Get(); }

    /// The layer of cookies() that holds the texture asset `id` as a cookie, which puts it
    /// there - brought to kLightCookieSize pixels square - the first time it is asked for.
    /// -1 for nil, and for what cannot be a cookie, which is reported once: a texture that
    /// cannot be loaded, an image of light (floating point: a cookie is a picture), and
    /// one more than the array has layers for. When the assets are refreshed, the layers
    /// are given anew.
    [[nodiscard]] int32_t cookieLayer(const Uuid& id);

    /// Sends everything created since the last call to the GPU. Must happen before a
    /// command list that draws with it is executed.
    void flushUploads();

    /// The layout of GpuMaterial::bindings: register space 1, with the constants at b0,
    /// the textures at t0 to t4 and the sampler at s0 (see shaders/materials/Material.slang).
    [[nodiscard]] nvrhi::IBindingLayout* materialLayout() const { return m_materialLayout; }

    [[nodiscard]] const RenderResourceStatistics& statistics() const { return m_statistics; }

private:
    RenderResources(nvrhi::IDevice* device, AssetManager& assets, const RenderResourcesDesc& desc);

    /// Whether a texture keeps to the limit and the budget, or keeps its size.
    enum class TextureSize : uint8_t {
        Limited,
        AsItIs,
    };

    struct TextureKey {
        Uuid id;
        ColorSpace colorSpace = ColorSpace::Linear;
        TextureSize size = TextureSize::Limited;
        bool operator==(const TextureKey&) const = default;
    };

    struct TextureKeyHash {
        size_t operator()(const TextureKey& key) const noexcept;
    };

    [[nodiscard]] Status initialize();
    void clear();
    [[nodiscard]] nvrhi::ICommandList* uploadCommandList();
    [[nodiscard]] Result<GpuMesh> createMesh(const MeshData& mesh);
    /// A texture of `image` from its level `firstLevel` on.
    [[nodiscard]] Result<nvrhi::TextureHandle> createTexture(const Image& image, ColorSpace colorSpace,
                                                             const std::string& name, uint32_t firstLevel);
    [[nodiscard]] Result<GpuMaterial> createMaterial(const MaterialData& material, const std::string& name);
    [[nodiscard]] Result<nvrhi::TextureHandle> createRenderTarget(const RenderTargetData& target,
                                                                  const std::string& name);
    /// The texture for a material slot: the asset's, or `fallback` when the id is nil or
    /// the texture cannot be loaded. A render target is a picture: in a slot of colors it
    /// is renderTarget(); in one of numbers it is reported once and `fallback` stands in.
    [[nodiscard]] nvrhi::ITexture* texture(const Uuid& id, ColorSpace colorSpace, nvrhi::ITexture* fallback,
                                           TextureSize size = TextureSize::Limited);
    /// The level bias at which every texture known so far fits the budget - with
    /// `footprint` in the place of the texture of `key`, or beside them, when a key is given.
    [[nodiscard]] uint32_t biasOfKnownTextures(const std::optional<TextureKey>& key = std::nullopt,
                                               const TextureFootprint& footprint = {}) const;
    /// Takes `bias` for every texture from now on: the textures made so far are made
    /// again at the next beginFrame().
    void takeBias(uint32_t bias);
    [[nodiscard]] Result<nvrhi::ISampler*> sampler(const MaterialData& material);
    [[nodiscard]] Result<nvrhi::TextureHandle> createSolidTexture(const Vec4& color, const std::string& name);
    /// The normal map of a material that has none: one texel that leaves the surface
    /// normal exactly as it is.
    [[nodiscard]] Result<nvrhi::TextureHandle> createFlatNormalTexture();
    /// A texture array of `layers` layers of `size` pixels square, with all their levels,
    /// in display colors.
    [[nodiscard]] Result<nvrhi::TextureHandle> createCookieArray(uint32_t size, uint32_t layers, const char* name);
    /// Puts `image`, a picture of four 8-bit channels, into a layer of the cookie array,
    /// which is made, or made larger, when it has no such layer yet.
    [[nodiscard]] Status putCookie(const Image& image, uint32_t layer);

    nvrhi::IDevice* m_device;
    AssetManager& m_assets;
    RenderResourcesDesc m_desc;
    /// The revision of the asset database the resources were brought up to.
    uint64_t m_assetRevision = 0;
    /// What revision() says: how many times everything was dropped.
    uint64_t m_revision = 0;
    /// Every texture of the assets loaded since they were last read, whatever has been
    /// dropped since: the bias is that of all of them, so that it does not depend on the
    /// order they were asked for in.
    std::unordered_map<TextureKey, TextureFootprint, TextureKeyHash> m_knownTextures;
    uint32_t m_textureLimit = 0;
    uint32_t m_requestedTextureLimit = 0;
    uint32_t m_textureBias = 0;
    /// The textures are to be made again at another size, at the next beginFrame().
    bool m_texturesOutOfDate = false;
    /// The largest bias the log has been told of.
    uint32_t m_reportedBias = 0;

    nvrhi::BindingLayoutHandle m_materialLayout;
    nvrhi::CommandListHandle m_uploadCommandList;
    nvrhi::TextureHandle m_whiteTexture;
    nvrhi::TextureHandle m_flatNormalTexture;
    /// What views bind while no cookie is in use, and the array of cookies once one is.
    nvrhi::TextureHandle m_noCookies;
    nvrhi::TextureHandle m_cookies;
    /// Layers m_cookies has: it grows, twice as large every time, to kMaxLightCookies.
    uint32_t m_cookieCapacity = 0;
    /// The layer of every texture asked for as a cookie; -1 for one that cannot be one,
    /// so that it is tried and reported only once.
    std::unordered_map<Uuid, int32_t> m_cookieLayers;
    uint32_t m_cookieCount = 0;
    GpuMaterial m_defaultMaterial;

    std::unordered_map<Uuid, GpuMesh> m_meshes;
    std::unordered_map<Uuid, GpuMaterial> m_materials;
    std::unordered_map<TextureKey, nvrhi::TextureHandle, TextureKeyHash> m_textures;
    /// Null for one that could not be made, so that it is tried and reported only once.
    std::unordered_map<Uuid, nvrhi::TextureHandle> m_renderTargets;
    /// Render targets a slot of numbers asked for, so that each is reported only once.
    std::unordered_set<Uuid> m_misusedRenderTargets;
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
