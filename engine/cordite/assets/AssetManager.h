#pragma once

/// @file
/// Loads assets by id and keeps them, so that everything that uses an asset shares one copy.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto assets, AssetManager::create({.rootDirectory = projectDir / "assets"}));
///
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const MeshData> mesh, assets->loadMesh(meshId));
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const MaterialData> material, assets->loadMaterial(materialId));
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const Image> pixels, assets->loadTexture(material->baseColorTexture));
///
/// CORDITE_TRY(assets->refresh());     // after files were added, changed or removed
/// @endcode
///
/// The result is CPU-side data; turning it into GPU resources is the renderer's
/// business. A model file is imported once, on the first request for anything in it,
/// and all of its meshes and materials are kept from then on. Decoded images are only
/// kept while something else holds them: once a texture is on the GPU its pixels are of
/// no further use. (Images stored inside a model file stay in memory in their encoded
/// form, as part of the model.) The samples of a sound are what is played, again and
/// again, so they are kept like a mesh.
///
/// Loading happens on the calling thread and is not thread-safe.

#include "cordite/assets/Animation.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Model.h"
#include "cordite/assets/Prefab.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cordite {

class AssetManager {
public:
    /// Indexes the asset directory. Fails when it cannot be read; problems with single
    /// files are in database().problems().
    [[nodiscard]] static Result<std::unique_ptr<AssetManager>> create(AssetDatabaseDesc desc = {});
    ~AssetManager();
    CORDITE_NON_MOVABLE(AssetManager);

    /// What exists. Loading is not needed to list, name or look up assets.
    [[nodiscard]] const AssetDatabase& database() const { return m_database; }

    /// Indexes the directory again and forgets everything that was loaded, so that the
    /// next request reads the files as they are now. Data handed out earlier stays valid
    /// for whoever holds it.
    [[nodiscard]] Status refresh();

    /// Reads again which assets a scene or a prefab refers to, after its file was written,
    /// without reading anything else: AssetDatabase::rereadDependencies(). What was loaded
    /// stays as it is.
    [[nodiscard]] Status rereadDependencies(const Uuid& id) { return m_database.rereadDependencies(id); }

    /// Each load fails with NotFound for an id that names no asset, with InvalidArgument
    /// for an asset of another type, and with the reader's error for a file that cannot
    /// be read; the message names the asset.
    [[nodiscard]] Result<std::shared_ptr<const MeshData>> loadMesh(const Uuid& id);
    /// A material's parent, if it names one, is loaded and applied first.
    [[nodiscard]] Result<std::shared_ptr<const MaterialData>> loadMaterial(const Uuid& id);
    [[nodiscard]] Result<std::shared_ptr<const Image>> loadTexture(const Uuid& id);
    [[nodiscard]] Result<std::shared_ptr<const ModelData>> loadModel(const Uuid& id);
    /// A skeleton of a model file, and one of its animations. Both have been through
    /// validateSkeleton() and validateAnimation().
    [[nodiscard]] Result<std::shared_ptr<const SkeletonData>> loadSkeleton(const Uuid& id);
    [[nodiscard]] Result<std::shared_ptr<const AnimationData>> loadAnimation(const Uuid& id);
    /// The text of a script file. Fails with InvalidArgument for a file of more than
    /// kMaxScriptBytes.
    [[nodiscard]] Result<std::shared_ptr<const ScriptSource>> loadScript(const Uuid& id);
    /// The samples of a sound file, decoded: all of it, at the rate the file has. Fails
    /// with InvalidArgument for a file of more than kMaxSoundFileBytes, and as
    /// decodeSound() does.
    [[nodiscard]] Result<std::shared_ptr<const SoundData>> loadSound(const Uuid& id);
    /// What a prefab file holds. Fails with InvalidArgument for a file of more than
    /// kMaxPrefabFileBytes, and as readJsonFile() and parsePrefab() do. A prefab whose
    /// file gives it no name has that of the file.
    [[nodiscard]] Result<std::shared_ptr<const PrefabData>> loadPrefab(const Uuid& id);

private:
    explicit AssetManager(AssetDatabaseDesc desc);

    [[nodiscard]] Result<const AssetInfo*> lookUp(const Uuid& id, AssetType type) const;
    /// The file of an asset that is read whole, once it is known to have `maxBytes` at
    /// most: a file of any size can be given the name of a script. Fails with IoError
    /// for a file that cannot be asked, and with InvalidArgument for one that is larger;
    /// `what` is what the message calls such a file, "a script".
    [[nodiscard]] Result<std::filesystem::path> sourceFileWithin(const AssetInfo& asset, size_t maxBytes,
                                                                 std::string_view what) const;
    [[nodiscard]] Status importModelFile(const AssetInfo& asset);
    [[nodiscard]] Result<std::shared_ptr<const MaterialData>> loadMaterial(const Uuid& id, uint32_t depth);
    [[nodiscard]] Result<MaterialData> readMaterialFile(const AssetInfo& asset, uint32_t depth);

    AssetDatabase m_database;
    std::unordered_map<Uuid, std::shared_ptr<const MeshData>> m_meshes;
    std::unordered_map<Uuid, std::shared_ptr<const MaterialData>> m_materials;
    std::unordered_map<Uuid, std::shared_ptr<const ModelData>> m_models;
    std::unordered_map<Uuid, std::shared_ptr<const SkeletonData>> m_skeletons;
    std::unordered_map<Uuid, std::shared_ptr<const AnimationData>> m_animations;
    std::unordered_map<Uuid, std::shared_ptr<const ScriptSource>> m_scripts;
    std::unordered_map<Uuid, std::shared_ptr<const SoundData>> m_sounds;
    std::unordered_map<Uuid, std::shared_ptr<const PrefabData>> m_prefabs;
    std::unordered_map<Uuid, std::weak_ptr<const Image>> m_textures;
    /// The encoded bytes of images that are stored inside model files.
    std::unordered_map<Uuid, std::shared_ptr<const std::vector<std::byte>>> m_embeddedImages;
    /// Model files that have been imported, by the id of the file, with how it went. A
    /// file that failed is not read again for every mesh in it.
    std::unordered_map<Uuid, Status> m_importedFiles;
};

} // namespace cordite
