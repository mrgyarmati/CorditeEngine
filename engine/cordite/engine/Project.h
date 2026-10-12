#pragma once

/// @file
/// A project: the directory a game is made in.
///
/// ```
/// my-game/
///     cordite.project.json     what the project is called and where it starts
///     assets/                  everything the game is made of (see AssetDatabase)
///     .cordite/                what the engine derives and remembers; not for version control
/// ```
///
/// @code
/// CORDITE_TRY_ASSIGN(const Project project, loadProject("samples/showcase"));
///
/// EngineDesc desc;
/// desc.applicationName = project.settings.name;
/// desc.assetDirectory = project.assetDirectory();
/// CORDITE_TRY_ASSIGN(auto engine, Engine::create(desc));
/// CORDITE_TRY(loadScene(project.startScenePath(), engine->world(), engine->components()));
/// @endcode
///
/// The project file:
///
/// @code{.json}
/// {
///     "format": "cordite.project",
///     "version": 1,
///     "name": "Showcase",
///     "startScene": "scenes/showcase.scene.json",
///     "inputMap": "input/controls.input.json"
/// }
/// @endcode

#include "cordite/core/Reflection.h"
#include "cordite/core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace cordite {

/// The name of the file that makes a directory a project.
inline constexpr std::string_view kProjectFileName = "cordite.project.json";

/// What cordite.project.json holds.
struct ProjectSettings {
    /// Shown in window titles and used to name exported games.
    std::string name = "Untitled";
    /// The scene the game starts with and the editor opens: a file inside the asset
    /// directory, relative to it, with forward slashes. Empty for none.
    std::string startScene;
    /// The controls of the game - its actions and axes, and what moves them
    /// (InputMap.h): a file inside the asset directory, named as the start scene is.
    /// Empty for none.
    std::string inputMap;

    bool operator==(const ProjectSettings&) const = default;
};

[[nodiscard]] const TypeInfo& projectSettingsTypeInfo();

struct Project {
    /// The directory that holds cordite.project.json.
    std::filesystem::path directory;
    ProjectSettings settings;

    [[nodiscard]] std::filesystem::path file() const;
    [[nodiscard]] std::filesystem::path assetDirectory() const;
    /// Where derived data goes: caches, and what the editor remembers between sessions.
    [[nodiscard]] std::filesystem::path cacheDirectory() const;
    /// The file of the start scene; empty when the project names none.
    [[nodiscard]] std::filesystem::path startScenePath() const;
    /// The file of the input map; empty when the project names none.
    [[nodiscard]] std::filesystem::path inputMapPath() const;
};

/// Reads a project. `path` is the project directory or its cordite.project.json. Fails
/// with NotFound when there is no project file, and with ParseError when the file is
/// not a valid one - including a start scene or an input map that would lie outside
/// the asset directory. The files that a project names are not read here.
[[nodiscard]] Result<Project> loadProject(const std::filesystem::path& path);

/// Writes cordite.project.json, creating the project and asset directories if needed.
[[nodiscard]] Status saveProject(const Project& project);

/// Makes a new, empty project in `directory`. Fails with AlreadyExists when the
/// directory holds a project already.
[[nodiscard]] Result<Project> createProject(const std::filesystem::path& directory, std::string name);

} // namespace cordite
