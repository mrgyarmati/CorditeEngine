#pragma once

/// @file
/// A game made of a project, as a player runs it: no editor, and no shader compiler.
///
/// A game is a directory: its project file, its assets, and in the project's cache the
/// engine's shaders, compiled (compileEngineShaders()) - what an export writes.
/// Game::create() reads the project, makes an engine of it on that cache, gives the engine
/// the controls of the project and puts in its start scene; runFrame() plays a frame.
/// cordite-player is a thin main around it. A game that someone plays has their settings
/// (GameSettings.h): the program names the file, the game reads it as it starts.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<Game> game,
///                    Game::create({.directory = gameDirectory, .settingsFile = gameSettingsFile("Depot")}));
/// while (true) {
///     CORDITE_TRY_ASSIGN(const bool keepRunning, game->runFrame());
///     if (!keepRunning) {
///         break;   // the window was closed
///     }
/// }
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/GameSettings.h"
#include "cordite/engine/Project.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace cordite {

struct GameDesc {
    /// Where the game is: the directory of its project file.
    std::filesystem::path directory;

    /// How the engine is made: a window or none, its size, validation, a fixed time step.
    /// What the game says is filled in from its project - its name, its assets, and its
    /// cache (Project::cacheDirectory()) unless one is given here - and it writes nothing
    /// into its assets. Without a `shaderCompiler`, which a game is not given, its shaders
    /// come from its cache alone.
    EngineDesc engine;

    /// The file in which the person who plays keeps their settings - usually the game's own
    /// (gameSettingsFile()). Given one, the game reads it as it starts: when there is no
    /// such file yet it plays with the defaults and writes them into it, for the player to
    /// change; when the file cannot be read it plays with the defaults, says why, and
    /// leaves the file as it is. The settings then decide over `engine` whether the window
    /// fills the screen, its size, and vsync, and over the engine the quality of its
    /// pictures and the volumes of its sound. Empty for none, which is what a game that
    /// runs unattended is given: it is made as `engine` says.
    std::filesystem::path settingsFile;

    /// What wins, for this start, over the settings of the file - or over `engine`, without
    /// one - and is not written into the file: what the command line said.
    GameSettingsOverrides overrides;
};

/// Where the game of `program` is when it is not told: in Contents/Resources of the
/// application bundle of macOS that the program is in (applicationBundleOf()), where an
/// export puts it there (GameExport.h), and otherwise in the directory of the program.
[[nodiscard]] std::filesystem::path gameDirectoryOf(const std::filesystem::path& program);

class Game {
public:
    /// Fails as loadProject() does for a directory that holds no project; with NotFound
    /// for a project that names no start scene or one that its assets do not have; as
    /// reading the controls of the project fails; as Engine::create() does - with NotFound
    /// for a shader that the cache does not have; and as Engine::loadScene() does - with
    /// InvalidArgument for a start scene that is another kind of asset.
    [[nodiscard]] static Result<std::unique_ptr<Game>> create(GameDesc desc);
    ~Game();
    CORDITE_NON_MOVABLE(Game);

    /// Plays a frame (Engine::runFrame()). False once the game is to end: its window was
    /// closed, or something asked the engine to stop.
    [[nodiscard]] Result<bool> runFrame();

    [[nodiscard]] Engine& engine() { return *m_engine; }

    [[nodiscard]] const Project& project() const { return m_project; }

    /// The settings the game started with: those of its file, with the overrides. None for
    /// a game without a settings file.
    [[nodiscard]] const std::optional<GameSettings>& settings() const { return m_settings; }

private:
    Game() = default;

    Project m_project;
    std::unique_ptr<Engine> m_engine;
    std::optional<GameSettings> m_settings;
};

} // namespace cordite
