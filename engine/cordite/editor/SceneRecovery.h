#pragma once

/// @file
/// What the editor keeps of a scene it cannot go on with - the GPU was lost - so that the
/// changes that were not saved are not lost with it.
///
/// The scene lives on the CPU, where a lost GPU leaves it as it was: the editor writes it,
/// as a scene file would hold it, into recovery.json in its state directory (the project's
/// .cordite), with the file the scene was of. The next editor on that project finds it and
/// asks whether to open it or let it go (EditorApp; `scene.recover`).
///
/// @code
/// const std::filesystem::path file = project.cacheDirectory() / kSceneRecoveryFileName;
/// CORDITE_TRY(writeSceneRecovery(file, {.scenePath = context.scenePath(), .scene = context.documentScene()}));
/// CORDITE_TRY_ASSIGN(const SceneRecovery kept, readSceneRecovery(file));
/// CORDITE_TRY(context.restoreScene(kept.scene, kept.scenePath));
/// @endcode
///
/// The file is a document like the engine's others (format "cordite.recovery", version 1),
/// and is read as strictly: a member that is not known, or of the wrong kind, is an error
/// that names it. The scene in it is checked when it is restored, as a scene file is.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <filesystem>
#include <string_view>

namespace cordite {

/// The name of the file of a kept scene, in the editor's state directory.
inline constexpr std::string_view kSceneRecoveryFileName = "recovery.json";

/// A scene the editor kept when it could not go on.
struct SceneRecovery {
    /// The file the scene was opened from or last saved to; empty for one that never had
    /// a file.
    std::filesystem::path scenePath;
    /// The scene, as a scene file holds one (EditorContext::documentScene()).
    Json scene;
};

/// Writes `recovery` to `file`, and the directories it is in, replacing what was there whole
/// (writeJsonFile). Fails as writing the file fails.
[[nodiscard]] Status writeSceneRecovery(const std::filesystem::path& file, const SceneRecovery& recovery);

/// Reads what writeSceneRecovery() wrote. Fails with NotFound for a file that is not there,
/// with ParseError for what is no such document, names a member that is not known or holds
/// no object as its scene, with VersionMismatch for one of a newer editor, and as reading
/// the file fails.
[[nodiscard]] Result<SceneRecovery> readSceneRecovery(const std::filesystem::path& file);

} // namespace cordite
