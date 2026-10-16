#pragma once

/// @file
/// Moving and renaming the files of assets.
///
/// An asset is its id, which lives in the .meta file beside it: a file that moves with
/// its .meta keeps its id, and whatever refers to it by id - scenes, prefabs, materials -
/// follows it. What names a file by where it is does not. A glTF file names its buffers
/// and its images so, and a material in it a texture: moveAsset() refuses to break those.
/// The open scene's file follows the move. Scripts, and what scenes give them, may name
/// a file by where it is too; those are found and listed, and are changed by hand.
///
/// @code
/// CORDITE_TRY_ASSIGN(const AssetMove moved, moveAsset(context, wallId, "textures/brick/wall.png"));
/// for (const FileMention& mention : moved.files) {
///     CORDITE_LOG_WARN("Assets", "{}:{} still names {}", mention.file, mention.line, moved.from);
/// }
/// @endcode

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/world/Entity.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// A line of a text file of the project that names a file by where it was.
struct FileMention {
    /// The file, as AssetInfo::reference(): "scripts/hop.lua".
    std::string file;
    /// Counted from 1.
    uint32_t line = 0;
    /// The line, without the blanks at its ends.
    std::string text;
};

/// A value in a component of the open scene that names a file by where it was: a text
/// among the values a script is given, say.
struct EntityMention {
    Entity entity;
    /// The name of the component: "Script".
    std::string component;
    /// Where in the component, as a JSON pointer into its properties: "/properties/landing".
    std::string property;
};

/// What moving the file of an asset did.
struct AssetMove {
    Uuid asset;
    /// Where the file was and is, as AssetInfo::reference() has it.
    std::string from;
    std::string to;
    /// What still names the file by where it was: lines of the scripts, scenes and
    /// prefabs of the project, as their files are, and values in the open scene.
    std::vector<FileMention> files;
    std::vector<EntityMention> entities;
};

/// Moves the file of an asset to `to` - another name, another directory or both - with
/// its .meta, so that it keeps its id; a file that had no .meta yet gets one that keeps
/// the id it has, which was made of its path. `to` is a path inside the asset directory,
/// with forward slashes; directories are made as they are needed. Then the assets are read
/// again (AssetManager::refresh()), and the open scene's file follows when it was this
/// one. Files are no part of the document: undo does not move it back.
///
/// Fails, and moves nothing, with NotFound for an id that is no asset; with
/// InvalidArgument for an asset that is built in or inside a file (its file moves), for
/// a path that is absolute, has a part that is empty, "." or "..", or begins with a dot
/// (directories of that kind are never read for assets), that ends otherwise than the
/// file does (it would be another kind of file), that is where the file is already, for
/// a glTF file that names files by where they are and would leave them behind in another
/// directory, and for a texture that a glTF file names by where it is; with
/// AlreadyExists when another file, or another .meta, is there; and with IoError when
/// the file system refuses. A path that differs only in case is a rename. When the
/// assets cannot be read again after the move, the files have moved, and the error
/// says so.
///
/// `id` and `to` are the function's own: the assets are read again on the way, and an
/// id or a path that belonged to them would not outlive that.
[[nodiscard]] Result<AssetMove> moveAsset(EditorContext& context, Uuid id, std::string to);

/// Checks that `path` names a place inside the asset directory as references do: relative,
/// with forward slashes, and without a colon - a drive on Windows, or a stream of a file
/// that nobody would see. Fails with InvalidArgument for one that is empty, and for a part
/// that is empty, "." or "..", or begins with a dot: directories of that kind are never
/// read for assets.
[[nodiscard]] Status checkAssetPath(std::string_view path);

/// What moveAsset() did, as `asset.move` returns it: the id, `from`, `to`, and `mentions` -
/// lines of files (`file`, `line`, `text`), then values in the open scene (`entity`, `id`,
/// `component`, `property`).
[[nodiscard]] Json describeAssetMove(const EditorContext& context, const AssetMove& move);

} // namespace cordite
