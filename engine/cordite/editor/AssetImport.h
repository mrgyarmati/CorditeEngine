#pragma once

/// @file
/// Bringing files from elsewhere into the assets of a project.
///
/// A file is brought in by being copied into the asset directory; the assets are then
/// read again, and it is one of them, with an id of its own. A glTF file brings along
/// what it names by where it is - its buffers, its images - to the same places beside
/// it. What a file was called by in another project, its .meta, stays behind: in this
/// one it is new.
///
/// @code
/// CORDITE_TRY_ASSIGN(const AssetImport imported,
///                    importAssets(context, {downloads / "crate.glb", downloads / "rust.png"}, "props"));
/// for (const std::string& file : imported.files) {
///     CORDITE_LOG_INFO("Assets", "brought in {}", file);
/// }
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/editor/EditorContext.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cordite {

/// What importAssets() brought in.
struct AssetImport {
    /// Where each file went, as AssetInfo::reference() has it: the files as they were given,
    /// each followed by what a glTF file brought along. A buffer is no asset, and is here
    /// all the same.
    std::vector<std::string> files;
};

/// Copies `files` - from anywhere but the asset directory - into the directory `to` of the
/// assets, under their own names, and reads the assets again. `to` is a path inside the
/// asset directory as asset.move takes one, or empty for the top; directories are made as
/// they are needed. A glTF file brings the files it names by where they are
/// (GltfDocument::externalFiles) to the same places beside it. Everything is checked before
/// anything is copied, and what was copied is taken away again when a later copy fails.
/// Not undoable: files are no part of the document.
///
/// Fails, and copies nothing, with InvalidArgument for no files, for a file that is a
/// directory, is among the assets already, or is no asset by its ending, for two files
/// that would go to one place, for a `to` that is no place inside the asset directory,
/// for a glTF file that cannot be read or names a file that would land outside the assets;
/// with NotFound for a file that is not there; with AlreadyExists when a place holds a
/// file; and with IoError when copying fails. When the assets cannot be read again
/// afterwards, the files have been copied, and the error says so.
[[nodiscard]] Result<AssetImport> importAssets(EditorContext& context, const std::vector<std::filesystem::path>& files,
                                               std::string_view to);

} // namespace cordite
