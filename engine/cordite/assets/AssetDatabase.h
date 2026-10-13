#pragma once

/// @file
/// Which assets exist: the index of a project's asset directory.
///
/// An asset is identified by a Uuid that stays the same when its file is renamed or
/// moved. For a file, that id is stored next to it in `<file>.meta`; for something inside
/// a file (a mesh of a glTF model) it is derived from the file's id and the thing's
/// position in the file. Scenes and materials refer to assets by id only.
///
/// @code
/// AssetDatabase database({.rootDirectory = projectDir / "assets", .writeMetaFiles = true});
/// CORDITE_TRY(database.refresh());
///
/// for (const AssetInfo& asset : database.assets()) {
///     CORDITE_LOG_INFO("Assets", "{} {} {}", asset.id.toString(), nameOf(asset.type), asset.reference());
/// }
/// const AssetInfo* crate = database.findByReference("models/crate.gltf#mesh/0");
/// const AssetInfo* cube = database.find(builtinMeshId(BuiltinMesh::Cube));
/// @endcode
///
/// File types: `.gltf` and `.glb` are models, which contain meshes and materials (and
/// textures, when images are embedded; skeletons and animations, when they are
/// skinned); `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp` and `.hdr`
/// are textures; `.material.json` is a material; `.scene.json` is a scene; `.lua` is a
/// script. Other files are ignored, as are directories whose names start with a dot.
///
/// A file that has no `.meta` yet gets an id derived from its path, so that a freshly
/// generated project has the same ids on every machine. The `.meta` then keeps that id
/// when the file moves. Commit `.meta` files together with their assets.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cordite {

enum class AssetType : uint8_t {
    Texture,
    Mesh,
    Material,
    /// A hierarchy of meshes that can be placed in a scene as a group of entities.
    Model,
    Scene,
    /// The joints a skinned mesh hangs on.
    Skeleton,
    /// A clip that moves the joints of a skeleton.
    Animation,
    /// The text of a script: what an entity does while the game runs.
    Script,
};

inline constexpr std::array<AssetType, 8> kAssetTypes = {
    AssetType::Texture, AssetType::Mesh,     AssetType::Material,  AssetType::Model,
    AssetType::Scene,   AssetType::Skeleton, AssetType::Animation, AssetType::Script,
};

/// The lowercase name used in files and by tools: "texture", "mesh", ...
std::string_view nameOf(AssetType type);
/// The name with a capital, as PropertyAttributes::referenceType spells it: "Texture".
std::string_view displayNameOf(AssetType type);
/// The name as a sentence speaks of one of the kind, for messages: "a mesh", "an
/// animation".
std::string_view withArticle(AssetType type);
std::optional<AssetType> assetTypeFromName(std::string_view name);

/// The meshes every project has without any files.
enum class BuiltinMesh : uint8_t {
    Cube,
    Sphere,
    Plane,
    Cylinder,
    Cone,
    Capsule,
    Torus,
};

inline constexpr std::array<BuiltinMesh, 7> kBuiltinMeshes = {
    BuiltinMesh::Cube, BuiltinMesh::Sphere,  BuiltinMesh::Plane, BuiltinMesh::Cylinder,
    BuiltinMesh::Cone, BuiltinMesh::Capsule, BuiltinMesh::Torus,
};

std::string_view nameOf(BuiltinMesh mesh);

/// The id of a built-in mesh. It is the same in every project and every version.
[[nodiscard]] Uuid builtinMeshId(BuiltinMesh mesh);

/// The id of the material a surface has when it names none: plain white, neither metal
/// nor mirror (a MaterialData with every value at its default).
[[nodiscard]] Uuid defaultMaterialId();

/// The `path` of assets that are built into the engine.
inline constexpr std::string_view kBuiltinAssetPath = "builtin";

struct AssetInfo {
    Uuid id;
    AssetType type = AssetType::Texture;
    /// What to show a person: the file's name without extensions, or the name the asset
    /// has inside its file.
    std::string name;
    /// The file the asset is in, relative to the asset directory, with forward slashes.
    /// kBuiltinAssetPath for a built-in asset.
    std::string path;
    /// For an asset inside a file: what it is there and its index, such as "mesh/2".
    std::string subPath;
    /// The assets this one needs: a material its textures and its parent, a model its
    /// meshes and materials.
    std::vector<Uuid> dependencies;
    /// For a model: what its file contains that the importer left out or changed.
    std::vector<std::string> warnings;

    /// `path`, followed by `#` and `subPath` for an asset inside a file. Unique, and
    /// accepted by AssetDatabase::findByReference().
    [[nodiscard]] std::string reference() const;

    [[nodiscard]] bool isBuiltin() const { return path == kBuiltinAssetPath; }
};

struct AssetDatabaseDesc {
    /// The directory that holds the asset files. Empty for a database that only knows
    /// the built-in assets.
    std::filesystem::path rootDirectory;

    /// Create a `.meta` file for every asset that has none, and give a copied asset an
    /// id of its own. Editors do; a running game and most tests only read.
    bool writeMetaFiles = false;
};

class AssetDatabase {
public:
    /// Knows only the built-in assets until refresh() is called.
    explicit AssetDatabase(AssetDatabaseDesc desc = {});

    /// Reads the asset directory again. Fails only when the directory itself cannot be
    /// read; a problem with one file leaves that file out and is listed in problems().
    [[nodiscard]] Status refresh();

    [[nodiscard]] const AssetInfo* find(const Uuid& id) const;

    /// Finds an asset by AssetInfo::reference(), for example "models/crate.gltf" or
    /// "models/crate.gltf#material/0". Upper and lower case are distinguished on every
    /// platform, so that a project made on Windows works on Linux.
    [[nodiscard]] const AssetInfo* findByReference(std::string_view reference) const;

    /// Every asset: the built-in ones first, then files in the order of their paths, each
    /// followed by what it contains.
    [[nodiscard]] std::span<const AssetInfo> assets() const { return m_assets; }

    /// What the last refresh() found wrong, one sentence per problem, each naming a file.
    [[nodiscard]] std::span<const std::string> problems() const { return m_problems; }

    [[nodiscard]] const std::filesystem::path& rootDirectory() const { return m_desc.rootDirectory; }

    /// The file an asset is in. Empty for a built-in asset.
    [[nodiscard]] std::filesystem::path sourceFile(const AssetInfo& asset) const;

    /// Changes whenever refresh() ran. Caches of loaded assets compare it to know when
    /// what they hold may be stale.
    [[nodiscard]] uint64_t revision() const { return m_revision; }

private:
    struct ScannedFile;

    void addBuiltinAssets();
    void add(AssetInfo asset);
    void addProblem(std::string problem);
    [[nodiscard]] std::optional<Uuid> identify(const ScannedFile& file);
    void addModelContents(const ScannedFile& file, const Uuid& modelId);
    void addMaterialDependencies(const ScannedFile& file, AssetInfo& asset);
    void resolvePendingDependencies();

    AssetDatabaseDesc m_desc;
    std::vector<AssetInfo> m_assets;
    std::unordered_map<Uuid, size_t> m_indexById;
    std::unordered_map<std::string, size_t> m_indexByReference;
    std::vector<std::string> m_problems;

    /// Dependencies named by file path, which can only be turned into ids once every
    /// file has been seen.
    struct PendingDependency {
        Uuid asset;
        std::string reference;
        std::string describedAs;
    };

    std::vector<PendingDependency> m_pendingDependencies;
    uint64_t m_revision = 0;
};

/// The reference of the image a glTF file calls `uri`: the path made relative to the
/// asset directory instead of to the glTF file. Empty when that leads outside it.
[[nodiscard]] std::string resolveAssetReference(std::string_view fromFile, std::string_view relativeUri);

/// The id of something inside a file: derived from the file's id and the sub-path, such
/// as "mesh/2".
[[nodiscard]] Uuid subAssetId(const Uuid& fileId, std::string_view subPath);

} // namespace cordite
