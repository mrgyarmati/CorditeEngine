#include "cordite/engine/Game.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/platform/InputMap.h"

#include <utility>

namespace cordite {
namespace {

/// The settings in `file`. The defaults when there is no such file yet - written into it,
/// so that there is something to change - and when it cannot be read, which is said.
GameSettings readSettings(const std::filesystem::path& file) {
    const Result<GameSettings> settings = loadGameSettings(file);
    if (settings) {
        return *settings;
    }
    if (settings.error().code == ErrorCode::NotFound) {
        if (const Status saved = saveGameSettings(file, GameSettings{}); !saved) {
            CORDITE_LOG_WARN("Game", "the settings of the game could not be written: {}", saved.error().describe());
        }
        return {};
    }
    // Left as it is: what the person wrote there may be one mistake away from what they meant.
    CORDITE_LOG_WARN("Game", "the settings of the game cannot be read, and the defaults are used: {}",
                     settings.error().describe());
    return {};
}

/// What the overrides say over an engine that is made without settings.
void applyOverrides(const GameSettingsOverrides& overrides, EngineDesc& desc) {
    desc.window.fullscreen = overrides.fullscreen.value_or(desc.window.fullscreen);
    if (overrides.width) {
        desc.window.width = *overrides.width;
        desc.renderSize.x = *overrides.width;
    }
    if (overrides.height) {
        desc.window.height = *overrides.height;
        desc.renderSize.y = *overrides.height;
    }
    desc.vsync = overrides.vsync.value_or(desc.vsync);
}

} // namespace

Game::~Game() = default;

Result<std::unique_ptr<Game>> Game::create(GameDesc desc) {
    std::unique_ptr<Game> game(new Game());
    CORDITE_TRY_ASSIGN(game->m_project, loadProject(desc.directory));
    const Project& project = game->m_project;
    if (project.settings.startScene.empty()) {
        return makeError(ErrorCode::NotFound, "{} names no start scene, and a game starts with one",
                         pathToUtf8(project.file()));
    }

    EngineDesc engineDesc = std::move(desc.engine);
    if (!desc.settingsFile.empty()) {
        game->m_settings = withOverrides(readSettings(desc.settingsFile), desc.overrides);
        applyGameSettings(*game->m_settings, engineDesc);
    } else {
        applyOverrides(desc.overrides, engineDesc);
    }
    engineDesc.applicationName = project.settings.name;
    if (engineDesc.window.title == WindowDesc().title) {
        engineDesc.window.title = project.settings.name;
    }
    engineDesc.assetDirectory = project.assetDirectory();
    if (engineDesc.cacheDirectory.empty()) {
        engineDesc.cacheDirectory = project.cacheDirectory();
    }
    // A game reads its assets as they were given: ids are in their .meta files already.
    engineDesc.writeAssetMetaFiles = false;
    CORDITE_TRY_ASSIGN(game->m_engine, Engine::create(engineDesc));
    Engine& engine = *game->m_engine;
    if (game->m_settings) {
        applyGameSettings(*game->m_settings, engine);
    } else if (desc.overrides.quality) {
        applyRenderQuality(*desc.overrides.quality, engine.renderSettings());
    }

    // Read as strictly as the editor reads them: a game whose controls cannot be read does not start.
    if (!project.settings.inputMap.empty()) {
        CORDITE_TRY_ASSIGN(InputMap map, loadInputMap(project.inputMapPath()));
        engine.actions().setMap(std::move(map));
    }

    const AssetInfo* start = engine.assets().database().findByReference(project.settings.startScene);
    // Of another kind it is refused by loading it, which says of which.
    if (start == nullptr) {
        return makeError(ErrorCode::NotFound, "the start scene {} of {} is not among its assets",
                         project.settings.startScene, pathToUtf8(project.file()));
    }
    CORDITE_TRY(engine.loadScene(start->id));
    return game;
}

Result<bool> Game::runFrame() {
    return m_engine->runFrame();
}

} // namespace cordite
