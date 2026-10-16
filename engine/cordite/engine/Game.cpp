#include "cordite/engine/Game.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/FileSystem.h"
#include "cordite/platform/InputMap.h"

#include <utility>

namespace cordite {

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
