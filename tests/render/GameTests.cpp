#include "cordite/engine/Game.h"

#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/engine/GameSettings.h"
#include "cordite/engine/Project.h"
#include "cordite/renderer/SceneRenderer.h"

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

    TEST_CASE("a game plays with the settings of the person who plays, and writes the defaults when there are none") {
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / "player" / "settings.json";
        GameDesc desc = showcaseGame(test::compiledShaderCache());
        desc.settingsFile = file;
        {
            // None yet: the defaults - what a game without settings has - and a file with
            // every one of them in it, to change.
            const std::unique_ptr<Game> game = unwrap(Game::create(desc));
            REQUIRE(game->settings().has_value());
            CHECK(*game->settings() == GameSettings{});
            CHECK(unwrap(loadGameSettings(file)) == GameSettings{});
            CHECK(game->engine().renderSize() == UVec2(GameSettings{}.width, GameSettings{}.height));
            CHECK(renderQualityOf(game->engine().renderSettings()) == GameSettings{}.quality);
        }

        // Changed by the person, between two starts: the next start takes it.
        GameSettings chosen;
        chosen.width = 400;
        chosen.height = 240;
        chosen.quality = RenderQuality::Low;
        chosen.masterVolume = 0.5f;
        chosen.musicVolume = 0.25f;
        chosen.voiceVolume = 0.0f;
        REQUIRE(saveGameSettings(file, chosen).has_value());
        {
            const std::unique_ptr<Game> game = unwrap(Game::create(desc));
            Engine& engine = game->engine();
            CHECK(*game->settings() == chosen);
            CHECK(engine.renderSize() == UVec2(400, 240));
            CHECK(renderQualityOf(engine.renderSettings()) == RenderQuality::Low);
            CHECK(engine.audio().masterVolume() == 0.5f);
            CHECK(engine.audio().volume(AudioBus::Music) == 0.25f);
            CHECK(engine.audio().volume(AudioBus::Voice) == 0.0f);
            CHECK(engine.audio().volume(AudioBus::Effects) == 1.0f);
        }

        // What the command line says wins for one start, and is not written.
        desc.overrides = {.width = 320, .quality = RenderQuality::High};
        {
            const std::unique_ptr<Game> game = unwrap(Game::create(desc));
            CHECK(game->engine().renderSize() == UVec2(320, 240));
            CHECK(renderQualityOf(game->engine().renderSettings()) == RenderQuality::High);
            CHECK(game->engine().audio().volume(AudioBus::Music) == 0.25f);
        }
        CHECK(unwrap(loadGameSettings(file)) == chosen);
    }

    TEST_CASE("settings that cannot be read are said and left alone, and the game plays with the defaults") {
        const TempDirectory directory;
        GameDesc desc = showcaseGame(test::compiledShaderCache());
        desc.settingsFile = directory.path() / "settings.json";
        const std::string broken = R"({"format": "cordite.settings", "version": 1, "musicVolume": 2})";
        REQUIRE(writeTextFile(desc.settingsFile, broken).has_value());
        {
            const test::LogCapture log;
            const std::unique_ptr<Game> game = unwrap(Game::create(desc));
            CHECK(*game->settings() == GameSettings{});
            CHECK(log.contains("the settings of the game cannot be read, and the defaults are used"));
            CHECK(log.contains("musicVolume"));
        }
        CHECK(unwrap(readTextFile(desc.settingsFile)) == broken);

        // Nowhere to write them: the game plays all the same, and says so.
        REQUIRE(writeTextFile(directory.path() / "taken", "a file, where a directory would be").has_value());
        desc.settingsFile = directory.path() / "taken" / "settings.json";
        {
            const test::LogCapture log;
            const std::unique_ptr<Game> game = unwrap(Game::create(desc));
            CHECK(*game->settings() == GameSettings{});
            CHECK(log.contains("the settings of the game could not be written"));
        }
    }

    TEST_CASE("a game without settings is made as it is told, with what the command line says over it") {
        GameDesc desc = showcaseGame(test::compiledShaderCache());
        desc.overrides = {.height = 200, .quality = RenderQuality::Low};
        const std::unique_ptr<Game> game = unwrap(Game::create(desc));
        CHECK_FALSE(game->settings().has_value());
        CHECK(game->engine().renderSize() == UVec2(320, 200));
        CHECK(renderQualityOf(game->engine().renderSettings()) == RenderQuality::Low);
        CHECK(game->engine().audio().masterVolume() == 1.0f);
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
