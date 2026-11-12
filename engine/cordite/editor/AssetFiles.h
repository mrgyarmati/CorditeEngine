#pragma once

/// @file
/// Making and taking away files of the assets from the editor: a directory, a material, and
/// the deletion of an asset's file.
///
/// Each checks everything before it touches a file, keeps every path inside the asset
/// directory, and reads the assets again after. None is undoable: files are no part of the
/// document, as asset.move and asset.import say too.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::string folder, createAssetDirectory(context, "materials/metal"));
/// CORDITE_TRY_ASSIGN(const Uuid rust, createMaterial(context, "materials/metal/rust.material.json"));
/// CORDITE_TRY_ASSIGN(const AssetDeletion deleted, deleteAsset(context, rust, /*force=*/false));
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// What uses the assets of one file, and would be left without them were it deleted.
struct AssetUsers {
    /// The other assets whose dependencies name the asset or anything inside its file: a
    /// material that has a texture, a scene or a prefab whose entities refer to it. In
    /// the order of AssetDatabase::assets(), each once.
    std::vector<Uuid> assets;
    /// How many properties of the open scene refer to the asset or anything inside its
    /// file - which a scene on the disk does not say until it is saved.
    size_t sceneReferences = 0;

    [[nodiscard]] bool empty() const { return assets.empty() && sceneReferences == 0; }
};

/// What deleteAsset() took away.
struct AssetDeletion {
    /// The file, as AssetInfo::path has it.
    std::string path;
    /// What used it, which now refers to an asset that is not there.
    AssetUsers users;
};

/// What uses the asset of `id` and whatever else is in its file. Fails with NotFound for an
/// id that is no asset.
[[nodiscard]] Result<AssetUsers> findAssetUsers(const EditorContext& context, const Uuid& id);

/// Deletes the file of the asset of `id` and its .meta, and reads the assets again: a model
/// goes with all that is in it. What a glTF file names by where it is - its buffers and
/// images - stays, since images are assets of their own. Refused, deleting nothing, while
/// the scene is played, with InvalidArgument for what is built in or inside a file (delete
/// the file), with NotFound for an id that is no asset, with InvalidState for the file of
/// the scene that is open, and with InvalidState for an asset that something uses - which
/// the message lists - unless `force`. Fails with IoError when the file cannot be deleted.
[[nodiscard]] Result<AssetDeletion> deleteAsset(EditorContext& context, const Uuid& id, bool force);

/// Makes a directory of the assets, and the directories it is in, and reads the assets
/// again: the Assets panel shows it, empty (AssetDatabase::directories()). `path` is a place
/// inside the asset directory, as asset.move takes one. Returns it. Refused with
/// InvalidState without an asset directory, with InvalidArgument for a path that
/// checkAssetPath() refuses, and with AlreadyExists where there is a file or a directory.
[[nodiscard]] Result<std::string> createAssetDirectory(EditorContext& context, std::string_view path);

/// Writes a material with every property at its default (serializeMaterial(MaterialData{}))
/// at `path`, a place inside the asset directory that ends in ".material.json", makes the
/// directories it is in, and reads the assets again. Returns the id the new material was
/// given. Refused with InvalidState without an asset directory, with InvalidArgument for a
/// path that checkAssetPath() refuses or that is no material by its ending, and with
/// AlreadyExists where there is a file.
[[nodiscard]] Result<Uuid> createMaterial(EditorContext& context, std::string_view path);

/// The first of `base`, "base 2", "base 3" ... - followed by `ending` - that no file or
/// directory of `directory` of the assets is called: where the Assets panel makes a new
/// folder or material. `directory` is empty for the top.
[[nodiscard]] std::string freeAssetPath(const EditorContext& context, std::string_view directory, std::string_view base,
                                        std::string_view ending);

} // namespace cordite
