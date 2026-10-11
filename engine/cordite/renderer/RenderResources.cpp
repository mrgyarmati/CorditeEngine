#include "cordite/renderer/RenderResources.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

namespace cordite {
namespace {

/// Mirrors MaterialConstants in shaders/materials/Material.slang.
struct MaterialConstants {
    Vec4 baseColor;
    Vec3 emissive;
    float alphaCutoff;
    float metallic;
    float roughness;
    float specular;
    float normalScale;
    Vec2 uvScale;
    Vec2 uvOffset;
    float occlusionStrength;
    uint32_t uvChannel;
    uint32_t occlusionUvChannel;
    float padding;
};

static_assert(sizeof(MaterialConstants) == 80, "must match the cbuffer layout in Material.slang");

/// The register space of the material binding set; the view's bindings are in space 0.
constexpr uint32_t kMaterialRegisterSpace = 1;

nvrhi::SamplerAddressMode addressMode(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::Repeat:
            return nvrhi::SamplerAddressMode::Wrap;
        case TextureWrap::MirroredRepeat:
            return nvrhi::SamplerAddressMode::Mirror;
        case TextureWrap::Clamp:
            return nvrhi::SamplerAddressMode::Clamp;
    }
    return nvrhi::SamplerAddressMode::Wrap;
}

nvrhi::Format textureFormat(ImageFormat format, ColorSpace colorSpace) {
    switch (format) {
        case ImageFormat::R8:
            return nvrhi::Format::R8_UNORM;
        case ImageFormat::Rg8:
            return nvrhi::Format::RG8_UNORM;
        case ImageFormat::Rgba8:
            return colorSpace == ColorSpace::Srgb ? nvrhi::Format::SRGBA8_UNORM : nvrhi::Format::RGBA8_UNORM;
        case ImageFormat::Rgba32F:
            // Sent as half floats (see halfFloatsOf).
            return nvrhi::Format::RGBA16_FLOAT;
    }
    return nvrhi::Format::RGBA8_UNORM;
}

/// The pixels of an image of 32-bit floats as half floats, which is how such images are
/// kept on the GPU: every GPU can filter half floats, which is not so for 32-bit ones,
/// and they take half the memory. What a half float cannot hold - more than 65504, or
/// no number at all - becomes the most it can hold, or nothing.
std::vector<uint16_t> halfFloatsOf(const Image& image) {
    constexpr float kLargestHalf = 65504.0f;
    const std::span<const std::byte> bytes = image.pixels();
    std::vector<uint16_t> halves(bytes.size() / sizeof(float));
    for (size_t index = 0; index < halves.size(); ++index) {
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + index * sizeof(float), sizeof(float));
        value = std::isnan(value) ? 0.0f : std::clamp(value, -kLargestHalf, kLargestHalf);
        halves[index] = glm::packHalf1x16(value);
    }
    return halves;
}

/// The first level of a mip chain to upload, so that no side exceeds `limit`.
uint32_t firstLevelWithin(uint32_t width, uint32_t height, uint32_t limit, uint32_t levelCount) {
    uint32_t level = 0;
    while (limit != 0 && level + 1 < levelCount && std::max(width >> level, height >> level) > limit) {
        ++level;
    }
    return level;
}

} // namespace

size_t RenderResources::TextureKeyHash::operator()(const TextureKey& key) const noexcept {
    return std::hash<Uuid>()(key.id) ^ (key.colorSpace == ColorSpace::Srgb ? 0x9e3779b97f4a7c15ull : 0ull);
}

RenderResources::RenderResources(nvrhi::IDevice* device, AssetManager& assets, const RenderResourcesDesc& desc)
    : m_device(device), m_assets(assets), m_desc(desc) {}

RenderResources::~RenderResources() {
    if (m_uploadCommandList) {
        // Never executed: what it would have filled is going away with it.
        m_uploadCommandList->close();
    }
}

Result<std::unique_ptr<RenderResources>> RenderResources::create(nvrhi::IDevice* device, AssetManager& assets,
                                                                 const RenderResourcesDesc& desc) {
    std::unique_ptr<RenderResources> resources(new RenderResources(device, assets, desc));
    CORDITE_TRY(resources->initialize());
    return resources;
}

Status RenderResources::initialize() {
    m_assetRevision = m_assets.database().revision();

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kMaterialRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),    nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Texture_SRV(3),    nvrhi::BindingLayoutItem::Texture_SRV(4),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    m_materialLayout = m_device->createBindingLayout(layoutDesc);
    if (!m_materialLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the material binding layout");
    }

    // What an empty texture slot reads as: white leaves the material's own values as
    // they are, and the flat normal leaves the surface normal alone.
    CORDITE_TRY_ASSIGN(m_whiteTexture, createSolidTexture(Vec4(1.0f), "Default white"));
    CORDITE_TRY_ASSIGN(m_flatNormalTexture, createFlatNormalTexture());
    CORDITE_TRY_ASSIGN(m_defaultMaterial, createMaterial(MaterialData(), "Default material"));
    flushUploads();
    // These few stay for good, whatever happens to the assets.
    m_permanentStatistics = m_statistics;
    return {};
}

void RenderResources::clear() {
    m_meshes.clear();
    m_materials.clear();
    m_textures.clear();
    m_failedMeshes.clear();
    m_statistics = m_permanentStatistics;
}

void RenderResources::beginFrame() {
    const uint64_t revision = m_assets.database().revision();
    if (revision != m_assetRevision) {
        m_assetRevision = revision;
        // The GPU objects stay alive for as long as command lists in flight use them.
        clear();
    }
}

nvrhi::ICommandList* RenderResources::uploadCommandList() {
    if (!m_uploadCommandList) {
        // Uploads are recorded while the frame's command list is open. Only one
        // "immediate" command list may be open at a time, so this one is not.
        m_uploadCommandList =
            m_device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
        m_uploadCommandList->open();
    }
    return m_uploadCommandList;
}

void RenderResources::flushUploads() {
    if (!m_uploadCommandList) {
        return;
    }
    m_uploadCommandList->close();
    m_device->executeCommandList(m_uploadCommandList);
    // A command list keeps the staging memory of everything it ever uploaded. Letting it
    // go returns that memory once the GPU has taken the data.
    m_uploadCommandList = nullptr;
}

Result<GpuMesh> RenderResources::createMesh(const MeshData& mesh) {
    CORDITE_TRY(validateMesh(mesh));

    GpuMesh gpuMesh;
    gpuMesh.submeshes = mesh.submeshes;
    gpuMesh.bounds = mesh.bounds;
    gpuMesh.serial = m_nextSerial++;

    nvrhi::BufferDesc vertexDesc;
    vertexDesc.byteSize = mesh.vertices.size() * sizeof(MeshVertex);
    vertexDesc.debugName = mesh.name + " vertices";
    vertexDesc.isVertexBuffer = true;
    vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
    vertexDesc.keepInitialState = true;
    gpuMesh.vertexBuffer = m_device->createBuffer(vertexDesc);

    // Sixteen-bit indices where they reach every vertex: half the memory and bandwidth.
    const bool smallIndices = mesh.vertices.size() <= 0xFFFF;
    gpuMesh.indexFormat = smallIndices ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT;
    nvrhi::BufferDesc indexDesc;
    indexDesc.byteSize = mesh.indices.size() * (smallIndices ? sizeof(uint16_t) : sizeof(uint32_t));
    indexDesc.debugName = mesh.name + " indices";
    indexDesc.isIndexBuffer = true;
    indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
    indexDesc.keepInitialState = true;
    // A mesh may consist of empty submeshes only; a buffer cannot be empty.
    if (indexDesc.byteSize > 0) {
        gpuMesh.indexBuffer = m_device->createBuffer(indexDesc);
    }
    if (!gpuMesh.vertexBuffer || (indexDesc.byteSize > 0 && !gpuMesh.indexBuffer)) {
        return makeError(ErrorCode::DeviceError, "cannot allocate {} bytes of GPU memory for mesh '{}'",
                         vertexDesc.byteSize + indexDesc.byteSize, mesh.name);
    }

    nvrhi::ICommandList* commandList = uploadCommandList();
    commandList->writeBuffer(gpuMesh.vertexBuffer, mesh.vertices.data(), vertexDesc.byteSize);
    if (gpuMesh.indexBuffer) {
        if (smallIndices) {
            std::vector<uint16_t> narrow(mesh.indices.size());
            std::ranges::transform(mesh.indices, narrow.begin(),
                                   [](uint32_t index) { return static_cast<uint16_t>(index); });
            commandList->writeBuffer(gpuMesh.indexBuffer, narrow.data(), indexDesc.byteSize);
        } else {
            commandList->writeBuffer(gpuMesh.indexBuffer, mesh.indices.data(), indexDesc.byteSize);
        }
    }

    ++m_statistics.meshCount;
    m_statistics.meshBytes += vertexDesc.byteSize + indexDesc.byteSize;
    return gpuMesh;
}

const GpuMesh* RenderResources::mesh(const Uuid& id) {
    if (id.isNil()) {
        return nullptr;
    }
    if (const auto found = m_meshes.find(id); found != m_meshes.end()) {
        return &found->second;
    }
    if (m_failedMeshes.contains(id)) {
        return nullptr;
    }

    Result<GpuMesh> created = makeError(ErrorCode::Unknown, "not loaded");
    if (Result<std::shared_ptr<const MeshData>> loaded = m_assets.loadMesh(id)) {
        created = createMesh(**loaded);
    } else {
        created = tl::unexpected(loaded.error());
    }
    if (!created) {
        CORDITE_LOG_ERROR("Renderer", "cannot draw mesh {}: {}", id.toString(), created.error().message);
        m_failedMeshes.insert(id);
        return nullptr;
    }
    return &m_meshes.emplace(id, std::move(*created)).first->second;
}

Result<nvrhi::TextureHandle> RenderResources::createTexture(const Image& image, ColorSpace colorSpace,
                                                            const std::string& name) {
    CORDITE_TRY_ASSIGN(const std::vector<Image> smaller, generateMipChain(image, colorSpace));

    // Level 0 is the image itself; the chain holds the rest.
    const auto levelCount = static_cast<uint32_t>(smaller.size() + 1);
    const uint32_t firstLevel = firstLevelWithin(image.width(), image.height(), m_desc.maxTextureDimension, levelCount);
    const auto level = [&](uint32_t index) -> const Image& { return index == 0 ? image : smaller[index - 1]; };

    nvrhi::TextureDesc desc;
    desc.width = level(firstLevel).width();
    desc.height = level(firstLevel).height();
    desc.mipLevels = levelCount - firstLevel;
    desc.format = textureFormat(image.format(), colorSpace);
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = m_device->createTexture(desc);
    if (!texture) {
        return makeError(ErrorCode::DeviceError, "cannot allocate a {}x{} texture for {}", desc.width, desc.height,
                         name);
    }

    nvrhi::ICommandList* commandList = uploadCommandList();
    for (uint32_t index = firstLevel; index < levelCount; ++index) {
        const Image& pixels = level(index);
        if (pixels.format() == ImageFormat::Rgba32F) {
            const std::vector<uint16_t> halves = halfFloatsOf(pixels);
            const size_t byteSize = halves.size() * sizeof(uint16_t);
            commandList->writeTexture(texture, 0, index - firstLevel, halves.data(), byteSize / pixels.height());
            m_statistics.textureBytes += byteSize;
            continue;
        }
        commandList->writeTexture(texture, 0, index - firstLevel, pixels.pixels().data(), pixels.rowBytes());
        m_statistics.textureBytes += pixels.pixels().size();
    }
    ++m_statistics.textureCount;
    return texture;
}

Result<nvrhi::TextureHandle> RenderResources::createSolidTexture(const Vec4& color, const std::string& name) {
    CORDITE_TRY_ASSIGN(Image image, Image::create(1, 1, ImageFormat::Rgba8));
    image.setPixel(0, 0, color);
    return createTexture(image, ColorSpace::Linear, name);
}

Result<nvrhi::TextureHandle> RenderResources::createFlatNormalTexture() {
    // "No tilt" is one half in a normal map, which no 8-bit number is: 128 of 255 is a
    // normal that leans by a third of a degree, enough to show in light that grazes a
    // surface. In half floats, one half and one are exact: (0.5, 0.5, 1, 1).
    constexpr std::array<uint16_t, 4> kFlatNormal = {0x3800, 0x3800, 0x3C00, 0x3C00};

    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "Default normal";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    nvrhi::TextureHandle texture = m_device->createTexture(desc);
    if (!texture) {
        return makeError(ErrorCode::DeviceError, "cannot allocate the default normal texture");
    }
    uploadCommandList()->writeTexture(texture, 0, 0, kFlatNormal.data(), sizeof(kFlatNormal));
    m_statistics.textureBytes += sizeof(kFlatNormal);
    ++m_statistics.textureCount;
    return texture;
}

nvrhi::ITexture* RenderResources::texture(const Uuid& id, ColorSpace colorSpace, nvrhi::ITexture* fallback) {
    if (id.isNil()) {
        return fallback;
    }
    const TextureKey key{id, colorSpace};
    if (const auto found = m_textures.find(key); found != m_textures.end()) {
        // A null entry records a texture that could not be loaded.
        return found->second ? found->second.Get() : fallback;
    }

    const AssetInfo* asset = m_assets.database().find(id);
    const std::string name = asset != nullptr ? asset->reference() : id.toString();

    Result<nvrhi::TextureHandle> created = makeError(ErrorCode::Unknown, "not loaded");
    if (Result<std::shared_ptr<const Image>> loaded = m_assets.loadTexture(id)) {
        created = createTexture(**loaded, colorSpace, name);
    } else {
        created = tl::unexpected(loaded.error());
    }
    if (!created) {
        CORDITE_LOG_ERROR("Renderer", "cannot use texture {}: {}", name, created.error().message);
        m_textures.emplace(key, nullptr);
        return fallback;
    }
    return m_textures.emplace(key, std::move(*created)).first->second.Get();
}

Result<nvrhi::ISampler*> RenderResources::sampler(const MaterialData& material) {
    const uint32_t key = static_cast<uint32_t>(material.wrapU) | (static_cast<uint32_t>(material.wrapV) << 8) |
                         (static_cast<uint32_t>(material.filter) << 16);
    if (const auto found = m_samplers.find(key); found != m_samplers.end()) {
        return found->second.Get();
    }

    const bool smooth = material.filter == TextureFilter::Linear;
    nvrhi::SamplerDesc desc;
    desc.setAllFilters(smooth)
        .setAddressU(addressMode(material.wrapU))
        .setAddressV(addressMode(material.wrapV))
        .setAddressW(nvrhi::SamplerAddressMode::Wrap)
        .setMaxAnisotropy(smooth ? std::clamp(m_desc.maxAnisotropy, 1.0f, 16.0f) : 1.0f);
    nvrhi::SamplerHandle created = m_device->createSampler(desc);
    if (!created) {
        return makeError(ErrorCode::DeviceError, "cannot create a texture sampler");
    }
    return m_samplers.emplace(key, std::move(created)).first->second.Get();
}

Result<GpuMaterial> RenderResources::createMaterial(const MaterialData& material, const std::string& name) {
    MaterialConstants constants{};
    constants.baseColor = material.baseColor;
    constants.emissive = material.emissiveColor * material.emissiveIntensity;
    constants.alphaCutoff = material.alphaCutoff;
    constants.metallic = material.metallic;
    constants.roughness = material.roughness;
    constants.specular = material.specular;
    constants.normalScale = material.normalScale;
    constants.uvScale = material.uvScale;
    constants.uvOffset = material.uvOffset;
    constants.occlusionStrength = material.occlusionStrength;
    constants.uvChannel = material.uvChannel == 1 ? 1u : 0u;
    constants.occlusionUvChannel = material.occlusionUvChannel == 1 ? 1u : 0u;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = name + " constants";
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    const nvrhi::BufferHandle buffer = m_device->createBuffer(bufferDesc);
    if (!buffer) {
        return makeError(ErrorCode::DeviceError, "cannot allocate the constants of material {}", name);
    }
    uploadCommandList()->writeBuffer(buffer, &constants, sizeof(constants));
    CORDITE_TRY_ASSIGN(nvrhi::ISampler* const materialSampler, sampler(material));

    // The slot a texture is in decides how it is read: colors are sRGB, the rest numbers.
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, buffer),
        nvrhi::BindingSetItem::Texture_SRV(0, texture(material.baseColorTexture, ColorSpace::Srgb, m_whiteTexture)),
        nvrhi::BindingSetItem::Texture_SRV(
            1, texture(material.metallicRoughnessTexture, ColorSpace::Linear, m_whiteTexture)),
        nvrhi::BindingSetItem::Texture_SRV(2, texture(material.normalTexture, ColorSpace::Linear, m_flatNormalTexture)),
        nvrhi::BindingSetItem::Texture_SRV(3, texture(material.occlusionTexture, ColorSpace::Linear, m_whiteTexture)),
        nvrhi::BindingSetItem::Texture_SRV(4, texture(material.emissiveTexture, ColorSpace::Srgb, m_whiteTexture)),
        nvrhi::BindingSetItem::Sampler(0, materialSampler),
    };

    GpuMaterial gpuMaterial;
    gpuMaterial.bindings = m_device->createBindingSet(setDesc, m_materialLayout);
    if (!gpuMaterial.bindings) {
        return makeError(ErrorCode::DeviceError, "cannot create the bindings of material {}", name);
    }
    gpuMaterial.alphaMode = material.alphaMode;
    gpuMaterial.doubleSided = material.doubleSided;
    gpuMaterial.serial = m_nextSerial++;
    ++m_statistics.materialCount;
    return gpuMaterial;
}

const GpuMaterial& RenderResources::material(const Uuid& id) {
    if (id.isNil()) {
        return m_defaultMaterial;
    }
    if (const auto found = m_materials.find(id); found != m_materials.end()) {
        return found->second;
    }

    const AssetInfo* asset = m_assets.database().find(id);
    const std::string name = asset != nullptr ? asset->reference() : id.toString();

    Result<GpuMaterial> created = makeError(ErrorCode::Unknown, "not loaded");
    if (Result<std::shared_ptr<const MaterialData>> loaded = m_assets.loadMaterial(id)) {
        created = createMaterial(**loaded, name);
    } else {
        created = tl::unexpected(loaded.error());
    }
    if (!created) {
        CORDITE_LOG_ERROR("Renderer", "cannot use material {}: {}; the default material is drawn instead", name,
                          created.error().message);
        // Remembered under its id, so that it is neither loaded nor reported again.
        return m_materials.emplace(id, m_defaultMaterial).first->second;
    }
    return m_materials.emplace(id, std::move(*created)).first->second;
}

} // namespace cordite
