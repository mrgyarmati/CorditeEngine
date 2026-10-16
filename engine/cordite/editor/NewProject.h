#pragma once

/// @file
/// A project to begin a game with: what File > New project and `cordite-editor
/// --new-project` make.
///
/// It is a project (createProject()) with one scene, its start scene: a camera that looks
/// at a box on a floor, under a sun that casts shadows, with light from the sky for what
/// the sun does not reach. The scene uses the built-in meshes and material, so that the
/// project has no other file; played or exported as it is, it shows something.
///
/// @code
/// CORDITE_TRY_ASSIGN(const Project project, createStarterProject("games/depot", "Depot"));
/// // project.settings.startScene == kStarterScene
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/engine/Project.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cordite {

/// The start scene of a project that createStarterProject() makes, inside its assets.
inline constexpr std::string_view kStarterScene = "scenes/main.scene.json";

/// Makes a project called `name` in `directory` - which may be there already, but must not
/// hold a project - with a start scene (see the file comment). An empty name is the name of
/// the directory. Fails with AlreadyExists for a directory that holds a project, and as
/// createProject() and writing the scene fail; what was written is taken away then.
[[nodiscard]] Result<Project> createStarterProject(const std::filesystem::path& directory, std::string name);

} // namespace cordite
