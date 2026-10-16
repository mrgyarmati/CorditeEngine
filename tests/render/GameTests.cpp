#include "cordite/engine/Game.h"

#include "cordite/core/FileSystem.h"
#include "cordite/engine/Project.h"

#include "render/RenderTestSupport.h"
#include "support/TestSupport.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A game as a player is given one: the showcase, with the engine's shaders compiled ahead
/// into a cache of its own, and no compiler.
GameDesc showcaseGame(const std::filesystem::path& cache) {
    GameDesc desc;
    desc.directory = test::sourceDirectory() / "samples" / "showcase";
    desc.engine.headless = true;
    desc.engine.validation = true;
    desc.engine.renderSize = UVec2(320, 180);
    desc.engine.fixedTimeStep = 1.0 / 60.0;
    desc.engine.cacheDirectory = cache;
    return desc;
}

/// The error of a game that does not start.
Error refusal(GameDesc desc) {
    const Result<std::unique_ptr<Game>> game = Game::create(std::move(desc));
    REQUIRE_FALSE(game.has_value());
    return game.error();
}

} // namespace

TEST_SUITE("Game") {
    TEST_CASE("a game plays its start scene with its controls, on the shaders compiled ahead of it") {
        const std::unique_ptr<Game> game = unwrap(Game::create(showcaseGame(test::compiledShaderCache())));
        Engine& engine = game->engine();
        CHECK(game->project().settings.name == "Showcase");
        CHECK(engine.world().sceneName() == "Showcase");
        CHECK_FALSE(engine.actions().map().actions.empty());

        // A second and a half: its scripts start, and the self-test of the showcase runs.
        for (int frame = 0; frame < 90; ++frame) {
            REQUIRE(unwrap(game->runFrame()));
        }
        CHECK(engine.scripts().problems().empty());
        CHECK(engine.shaders().stats().compilations == 0);
        CHECK(engine.shaders().stats().diskHits > 0);
        test::checkNoValidationIssues(engine.graphics());
    }

    TEST_CASE("a game finds its shaders in its own cache, and writes nothing into its assets") {
        const TempDirectory directory;
        Project project = unwrap(createProject(directory.path() / "game", "Small"));
        // A scene without a .meta file: its id follows from its path, and stays unwritten.
        const std::filesystem::path scene = project.assetDirectory() / "scenes" / "start.scene.json";
        REQUIRE(writeTextFile(scene, R"({"format": "cordite.scene", "version": 1, "name": "Small", "entities": []})")
                    .has_value());
        project.settings.startScene = "scenes/start.scene.json";
        REQUIRE(saveProject(project).has_value());
        REQUIRE(ensureDirectory(project.cacheDirectory()).has_value());
        std::filesystem::copy(test::compiledShaderCache() / "shaders", project.cacheDirectory() / "shaders",
                              std::filesystem::copy_options::recursive);

        GameDesc desc = showcaseGame({});
        desc.directory = project.directory;
        const std::unique_ptr<Game> game = unwrap(Game::create(std::move(desc)));
        REQUIRE(unwrap(game->runFrame()));
        CHECK(game->engine().world().sceneName() == "Small");
        CHECK(game->engine().shaders().stats().compilations == 0);
        CHECK_FALSE(std::filesystem::exists(pathFromUtf8(pathToUtf8(scene) + ".meta")));
        test::checkNoValidationIssues(game->engine().graphics());
    }

    TEST_CASE("what is no game, or cannot start, is refused") {
        const TempDirectory directory;
        const TempDirectory empty;
        CHECK(refusal(GameDesc{.directory = directory.path()}).code == ErrorCode::NotFound);

        const Project bare = unwrap(createProject(directory.path() / "bare", "Bare"));
        const Error noStart = refusal(GameDesc{.directory = bare.directory});
        CHECK(noStart.code == ErrorCode::NotFound);
        CHECK(noStart.message.find("names no start scene") != std::string::npos);

        Project lost = unwrap(createProject(directory.path() / "lost", "Lost"));
        lost.settings.startScene = "scenes/none.scene.json";
        REQUIRE(saveProject(lost).has_value());
        GameDesc lostGame = showcaseGame(empty.path());
        lostGame.directory = lost.directory;
        // Without the shaders the engine does not start; with them, the scene is missing.
        CHECK(refusal(lostGame).message.find(".slang") != std::string::npos);
        lostGame.engine.cacheDirectory = test::compiledShaderCache();
        const Error noScene = refusal(lostGame);
        CHECK(noScene.code == ErrorCode::NotFound);
        CHECK(noScene.message.find("scenes/none.scene.json") != std::string::npos);

        // Another kind of asset is no scene to start with.
        REQUIRE(writeTextFile(lost.assetDirectory() / "scripts" / "start.lua", "return {}\n").has_value());
        lost.settings.startScene = "scripts/start.lua";
        REQUIRE(saveProject(lost).has_value());
        const Error script = refusal(lostGame);
        CHECK(script.code == ErrorCode::InvalidArgument);
        CHECK(script.message.find("not a scene") != std::string::npos);
    }
}
