#include "cordite/engine/SceneChange.h"

#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/SceneSerializer.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <memory>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::unwrap;

namespace {

/// A script that says when it starts and when it ends.
constexpr const char* kWitness = R"(
    local Witness = {}
    function Witness:start() print("start " .. self.entity:name()) end
    function Witness:destroy() print("destroy " .. self.entity:name()) end
    return Witness
)";

/// A scene of one entity, three metres along X, that has a collider and the witness for
/// a script.
std::string sceneOf(std::string_view name, std::string_view entity, const Uuid& script) {
    return fmt::format(R"json({{
        "format": "cordite.scene",
        "version": 1,
        "name": "{}",
        "entities": [
            {{
                "id": "20000000-0000-4000-8000-000000000001",
                "name": "{}",
                "enabled": true,
                "parent": null,
                "components": {{
                    "Transform": {{ "position": [3.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0] }},
                    "Collider": {{}},
                    "Script": {{ "script": "{}", "properties": {{}} }}
                }}
            }}
        ]
    }})json",
                       name, entity, script.toString());
}

/// A directory of assets with two scenes in it, and a world that is being played in the
/// first of them: its script has started, and its body is there.
struct Game {
    test::TempDirectory directory;
    std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
    ComponentRegistry components;
    World world;
    std::unique_ptr<ScriptSystem> scripts = unwrap(ScriptSystem::create());
    std::unique_ptr<PhysicsSystem> physics = unwrap(PhysicsSystem::create());
    AnimationSystem animation;
    Uuid yard;
    Uuid cellar;
    Uuid witness;

    Game() {
        REQUIRE(registerEngineComponents(components).has_value());
        witness = write("scripts/witness.lua", kWitness);
        yard = write("scenes/yard.scene.json", sceneOf("Yard", "Gate", witness));
        cellar = write("scenes/cellar.scene.json", sceneOf("Cellar", "Stairs", witness));
        const Status loaded = loadScene(directory.path() / "scenes" / "yard.scene.json", world, components);
        REQUIRE_MESSAGE(loaded.has_value(), test::errorOf(loaded));
        play();
    }

    /// Writes a file of the project and has the assets look again. Returns its id.
    Uuid write(std::string_view path, std::string_view text) {
        const Status written = writeTextFile(directory.path() / pathFromUtf8(path), text);
        REQUIRE_MESSAGE(written.has_value(), test::errorOf(written));
        REQUIRE(assets->refresh().has_value());
        const AssetInfo* asset = assets->database().findByReference(path);
        REQUIRE(asset != nullptr);
        return asset->id;
    }

    /// A frame of the game: the scripts run, and the simulation moves on.
    ScriptStatistics play() {
        const ScriptStatistics statistics = scripts->update({.world = world,
                                                             .components = components,
                                                             .assets = *assets,
                                                             .deltaSeconds = 0.5,
                                                             .physics = physics.get()});
        physics->update(world, *assets, 1.0f / 60.0f);
        return statistics;
    }

    [[nodiscard]] SceneChange change() {
        return {.world = world,
                .components = components,
                .assets = *assets,
                .scripts = *scripts,
                .physics = physics.get(),
                .animation = &animation};
    }
};

} // namespace

TEST_SUITE("SceneChange") {
    TEST_CASE("a scene takes the place of another: its scripts end, its bodies go, and the new one is there") {
        Game game;
        const Entity gate = game.world.findByName("Gate");
        REQUIRE_FALSE(gate.isNull());
        REQUIRE_FALSE(game.physics->bodyOf(gate).isNull());
        const BodyId gateBody = game.physics->bodyOf(gate);
        CHECK(game.scripts->seconds() == 0.5);

        const test::LogCapture log(LogLevel::Info);
        const Status changed = changeScene(game.change(), game.cellar);
        REQUIRE_MESSAGE(changed.has_value(), test::errorOf(changed));

        // The script of the scene that went was told so, by its name: its entity was
        // still there to ask.
        CHECK(log.contains("destroy Gate"));
        CHECK_FALSE(log.contains("start Stairs"));
        // The world holds the new scene, and nothing of the old.
        CHECK(game.world.sceneName() == "Cellar");
        CHECK(game.world.findByName("Gate").isNull());
        const Entity stairs = game.world.findByName("Stairs");
        REQUIRE_FALSE(stairs.isNull());
        // Where its entities are in the world is worked out: a frame that draws the scene
        // before anything has updated it draws it where it is.
        CHECK(Vec3(game.world.worldMatrix(stairs)[3]) == Vec3(3.0f, 0.0f, 0.0f));
        // The body of the gate is gone, not handed on to whatever has its handle now; the
        // stairs get theirs with the next step.
        CHECK_FALSE(game.physics->simulation().isValid(gateBody));
        CHECK(game.physics->bodyOf(stairs).isNull());
        // The scripts begin anew: their clock, and what they run.
        CHECK(game.scripts->seconds() == 0.0);
        CHECK(game.scripts->problems().empty());

        const ScriptStatistics statistics = game.play();
        CHECK(log.contains("start Stairs"));
        CHECK(statistics.instances == 1);
        CHECK(game.scripts->seconds() == 0.5);
        CHECK_FALSE(game.physics->bodyOf(stairs).isNull());
        CHECK(log.count(LogLevel::Error) == 0);
    }

    TEST_CASE("a scene can be loaded again: the one that is there starts anew") {
        Game game;
        const test::LogCapture log(LogLevel::Info);
        REQUIRE(changeScene(game.change(), game.yard).has_value());
        CHECK(log.contains("destroy Gate"));
        game.play();
        CHECK(log.contains("start Gate"));
        CHECK(game.world.sceneName() == "Yard");
    }

    TEST_CASE("a scene that cannot be loaded leaves the one that is there as it was") {
        Game game;
        const Uuid broken = game.write("scenes/broken.scene.json", sceneOf("Broken", "Rubble", game.witness) + "}");
        const Uuid wrong = game.write(
            "scenes/wrong.scene.json",
            R"({"format": "cordite.scene", "version": 1, "name": "Wrong", "entities": [{"id": "20000000-0000-4000-8000-000000000001", "name": "Ghost", "enabled": true, "parent": null, "components": {"Transform": {}, "Haunting": {}}}]})");
        const Uuid missing = Uuid::parse("0123abcd-0123-4abc-8def-0123456789ab").value_or(Uuid());
        REQUIRE_FALSE(missing.isNil());
        const Entity gate = game.world.findByName("Gate");
        const BodyId gateBody = game.physics->bodyOf(gate);

        const test::LogCapture log(LogLevel::Info);
        const auto refused = [&](const Uuid& scene) {
            const Status changed = changeScene(game.change(), scene);
            REQUIRE_FALSE(changed.has_value());
            // Nothing has ended, and nothing is gone.
            CHECK_FALSE(log.contains("destroy Gate"));
            CHECK(game.world.sceneName() == "Yard");
            CHECK(game.world.isValid(gate));
            CHECK(game.physics->simulation().isValid(gateBody));
            CHECK(game.scripts->seconds() == 0.5);
            return changed.error();
        };

        Error error = refused(missing);
        CHECK(error.code == ErrorCode::NotFound);
        CHECK(error.message == "there is no asset 0123abcd-0123-4abc-8def-0123456789ab to load as a scene");
        error = refused(game.witness);
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "'scripts/witness.lua' is of the kind script, not a scene");
        // Text that is no JSON, and a scene that names a component there is none of: both
        // are said with the file they are in.
        error = refused(broken);
        CHECK(error.message.find("broken.scene.json") != std::string::npos);
        error = refused(wrong);
        CHECK(error.message.find("wrong.scene.json") != std::string::npos);
        CHECK(error.message.find("Haunting") != std::string::npos);

        // The game goes on where it was.
        const ScriptStatistics statistics = game.play();
        CHECK(statistics.instances == 1);
        CHECK(statistics.stopped == 0);
        CHECK(game.scripts->seconds() == 1.0);
        CHECK_FALSE(log.contains("start Gate"));
    }

    TEST_CASE("a world without a simulation or poses changes its scene all the same") {
        Game game;
        const test::LogCapture log(LogLevel::Info);
        const SceneChange bare = {
            .world = game.world, .components = game.components, .assets = *game.assets, .scripts = *game.scripts};
        REQUIRE(changeScene(bare, game.cellar).has_value());
        CHECK(log.contains("destroy Gate"));
        CHECK(game.world.sceneName() == "Cellar");
    }
}
