#pragma once

/// @file
/// A game made of a project, as a player runs it: no editor, and no shader compiler.
///
/// A game is a directory: its project file, its assets, and in the project's cache the
/// engine's shaders, compiled (compileEngineShaders()) - what an export writes.
/// Game::create() reads the project, makes an engine of it on that cache, gives the engine
/// the controls of the project and puts in its start scene; runFrame() plays a frame.
/// cordite-player is a thin main around it.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<Game> game, Game::create({.directory = gameDirectory}));
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
#include "cordite/engine/Project.h"

#include <filesystem>
#include <memory>

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
};

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

private:
    Game() = default;

    Project m_project;
    std::unique_ptr<Engine> m_engine;
};

} // namespace cordite
