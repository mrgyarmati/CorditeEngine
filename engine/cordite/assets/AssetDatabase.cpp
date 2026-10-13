#include "cordite/assets/AssetDatabase.h"

#include "cordite/assets/GltfImporter.h"
#include "cordite/assets/Material.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace cordite {
namespace {

// Namespaces for derived ids. Changing either changes the id of every built-in asset,
// or of every asset that has no .meta file yet; tests pin the ids they produce.
constexpr Uuid kBuiltinNamespace(0x8c1f2f5e3d6b4a71ull, 0x9b0e5a7c2d4f6e13ull);
constexpr Uuid kPathNamespace(0x2e7a9d04c5b84f36ull, 0xa1d38b6f0e2c7a59ull);

constexpr DocumentFormat kMetaFormat{.name = "cordite.asset", .version = 1, .kind = "asset description"};
constexpr std::array<std::string_view, 3> kMetaMembers = {"format", "version", "id"};
constexpr std::string_view kMetaExtension = ".meta";

struct FileKind {
    std::string_view extension;
    AssetType type;
};

/// Longer extensions first: "x.material.json" is a material, not an unknown ".json".
constexpr std::array<FileKind, 14> kFileKinds = {{
    {".material.json", AssetType::Material},
    {".scene.json", AssetType::Scene},
    {".lua", AssetType::Script},
    {".wav", AssetType::Sound},
    {".flac", AssetType::Sound},
    {".mp3", AssetType::Sound},
    {".gltf", AssetType::Model},
    {".glb", AssetType::Model},
    {".png", AssetType::Texture},
    {".jpg", AssetType::Texture},
    {".jpeg", AssetType::Texture},
    {".tga", AssetType::Texture},
    {".bmp", AssetType::Texture},
    {".hdr", AssetType::Texture},
}};

const FileKind* kindOfFile(std::string_view path) {
    const std::string lowered = toLowerAscii(path);
    for (const FileKind& kind : kFileKinds) {
        if (lowered.size() > kind.extension.size() && lowered.ends_with(kind.extension)) {
            return &kind;
        }
    }
    return nullptr;
}

/// The file name without directories and without the extension that makes it an asset.
std::string displayName(std::string_view path, const FileKind& kind) {
    const size_t slash = path.find_last_of('/');
    const std::string_view fileName = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return std::string(fileName.substr(0, fileName.size() - kind.extension.size()));
}

std::filesystem::path metaPathOf(const std::filesystem::path& file) {
    std::filesystem::path meta = file;
    meta += pathFromUtf8(kMetaExtension);
    return meta;
}

Result<Uuid> readMetaFile(const std::filesystem::path& path) {
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(path));
    CORDITE_TRY(readDocumentHeader(document, kMetaFormat));
    CORDITE_TRY(rejectUnknownMembers(document, kMetaMembers));
    CORDITE_TRY_ASSIGN(const std::string text, jsonGet<std::string>(document, "id"));
    const std::optional<Uuid> id = Uuid::parse(text);
    if (!id || id->isNil()) {
        return makeError(ErrorCode::ParseError, "'{}' is not a valid asset id", text);
    }
    return *id;
}

Status writeMetaFile(const std::filesystem::path& path, const Uuid& id) {
    Json document = makeDocument(kMetaFormat);
    document["id"] = id.toString();
    return writeJsonFile(path, document);
}

std::string_view builtinMeshDisplayName(BuiltinMesh mesh) {
    switch (mesh) {
        case BuiltinMesh::Cube:
            return "Cube";
        case BuiltinMesh::Sphere:
            return "Sphere";
        case BuiltinMesh::Plane:
            return "Plane";
        case BuiltinMesh::Cylinder:
            return "Cylinder";
        case BuiltinMesh::Cone:
            return "Cone";
        case BuiltinMesh::Capsule:
            return "Capsule";
        case BuiltinMesh::Torus:
            return "Torus";
    }
    return "Mesh";
}

} // namespace

std::string_view nameOf(AssetType type) {
    switch (type) {
        case AssetType::Texture:
            return "texture";
        case AssetType::Mesh:
            return "mesh";
        case AssetType::Material:
            return "material";
        case AssetType::Model:
            return "model";
        case AssetType::Scene:
            return "scene";
        case AssetType::Skeleton:
            return "skeleton";
        case AssetType::Animation:
            return "animation";
        case AssetType::Script:
            return "script";
        case AssetType::Sound:
            return "sound";
    }
    return "unknown";
}

std::string_view displayNameOf(AssetType type) {
    switch (type) {
        case AssetType::Texture:
            return "Texture";
        case AssetType::Mesh:
            return "Mesh";
        case AssetType::Material:
            return "Material";
        case AssetType::Model:
            return "Model";
        case AssetType::Scene:
            return "Scene";
        case AssetType::Skeleton:
            return "Skeleton";
        case AssetType::Animation:
            return "Animation";
        case AssetType::Script:
            return "Script";
        case AssetType::Sound:
            return "Sound";
    }
    return "Unknown";
}

std::string_view withArticle(AssetType type) {
    switch (type) {
        case AssetType::Texture:
            return "a texture";
        case AssetType::Mesh:
            return "a mesh";
        case AssetType::Material:
            return "a material";
        case AssetType::Model:
            return "a model";
        case AssetType::Scene:
            return "a scene";
        case AssetType::Skeleton:
            return "a skeleton";
        case AssetType::Animation:
            return "an animation";
        case AssetType::Script:
            return "a script";
        case AssetType::Sound:
            return "a sound";
    }
    return "an unknown asset";
}

std::optional<AssetType> assetTypeFromName(std::string_view name) {
    for (const AssetType type : kAssetTypes) {
        if (equalsIgnoreCase(name, nameOf(type))) {
            return type;
        }
    }
    return std::nullopt;
}

std::string_view nameOf(BuiltinMesh mesh) {
    switch (mesh) {
        case BuiltinMesh::Cube:
            return "cube";
        case BuiltinMesh::Sphere:
            return "sphere";
        case BuiltinMesh::Plane:
            return "plane";
        case BuiltinMesh::Cylinder:
            return "cylinder";
        case BuiltinMesh::Cone:
            return "cone";
        case BuiltinMesh::Capsule:
            return "capsule";
        case BuiltinMesh::Torus:
            return "torus";
    }
    return "unknown";
}

Uuid builtinMeshId(BuiltinMesh mesh) {
    return Uuid::fromName(kBuiltinNamespace, fmt::format("mesh/{}", nameOf(mesh)));
}

Uuid defaultMaterialId() {
    return Uuid::fromName(kBuiltinNamespace, "material/default");
}

Uuid subAssetId(const Uuid& fileId, std::string_view subPath) {
    return Uuid::fromName(fileId, subPath);
}

std::string resolveAssetReference(std::string_view fromFile, std::string_view relativeUri) {
    // An absolute path or another drive cannot be inside the asset directory.
    if (relativeUri.empty() || relativeUri.front() == '/' || relativeUri.find(':') != std::string_view::npos ||
        relativeUri.find('\\') != std::string_view::npos) {
        return {};
    }

    std::vector<std::string_view> parts;
    const size_t slash = fromFile.find_last_of('/');
    if (slash != std::string_view::npos) {
        parts = split(fromFile.substr(0, slash), '/');
    }
    for (const std::string_view part : split(relativeUri, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (parts.empty()) {
                return {};
            }
            parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }

    std::string resolved;
    for (const std::string_view part : parts) {
        if (!resolved.empty()) {
            resolved += '/';
        }
        resolved += part;
    }
    return resolved;
}

std::string AssetInfo::reference() const {
    return subPath.empty() ? path : fmt::format("{}#{}", path, subPath);
}

/// A file of the asset directory that is an asset.
struct AssetDatabase::ScannedFile {
    /// Relative to the asset directory, with forward slashes.
    std::string relativePath;
    std::filesystem::path absolutePath;
    const FileKind* kind = nullptr;
};

AssetDatabase::AssetDatabase(AssetDatabaseDesc desc) : m_desc(std::move(desc)) {
    addBuiltinAssets();
}

void AssetDatabase::addBuiltinAssets() {
    for (const BuiltinMesh mesh : kBuiltinMeshes) {
        add({.id = builtinMeshId(mesh),
             .type = AssetType::Mesh,
             .name = std::string(builtinMeshDisplayName(mesh)),
             .path = std::string(kBuiltinAssetPath),
             .subPath = fmt::format("mesh/{}", nameOf(mesh))});
    }
    add({.id = defaultMaterialId(),
         .type = AssetType::Material,
         .name = "Default",
         .path = std::string(kBuiltinAssetPath),
         .subPath = "material/default"});
}

void AssetDatabase::add(AssetInfo asset) {
    const std::string reference = asset.reference();
    if (m_indexById.contains(asset.id)) {
        // Only reachable when two derived ids collide, which a 122-bit hash makes a
        // matter of theory. The asset is left out rather than replacing another.
        addProblem(fmt::format("{}: its id {} is already taken; the asset is ignored", reference, asset.id.toString()));
        return;
    }
    m_indexById.emplace(asset.id, m_assets.size());
    m_indexByReference.emplace(reference, m_assets.size());
    m_assets.push_back(std::move(asset));
}

void AssetDatabase::addProblem(std::string problem) {
    CORDITE_LOG_WARN("Assets", "{}", problem);
    m_problems.push_back(std::move(problem));
}

std::optional<Uuid> AssetDatabase::identify(const ScannedFile& file) {
    const std::filesystem::path metaPath = metaPathOf(file.absolutePath);
    const auto writeMeta = [&](const Uuid& id) {
        if (!m_desc.writeMetaFiles) {
            return;
        }
        if (const Status written = writeMetaFile(metaPath, id); !written) {
            addProblem(fmt::format("{}: cannot write its .meta file: {}", file.relativePath, written.error().message));
        }
    };

    if (fileExists(metaPath)) {
        const Result<Uuid> stored = readMetaFile(metaPath);
        if (!stored) {
            // Left alone rather than replaced: it may be half of a merge conflict, and a
            // new id would silently break every reference to the asset.
            addProblem(fmt::format("{}{}: {}; the asset is ignored until this is fixed", file.relativePath,
                                   kMetaExtension, stored.error().message));
            return std::nullopt;
        }
        const auto existing = m_indexById.find(*stored);
        if (existing == m_indexById.end()) {
            return *stored;
        }

        // Two files with one id: the second is a copy that brought its .meta along.
        const std::string& other = m_assets[existing->second].path;
        if (!m_desc.writeMetaFiles) {
            addProblem(fmt::format("{}: it has the same id as {} and is ignored (was it copied with its .meta file?)",
                                   file.relativePath, other));
            return std::nullopt;
        }
        const Uuid fresh = Uuid::generate();
        addProblem(fmt::format("{}: it had the same id as {} and was given a new one", file.relativePath, other));
        writeMeta(fresh);
        return fresh;
    }

    // No .meta yet. The id is derived from the path, so that the same files give the
    // same ids everywhere; only if that id is taken (by a file that used to live at
    // this path and kept its id when it moved) is a random one used.
    Uuid id = Uuid::fromName(kPathNamespace, file.relativePath);
    if (m_indexById.contains(id)) {
        id = Uuid::generate();
        if (!m_desc.writeMetaFiles) {
            addProblem(fmt::format("{}: it has no .meta file and gets a temporary id; open the project in the editor "
                                   "to give it a permanent one",
                                   file.relativePath));
        }
    }
    writeMeta(id);
    return id;
}

void AssetDatabase::addMaterialDependencies(const ScannedFile& file, AssetInfo& asset) {
    const auto fail = [&](const Error& error) { addProblem(fmt::format("{}: {}", file.relativePath, error.message)); };

    const Result<Json> document = readJsonFile(file.absolutePath);
    if (!document) {
        fail(document.error());
        return;
    }
    const Result<MaterialFile> material = parseMaterialFile(*document);
    if (!material) {
        fail(material.error());
        return;
    }

    if (!material->parent.isNil()) {
        asset.dependencies.push_back(material->parent);
    }
    // Only what the file itself states; what it inherits is the parent's dependency.
    MaterialData stated;
    material->properties.applyTo(&stated);
    for (const Uuid& texture : materialTextures(stated)) {
        asset.dependencies.push_back(texture);
    }
}

void AssetDatabase::addModelContents(const ScannedFile& file, const Uuid& modelId) {
    AssetInfo model{.id = modelId,
                    .type = AssetType::Model,
                    .name = displayName(file.relativePath, *file.kind),
                    .path = file.relativePath};

    // Only the structure: which meshes, materials and images the file holds.
    Result<GltfDocument> imported =
        importGltf(file.absolutePath, {.loadData = false, .rootDirectory = m_desc.rootDirectory});
    if (!imported) {
        // Still listed, so that references to it say "cannot be loaded" and not "unknown".
        addProblem(imported.error().describe());
        add(std::move(model));
        return;
    }
    const GltfDocument& document = *imported;
    model.warnings = document.warnings;

    const auto idOf = [&](std::string_view kind, size_t index) {
        return subAssetId(modelId, fmt::format("{}/{}", kind, index));
    };
    const auto subAsset = [&](AssetType type, std::string name, std::string_view kind, size_t index) {
        return AssetInfo{.id = idOf(kind, index),
                         .type = type,
                         .name = std::move(name),
                         .path = file.relativePath,
                         .subPath = fmt::format("{}/{}", kind, index)};
    };

    std::vector<AssetInfo> contents;
    for (size_t index = 0; index < document.meshes.size(); ++index) {
        const MeshData& mesh = document.meshes[index].mesh;
        if (!mesh.submeshes.empty()) {
            contents.push_back(subAsset(AssetType::Mesh, mesh.name, "mesh", index));
            model.dependencies.push_back(contents.back().id);
        }
    }
    for (size_t index = 0; index < document.materials.size(); ++index) {
        const GltfMaterial& material = document.materials[index];
        AssetInfo asset = subAsset(AssetType::Material, material.name, "material", index);

        for (const std::optional<uint32_t>& image :
             {material.images.baseColor, material.images.metallicRoughness, material.images.normal,
              material.images.occlusion, material.images.emissive}) {
            if (!image || *image >= document.images.size()) {
                continue;
            }
            const GltfImage& source = document.images[*image];
            if (source.isEmbedded()) {
                const Uuid texture = idOf("image", *image);
                if (std::ranges::find(asset.dependencies, texture) == asset.dependencies.end()) {
                    asset.dependencies.push_back(texture);
                }
            } else {
                m_pendingDependencies.push_back(
                    {.asset = asset.id,
                     .reference = resolveAssetReference(file.relativePath, source.uri),
                     .describedAs = fmt::format("the image '{}' of material '{}'", source.uri, material.name)});
            }
        }
        model.dependencies.push_back(asset.id);
        contents.push_back(std::move(asset));
    }
    for (size_t index = 0; index < document.images.size(); ++index) {
        if (document.images[index].isEmbedded()) {
            contents.push_back(subAsset(AssetType::Texture, document.images[index].name, "image", index));
        }
    }
    // A skin that could not be imported has no joints, and is no skeleton.
    for (size_t index = 0; index < document.skins.size(); ++index) {
        const SkeletonData& skeleton = document.skins[index].skeleton;
        if (!skeleton.joints.empty()) {
            contents.push_back(subAsset(AssetType::Skeleton, skeleton.name, "skeleton", index));
            model.dependencies.push_back(contents.back().id);
        }
    }
    for (size_t index = 0; index < document.animations.size(); ++index) {
        contents.push_back(
            subAsset(AssetType::Animation, document.animations[index].animation.name, "animation", index));
        model.dependencies.push_back(contents.back().id);
    }

    add(std::move(model));
    for (AssetInfo& asset : contents) {
        add(std::move(asset));
    }
}

void AssetDatabase::resolvePendingDependencies() {
    for (const PendingDependency& pending : m_pendingDependencies) {
        const auto owner = m_indexById.find(pending.asset);
        if (owner == m_indexById.end()) {
            continue;
        }
        AssetInfo& asset = m_assets[owner->second];

        const AssetInfo* target = pending.reference.empty() ? nullptr : findByReference(pending.reference);
        if (target == nullptr || target->type != AssetType::Texture) {
            addProblem(
                fmt::format("{}: {} is not a texture in the asset directory", asset.reference(), pending.describedAs));
            continue;
        }
        if (std::ranges::find(asset.dependencies, target->id) == asset.dependencies.end()) {
            asset.dependencies.push_back(target->id);
        }
    }
    m_pendingDependencies.clear();

    // A reference by id that leads nowhere: a texture that was deleted, a parent
    // material that was never committed.
    for (const AssetInfo& asset : m_assets) {
        for (const Uuid& dependency : asset.dependencies) {
            if (!m_indexById.contains(dependency)) {
                addProblem(fmt::format("{}: it refers to the asset {}, which does not exist", asset.reference(),
                                       dependency.toString()));
            }
        }
    }
}

Status AssetDatabase::refresh() {
    m_assets.clear();
    m_indexById.clear();
    m_indexByReference.clear();
    m_problems.clear();
    m_pendingDependencies.clear();
    ++m_revision;
    addBuiltinAssets();

    if (m_desc.rootDirectory.empty()) {
        return {};
    }
    if (!directoryExists(m_desc.rootDirectory)) {
        return makeError(ErrorCode::NotFound, "the asset directory {} does not exist",
                         pathToUtf8(m_desc.rootDirectory));
    }

    // Collected first and sorted, so that the result does not depend on the order in
    // which the file system happens to list a directory.
    std::vector<ScannedFile> files;
    std::vector<std::string> metaFiles;
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry(
        m_desc.rootDirectory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        const std::string name = pathToUtf8(entry->path().filename());
        std::error_code statusError;
        if (entry->is_directory(statusError)) {
            if (name.starts_with('.')) {
                entry.disable_recursion_pending();
            }
            continue;
        }
        if (!entry->is_regular_file(statusError)) {
            continue;
        }
        Result<std::string> relative = relativePathWithin(m_desc.rootDirectory, entry->path());
        if (!relative) {
            continue;
        }
        if (relative->ends_with(kMetaExtension)) {
            metaFiles.push_back(std::move(*relative));
        } else if (const FileKind* kind = kindOfFile(*relative)) {
            files.push_back({.relativePath = std::move(*relative), .absolutePath = entry->path(), .kind = kind});
        }
    }
    if (error) {
        return makeError(ErrorCode::IoError, "cannot read the asset directory {}: {}", pathToUtf8(m_desc.rootDirectory),
                         error.message());
    }
    std::ranges::sort(files, {}, &ScannedFile::relativePath);
    std::ranges::sort(metaFiles);

    for (const ScannedFile& file : files) {
        const std::optional<Uuid> id = identify(file);
        if (!id) {
            continue;
        }
        if (file.kind->type == AssetType::Model) {
            addModelContents(file, *id);
            continue;
        }

        AssetInfo asset{.id = *id,
                        .type = file.kind->type,
                        .name = displayName(file.relativePath, *file.kind),
                        .path = file.relativePath};
        if (asset.type == AssetType::Material) {
            addMaterialDependencies(file, asset);
        }
        add(std::move(asset));
    }
    resolvePendingDependencies();

    // A .meta whose asset is gone usually means the asset was deleted or renamed
    // without it.
    for (const std::string& meta : metaFiles) {
        const std::string asset = meta.substr(0, meta.size() - kMetaExtension.size());
        if (!std::ranges::binary_search(files, asset, {}, &ScannedFile::relativePath)) {
            addProblem(fmt::format("{}: there is no asset file for this description", meta));
        }
    }
    return {};
}

const AssetInfo* AssetDatabase::find(const Uuid& id) const {
    const auto found = m_indexById.find(id);
    return found != m_indexById.end() ? &m_assets[found->second] : nullptr;
}

const AssetInfo* AssetDatabase::findByReference(std::string_view reference) const {
    const auto found = m_indexByReference.find(std::string(reference));
    return found != m_indexByReference.end() ? &m_assets[found->second] : nullptr;
}

std::filesystem::path AssetDatabase::sourceFile(const AssetInfo& asset) const {
    if (asset.isBuiltin()) {
        return {};
    }
    return m_desc.rootDirectory / pathFromUtf8(asset.path);
}

} // namespace cordite
