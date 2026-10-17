#include "cordite/assets/AssetManager.h"

#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/GltfImporter.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// How many parents a material may have above it. Far more than any sensible chain, and
/// what stops a material that is its own ancestor.
constexpr uint32_t kMaxMaterialParents = 16;

MeshData makeBuiltinMesh(BuiltinMesh mesh) {
    switch (mesh) {
        case BuiltinMesh::Cube:
            return makeBox();
        case BuiltinMesh::Sphere:
            return makeSphere();
        case BuiltinMesh::Plane:
            return makePlane();
        case BuiltinMesh::Cylinder:
            return makeCylinder();
        case BuiltinMesh::Cone:
            return makeCone();
        case BuiltinMesh::Capsule:
            return makeCapsule();
        case BuiltinMesh::Torus:
            return makeTorus();
    }
    return makeBox();
}

/// Which nodes of a document are joints of its skins. A skin that could not be imported
/// lists none.
std::vector<bool> jointNodesOf(const GltfDocument& document) {
    std::vector<bool> isJoint(document.nodes.size(), false);
    for (const GltfSkin& skin : document.skins) {
        for (const uint32_t joint : skin.joints) {
            if (joint < isJoint.size()) {
                isJoint[joint] = true;
            }
        }
    }
    return isJoint;
}

} // namespace

AssetManager::AssetManager(AssetDatabaseDesc desc) : m_database(std::move(desc)) {}

AssetManager::~AssetManager() = default;

Result<std::unique_ptr<AssetManager>> AssetManager::create(AssetDatabaseDesc desc) {
    std::unique_ptr<AssetManager> manager(new AssetManager(std::move(desc)));
    CORDITE_TRY(manager->m_database.refresh());
    return manager;
}

Status AssetManager::refresh() {
    m_meshes.clear();
    m_materials.clear();
    m_models.clear();
    m_skeletons.clear();
    m_animations.clear();
    m_scripts.clear();
    m_sounds.clear();
    m_prefabs.clear();
    m_renderTargets.clear();
    m_textures.clear();
    m_embeddedImages.clear();
    m_importedFiles.clear();
    return m_database.refresh();
}

Result<const AssetInfo*> AssetManager::lookUp(const Uuid& id, AssetType type) const {
    const AssetInfo* asset = m_database.find(id);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset with the id {}", id.toString());
    }
    if (asset->type != type) {
        return makeError(ErrorCode::InvalidArgument, "{} is {}, not {}", asset->reference(), withArticle(asset->type),
                         withArticle(type));
    }
    return asset;
}

Result<std::filesystem::path> AssetManager::sourceFileWithin(const AssetInfo& asset, size_t maxBytes,
                                                             std::string_view what) const {
    std::filesystem::path file = m_database.sourceFile(asset);
    std::error_code failed;
    const std::uintmax_t size = std::filesystem::file_size(file, failed);
    if (failed) {
        return makeError(ErrorCode::IoError, "{} cannot be read: {}", asset.reference(), failed.message());
    }
    if (size > maxBytes) {
        return makeError(ErrorCode::InvalidArgument, "{} has {} bytes; {} has {} at most", asset.reference(), size,
                         what, maxBytes);
    }
    return file;
}

Status AssetManager::importModelFile(const AssetInfo& asset) {
    // `asset` may be the model or anything inside it; the file is the model's.
    const AssetInfo* file = m_database.findByReference(asset.path);
    if (file == nullptr || file->type != AssetType::Model) {
        return makeError(ErrorCode::NotFound, "{} is not inside a model file", asset.reference());
    }
    if (const auto imported = m_importedFiles.find(file->id); imported != m_importedFiles.end()) {
        return imported->second;
    }

    Result<GltfDocument> result =
        importGltf(m_database.sourceFile(*file), {.loadData = true, .rootDirectory = m_database.rootDirectory()});
    if (!result) {
        m_importedFiles.emplace(file->id, tl::unexpected(result.error()));
        return tl::unexpected(result.error());
    }
    GltfDocument& document = *result;
    for (const std::string& warning : document.warnings) {
        CORDITE_LOG_WARN("Assets", "{}: {}", file->path, warning);
    }

    const Uuid fileId = file->id;
    const auto idOf = [&fileId](std::string_view kind, size_t index) {
        return subAssetId(fileId, fmt::format("{}/{}", kind, index));
    };
    // The texture asset for an image of the file: the image itself when it is embedded,
    // otherwise the asset of the file it names. Nil when there is none; the database
    // has reported that as a problem.
    const auto textureOf = [&](const std::optional<uint32_t>& image) {
        if (!image || *image >= document.images.size()) {
            return Uuid();
        }
        const GltfImage& source = document.images[*image];
        if (source.isEmbedded()) {
            return idOf("image", *image);
        }
        const AssetInfo* texture = m_database.findByReference(resolveAssetReference(file->path, source.uri));
        return texture != nullptr && texture->type == AssetType::Texture ? texture->id : Uuid();
    };

    std::vector<Uuid> materialIds(document.materials.size());
    for (size_t index = 0; index < document.materials.size(); ++index) {
        const GltfMaterial& source = document.materials[index];
        MaterialData material = source.material;
        material.baseColorTexture = textureOf(source.images.baseColor);
        material.metallicRoughnessTexture = textureOf(source.images.metallicRoughness);
        material.normalTexture = textureOf(source.images.normal);
        material.occlusionTexture = textureOf(source.images.occlusion);
        material.emissiveTexture = textureOf(source.images.emissive);

        materialIds[index] = idOf("material", index);
        m_materials[materialIds[index]] = std::make_shared<const MaterialData>(material);
    }

    // For each mesh, the material ids of its submeshes; nodes need them below.
    std::vector<std::vector<Uuid>> meshMaterials(document.meshes.size());
    for (size_t index = 0; index < document.meshes.size(); ++index) {
        GltfMesh& source = document.meshes[index];
        if (source.mesh.submeshes.empty()) {
            continue;
        }
        for (const std::optional<uint32_t>& material : source.materials) {
            meshMaterials[index].push_back(material && *material < materialIds.size() ? materialIds[*material]
                                                                                      : Uuid());
        }
        // What the renderer is handed has its levels of detail; what an importer reads
        // has none - glTF has no place for them.
        generateLods(source.mesh);
        m_meshes[idOf("mesh", index)] = std::make_shared<const MeshData>(std::move(source.mesh));
    }

    for (size_t index = 0; index < document.images.size(); ++index) {
        GltfImage& source = document.images[index];
        if (source.isEmbedded()) {
            m_embeddedImages[idOf("image", index)] =
                std::make_shared<const std::vector<std::byte>>(std::move(source.encoded));
        }
    }

    // A skin that could not be imported is no skeleton, and a node that names it draws
    // its mesh without one - which the importer has seen to already.
    for (size_t index = 0; index < document.skins.size(); ++index) {
        if (!document.skins[index].joints.empty()) {
            m_skeletons[idOf("skeleton", index)] =
                std::make_shared<const SkeletonData>(std::move(document.skins[index].skeleton));
        }
    }

    ModelData model;
    model.name = file->name;
    model.roots = document.roots;
    for (size_t index = 0; index < document.animations.size(); ++index) {
        model.animations.push_back(idOf("animation", index));
        m_animations[model.animations.back()] =
            std::make_shared<const AnimationData>(std::move(document.animations[index].animation));
    }
    const std::vector<bool> isJoint = jointNodesOf(document);
    model.nodes.reserve(document.nodes.size());
    for (GltfNode& source : document.nodes) {
        ModelNode node;
        node.joint = isJoint[model.nodes.size()];
        node.name = std::move(source.name);
        node.transform = source.transform;
        node.parent = source.parent;
        node.children = std::move(source.children);
        node.camera = source.camera;
        if (source.mesh && m_meshes.contains(idOf("mesh", *source.mesh))) {
            node.mesh = idOf("mesh", *source.mesh);
            node.materials = meshMaterials[*source.mesh];
            if (source.skin && m_skeletons.contains(idOf("skeleton", *source.skin))) {
                node.skeleton = idOf("skeleton", *source.skin);
            }
        }
        model.nodes.push_back(std::move(node));
    }
    m_models[fileId] = std::make_shared<const ModelData>(std::move(model));

    m_importedFiles.emplace(fileId, Status());
    return {};
}

Result<std::shared_ptr<const MeshData>> AssetManager::loadMesh(const Uuid& id) {
    if (const auto cached = m_meshes.find(id); cached != m_meshes.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Mesh));

    if (asset->isBuiltin()) {
        for (const BuiltinMesh mesh : kBuiltinMeshes) {
            if (builtinMeshId(mesh) == id) {
                MeshData data = makeBuiltinMesh(mesh);
                data.name = asset->name;
                generateLods(data);
                return m_meshes.emplace(id, std::make_shared<const MeshData>(std::move(data))).first->second;
            }
        }
        return makeError(ErrorCode::NotFound, "{} is not a built-in mesh", asset->reference());
    }

    CORDITE_TRY(importModelFile(*asset));
    if (const auto imported = m_meshes.find(id); imported != m_meshes.end()) {
        return imported->second;
    }
    // Listed when the file was indexed, but empty once its data was read.
    return makeError(ErrorCode::NotFound, "{} holds nothing that can be drawn", asset->reference());
}

Result<MaterialData> AssetManager::readMaterialFile(const AssetInfo& asset, uint32_t depth) {
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(m_database.sourceFile(asset)));
    Result<MaterialFile> file = parseMaterialFile(document);
    if (!file) {
        return tl::unexpected(file.error().withContext(asset.reference()));
    }

    MaterialData material;
    if (!file->parent.isNil()) {
        if (depth >= kMaxMaterialParents) {
            return makeError(ErrorCode::InvalidArgument,
                             "{}: its chain of parent materials is longer than {}, or leads back to itself",
                             asset.reference(), kMaxMaterialParents);
        }
        Result<std::shared_ptr<const MaterialData>> parent = loadMaterial(file->parent, depth + 1);
        if (!parent) {
            return tl::unexpected(parent.error().withContext(fmt::format("{}: its parent", asset.reference())));
        }
        material = **parent;
    }
    file->properties.applyTo(&material);
    return material;
}

Result<std::shared_ptr<const MaterialData>> AssetManager::loadMaterial(const Uuid& id) {
    return loadMaterial(id, 0);
}

Result<std::shared_ptr<const MaterialData>> AssetManager::loadMaterial(const Uuid& id, uint32_t depth) {
    if (const auto cached = m_materials.find(id); cached != m_materials.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Material));

    if (asset->isBuiltin()) {
        return m_materials.emplace(id, std::make_shared<const MaterialData>()).first->second;
    }
    if (asset->subPath.empty()) {
        CORDITE_TRY_ASSIGN(const MaterialData material, readMaterialFile(*asset, depth));
        return m_materials.insert_or_assign(id, std::make_shared<const MaterialData>(material)).first->second;
    }

    CORDITE_TRY(importModelFile(*asset));
    if (const auto imported = m_materials.find(id); imported != m_materials.end()) {
        return imported->second;
    }
    return makeError(ErrorCode::NotFound, "{} is no longer in its file", asset->reference());
}

Result<std::shared_ptr<const Image>> AssetManager::loadTexture(const Uuid& id) {
    if (const auto cached = m_textures.find(id); cached != m_textures.end()) {
        if (std::shared_ptr<const Image> image = cached->second.lock()) {
            return image;
        }
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Texture));

    Result<Image> decoded = makeError(ErrorCode::NotFound, "{} is no longer in its file", asset->reference());
    if (asset->subPath.empty()) {
        decoded = Image::load(m_database.sourceFile(*asset));
    } else {
        CORDITE_TRY(importModelFile(*asset));
        if (const auto embedded = m_embeddedImages.find(id); embedded != m_embeddedImages.end()) {
            decoded = Image::decode(*embedded->second, asset->reference());
        }
    }
    if (!decoded) {
        return tl::unexpected(decoded.error());
    }

    auto image = std::make_shared<const Image>(std::move(*decoded));
    m_textures[id] = image;
    return image;
}

Result<std::shared_ptr<const ModelData>> AssetManager::loadModel(const Uuid& id) {
    if (const auto cached = m_models.find(id); cached != m_models.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Model));
    CORDITE_TRY(importModelFile(*asset));
    if (const auto imported = m_models.find(id); imported != m_models.end()) {
        return imported->second;
    }
    return makeError(ErrorCode::NotFound, "{} cannot be loaded", asset->reference());
}

Result<std::shared_ptr<const SkeletonData>> AssetManager::loadSkeleton(const Uuid& id) {
    if (const auto cached = m_skeletons.find(id); cached != m_skeletons.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Skeleton));
    CORDITE_TRY(importModelFile(*asset));
    if (const auto imported = m_skeletons.find(id); imported != m_skeletons.end()) {
        return imported->second;
    }
    return makeError(ErrorCode::NotFound, "{} is no longer in its file", asset->reference());
}

Result<std::shared_ptr<const AnimationData>> AssetManager::loadAnimation(const Uuid& id) {
    if (const auto cached = m_animations.find(id); cached != m_animations.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Animation));
    CORDITE_TRY(importModelFile(*asset));
    if (const auto imported = m_animations.find(id); imported != m_animations.end()) {
        return imported->second;
    }
    return makeError(ErrorCode::NotFound, "{} is no longer in its file", asset->reference());
}

Result<std::shared_ptr<const ScriptSource>> AssetManager::loadScript(const Uuid& id) {
    if (const auto cached = m_scripts.find(id); cached != m_scripts.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Script));

    CORDITE_TRY_ASSIGN(const std::filesystem::path file, sourceFileWithin(*asset, kMaxScriptBytes, "a script"));
    CORDITE_TRY_ASSIGN(std::string text, readTextFile(file));

    auto script = std::make_shared<const ScriptSource>(ScriptSource{.text = std::move(text)});
    m_scripts[id] = script;
    return script;
}

Result<std::shared_ptr<const SoundData>> AssetManager::loadSound(const Uuid& id) {
    if (const auto cached = m_sounds.find(id); cached != m_sounds.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Sound));

    CORDITE_TRY_ASSIGN(const std::filesystem::path file, sourceFileWithin(*asset, kMaxSoundFileBytes, "a sound file"));
    CORDITE_TRY_ASSIGN(const std::vector<std::byte> encoded, readBinaryFile(file));
    Result<SoundData> decoded = decodeSound(encoded);
    if (!decoded) {
        return tl::unexpected(decoded.error().withContext(asset->reference()));
    }

    auto sound = std::make_shared<const SoundData>(std::move(*decoded));
    m_sounds[id] = sound;
    return sound;
}

Result<RenderTargetData> AssetManager::loadRenderTarget(const Uuid& id) {
    if (const auto cached = m_renderTargets.find(id); cached != m_renderTargets.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::RenderTarget));
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(m_database.sourceFile(*asset)));
    Result<RenderTargetData> target = parseRenderTargetFile(document);
    if (!target) {
        return tl::unexpected(target.error().withContext(asset->reference()));
    }
    return m_renderTargets.emplace(id, *target).first->second;
}

Result<std::shared_ptr<const PrefabData>> AssetManager::loadPrefab(const Uuid& id) {
    if (const auto cached = m_prefabs.find(id); cached != m_prefabs.end()) {
        return cached->second;
    }
    CORDITE_TRY_ASSIGN(const AssetInfo* asset, lookUp(id, AssetType::Prefab));

    CORDITE_TRY_ASSIGN(const std::filesystem::path file,
                       sourceFileWithin(*asset, kMaxPrefabFileBytes, "a prefab file"));
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(file));
    Result<PrefabData> parsed = parsePrefab(document);
    if (!parsed) {
        return tl::unexpected(parsed.error().withContext(asset->reference()));
    }
    if (parsed->name.empty()) {
        parsed->name = asset->name;
    }

    auto prefab = std::make_shared<const PrefabData>(std::move(*parsed));
    m_prefabs[id] = prefab;
    return prefab;
}

} // namespace cordite
