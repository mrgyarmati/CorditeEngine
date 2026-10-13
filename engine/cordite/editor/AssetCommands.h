#pragma once

/// @file
/// The editor commands that deal with assets.
///
/// | Command            | Does                                                                  |
/// |--------------------|-----------------------------------------------------------------------|
/// | `asset.list`       | Lists the assets, optionally of one type.                             |
/// | `asset.get`        | Describes one asset: what it needs, and what is in it.                |
/// | `asset.refresh`    | Reads the asset directory again after files changed.                  |
/// | `asset.instantiate`| Places a model or a prefab in the scene as a group of entities.       |
/// | `prefab.create`    | Makes a prefab of an entity and everything below it.                  |
/// | `scene.validate`   | Finds references to assets that are missing or of the wrong kind.     |
///
/// @code
/// CORDITE_TRY(registerAssetCommands(commands, context));
///
/// const Json meshes = unwrap(commands.execute("asset.list", {{"type", "mesh"}}));
/// const Json placed = unwrap(commands.execute("asset.instantiate", {{"asset", "models/crate.gltf"}}));
/// @endcode
///
/// registerEditorCommands() registers these along with all the others.

#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/editor/EditorContext.h"

namespace cordite {

/// Registers the commands above. The registry and the context must outlive each other's
/// use: the handlers refer to the context.
[[nodiscard]] Status registerAssetCommands(CommandRegistry& commands, EditorContext& context);

} // namespace cordite
