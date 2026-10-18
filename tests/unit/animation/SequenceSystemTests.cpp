#include "cordite/animation/SequenceSystem.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/MathReflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A component of the tests: a property of each kind a track can write, that the
/// components of the world and of animation do not all have.
struct Gauge {
    int32_t count = 0;
    Vec2 size{0.0f};
    Vec4 tint{0.0f};
    float level = 0.0f;
    bool lit = false;
    float sealed = 1.0f;
};

TypeInfo describeGauge() {
    return TypeBuilder<Gauge>("Gauge")
        .property("count", &Gauge::count, {.minimum = 0.0, .maximum = 100.0})
        .property("size", &Gauge::size)
        .property("tint", &Gauge::tint)
        .property("level", &Gauge::level, {.minimum = 0.0, .maximum = 10.0})
        .property("lit", &Gauge::lit)
        .property("sealed", &Gauge::sealed, {.readOnly = true})
        .build();
}

/// A world that plays a sequence of the given tracks, four seconds long unless said:
///
///     Director   (SequencePlayer)
///       Rig
///         Camera (Camera)
///     Lamp       (Gauge)
///     Subject
struct Stage {
    explicit Stage(std::string_view tracks, float duration = 4.0f) {
        REQUIRE(writeTextFile(directory.path() / "scene.sequence.json",
                              std::string(R"({"format": "cordite.sequence", "version": 1, "duration": )") +
                                  std::to_string(duration) + R"(, "tracks": [)" + std::string(tracks) + "]}")
                    .has_value());
        assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        REQUIRE(registerWorldComponents(components).has_value());
        REQUIRE(registerAnimationComponents(components).has_value());
        REQUIRE(components.add<Gauge>(describeGauge()).has_value());

        director = world.createEntity("Director");
        rig = world.createEntity("Rig");
        camera = world.createEntity("Camera");
        lamp = world.createEntity("Lamp");
        subject = world.createEntity("Subject");
        REQUIRE(world.setParent(rig, director).has_value());
        REQUIRE(world.setParent(camera, rig).has_value());
        world.add<CameraComponent>(camera).primary = false;
        world.add<Gauge>(lamp);
        const AssetInfo* sequence = assets->database().findByReference("scene.sequence.json");
        REQUIRE(sequence != nullptr);
        world.add<SequencePlayerComponent>(director).sequence = sequence->id;
        world.updateWorldTransforms();
    }

    SequenceStatistics update(float seconds) { return system.update(world, components, *assets, seconds); }

    [[nodiscard]] SequencePlayerComponent& player() { return *world.tryGet<SequencePlayerComponent>(director); }

    [[nodiscard]] Gauge& gauge() { return *world.tryGet<Gauge>(lamp); }

    /// The names of the events of the last update.
    [[nodiscard]] std::vector<std::string> events() const {
        std::vector<std::string> names;
        for (const SequenceEvent& event : system.events()) {
            CHECK(event.player == director);
            names.push_back(event.name);
        }
        return names;
    }

    TempDirectory directory;
    std::unique_ptr<AssetManager> assets;
    ComponentRegistry components;
    World world;
    SequenceSystem system;
    Entity director;
    Entity rig;
    Entity camera;
    Entity lamp;
    Entity subject;
};

using Names = std::vector<std::string>;

constexpr std::string_view kEvents = R"({"kind": "event", "keys": [
    {"time": 0, "name": "start"}, {"time": 1, "name": "one"}, {"time": 2, "name": "two"},
    {"time": 4, "name": "end"}]})";

} // namespace

TEST_SUITE("SequenceSystem") {
    TEST_CASE("a player moves its time on at its speed, and stops at the end of a sequence that does not loop") {
        Stage stage(kEvents);
        stage.player().speed = 2.0f;
        const SequenceStatistics statistics = stage.update(0.5f);
        CHECK(statistics.players == 1);
        CHECK(stage.player().time == 1.0f);
        CHECK(stage.player().playing);
        stage.update(1.75f);
        CHECK(stage.player().time == 4.0f);
        CHECK_FALSE(stage.player().playing);
        // Stopped, it holds.
        CHECK(stage.update(1.0f).players == 0);
        CHECK(stage.player().time == 4.0f);
    }

    TEST_CASE("a player that loops goes round") {
        Stage stage(kEvents);
        stage.player().loop = true;
        stage.player().time = 3.5f;
        stage.update(1.0f);
        CHECK(stage.player().time == doctest::Approx(0.5f));
        CHECK(stage.player().playing);
    }

    TEST_CASE("the events a player's time passes are listed once each, in order, and at its end those at the end") {
        Stage stage(kEvents);
        stage.update(0.5f);
        CHECK(stage.events() == Names{"start"});
        // An event at the very time the step ends is the next step's.
        stage.update(0.5f);
        CHECK(stage.events().empty());
        stage.update(0.25f);
        CHECK(stage.events() == Names{"one"});
        stage.update(10.0f);
        CHECK(stage.events() == Names{"two", "end"});
        CHECK_FALSE(stage.player().playing);
        stage.update(1.0f);
        CHECK(stage.events().empty());
    }

    TEST_CASE("a player that loops passes what it goes round over, and in a long frame every event once at the most") {
        Stage stage(kEvents);
        stage.player().loop = true;
        stage.player().time = 1.5f;
        // Over the end and on from the beginning.
        stage.update(3.0f);
        CHECK(stage.events() == Names{"two", "end", "start"});
        CHECK(stage.player().time == doctest::Approx(0.5f));
        // Ten seconds of a sequence of four: each event once, from where it was.
        stage.update(10.0f);
        CHECK(stage.events() == Names{"one", "two", "end", "start"});
        CHECK(stage.player().time == doctest::Approx(0.5f));
    }

    TEST_CASE("a player set to a time goes on from there, and what it was set past is not told") {
        Stage stage(kEvents);
        stage.player().time = 1.5f;
        stage.update(0.25f);
        CHECK(stage.events().empty());
        stage.update(0.5f);
        CHECK(stage.events() == Names{"two"});
    }

    TEST_CASE("a transform track moves what it names, from the entity that plays it") {
        Stage stage(R"(
            {"kind": "transform", "target": "Rig/Camera", "keys": [
                {"time": 0, "position": [0, 0, 0], "rotation": [0, 0, 0, 1]},
                {"time": 2, "position": [4, 2, 0], "rotation": [0, 0.70710678, 0, 0.70710678]}]},
            {"kind": "transform", "target": "/Subject", "interpolation": "step", "keys": [
                {"time": 0, "scale": [1, 1, 1]}, {"time": 1, "scale": [2, 3, 4]}]})");
        const SequenceStatistics statistics = stage.update(1.0f);
        CHECK(statistics.tracks == 2);
        const Transform& camera = stage.world.transform(stage.camera);
        CHECK_NEAR(camera.position, Vec3(2.0f, 1.0f, 0.0f), 1e-5f);
        // Half way along the arc: 45 of the 90 degrees about Y.
        CHECK(eulerDegreesFromQuat(camera.rotation).y == doctest::Approx(45.0f).epsilon(1e-4));
        // At the time of a step, the step is taken.
        CHECK(stage.world.transform(stage.subject).scale == Vec3(2.0f, 3.0f, 4.0f));
        // What a track has no keys for is left as it is.
        CHECK(stage.world.transform(stage.subject).position == Vec3(0.0f));
        CHECK(camera.scale == Vec3(1.0f));
    }

    TEST_CASE("a transform track that looks at something turns its entity to face it, under what it hangs under") {
        Stage stage(R"({"kind": "transform", "target": "Rig/Camera", "lookAt": "/Subject", "keys": [
            {"time": 0, "position": [0, 0, 5]}, {"time": 4, "position": [5, 0, 5]}]})");
        // The rig is turned a quarter about Y and lifted; the subject stands to one side.
        stage.world.transform(stage.rig) =
            Transform{Vec3(0.0f, 2.0f, 0.0f), glm::angleAxis(toRadians(90.0f), kWorldUp), Vec3(1.0f)};
        stage.world.transform(stage.subject).position = Vec3(-3.0f, 0.0f, 4.0f);
        for (const float step : {0.0f, 1.0f, 2.0f}) {
            CAPTURE(step);
            stage.update(step);
            const Mat4 placement = stage.world.computeWorldMatrix(stage.camera);
            const Vec3 eye = Vec3(placement[3]);
            const Vec3 facing = -glm::normalize(Vec3(placement[2]));
            CHECK_NEAR(facing, glm::normalize(Vec3(-3.0f, 0.0f, 4.0f) - eye), 1e-5f);
            // Upright: its right is level.
            CHECK(std::abs(Vec3(placement[0]).y) < 1e-5f);
        }
    }

    TEST_CASE("a property track writes through the reflection, and whole numbers and yes or no step") {
        Stage stage(R"(
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
             "keys": [{"time": 0, "value": 0}, {"time": 2, "value": 10}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "count",
             "keys": [{"time": 0, "value": 1}, {"time": 2, "value": 7}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "lit",
             "keys": [{"time": 0, "value": false}, {"time": 2, "value": true}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "size",
             "keys": [{"time": 0, "value": [0, 0]}, {"time": 2, "value": [2, 4]}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "tint",
             "keys": [{"time": 0, "value": [1, 1, 1, 1]}, {"time": 2, "value": [0, 0, 0, 1]}]},
            {"kind": "property", "target": "Rig/Camera", "component": "Camera", "property": "verticalFovDegrees",
             "keys": [{"time": 0, "value": 60}, {"time": 2, "value": 30}]})");
        CHECK(stage.update(1.5f).tracks == 6);
        CHECK(stage.gauge().level == doctest::Approx(7.5f));
        CHECK(stage.gauge().count == 1);
        CHECK_FALSE(stage.gauge().lit);
        CHECK_NEAR(stage.gauge().size, Vec2(1.5f, 3.0f), 1e-6f);
        CHECK_NEAR(stage.gauge().tint, Vec4(0.25f, 0.25f, 0.25f, 1.0f), 1e-6f);
        CHECK(stage.world.tryGet<CameraComponent>(stage.camera)->verticalFovDegrees == doctest::Approx(37.5f));
        stage.update(0.5f);
        CHECK(stage.gauge().count == 7);
        CHECK(stage.gauge().lit);
    }

    TEST_CASE("what a smooth track gives a property is held to the property's range") {
        // Smooth through 0, 0, 10, 10: between the first two it swings below nothing.
        Stage stage(R"({"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
            "interpolation": "smooth", "keys": [{"time": 0, "value": 0}, {"time": 1, "value": 0},
                                                {"time": 2, "value": 10}, {"time": 3, "value": 10}]})");
        stage.gauge().level = 3.0f;
        const LogCapture log(LogLevel::Error);
        stage.update(0.5f);
        CHECK(stage.gauge().level == 0.0f);
        CHECK(log.count(LogLevel::Error) == 0);
    }

    TEST_CASE("a track that cannot be played is said once, plays nothing, and the others play") {
        Stage stage(R"(
            {"kind": "transform", "target": "Nobody", "keys": [{"time": 0, "position": [1, 1, 1]}]},
            {"kind": "transform", "target": "Rig", "lookAt": "/Nobody", "keys": [{"time": 0, "position": [1, 1, 1]}]},
            {"kind": "property", "target": "/Lamp", "component": "Lantern", "property": "level",
             "keys": [{"time": 0, "value": 1}]},
            {"kind": "property", "target": "/Subject", "component": "Gauge", "property": "level",
             "keys": [{"time": 0, "value": 1}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "depth",
             "keys": [{"time": 0, "value": 1}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
             "keys": [{"time": 0, "value": [1, 2, 3]}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "sealed",
             "keys": [{"time": 0, "value": 2}]},
            {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "count",
             "keys": [{"time": 0, "value": 9}]})");
        const LogCapture log(LogLevel::Error);
        CHECK(stage.update(0.5f).tracks == 1);
        CHECK(log.count(LogLevel::Error) == 7);
        for (const std::string_view fragment :
             {"transform track 0", "'Nobody' in the path 'Nobody' does not name a child of 'Director'",
              "transform track 1", "property track 0", "no component named 'Lantern'", "'Subject' has no Gauge",
              "Gauge has no property 'depth'", "Gauge.level holds a float", "property track 4"}) {
            CAPTURE(std::string(fragment));
            CHECK(log.contains(fragment));
        }
        CHECK(stage.gauge().count == 9);
        CHECK(stage.gauge().sealed == 1.0f);
        // The rig was moved before what it was to look at was looked for.
        CHECK(stage.world.transform(stage.rig).position == Vec3(1.0f));
        // Said once: the next frame says nothing new.
        stage.update(0.5f);
        CHECK(log.count(LogLevel::Error) == 7);
        // Until the system forgets what it said.
        stage.system.clear();
        stage.update(0.5f);
        CHECK(log.count(LogLevel::Error) == 14);
    }

    TEST_CASE("a camera track cuts to the camera it names while the sequence plays, and to none after") {
        Stage stage(
            R"({"kind": "camera", "keys": [{"time": 1, "camera": "Rig/Camera"}, {"time": 3, "camera": null}]})");
        const Uuid camera = stage.world.id(stage.camera);
        stage.update(0.5f);
        CHECK(stage.player().camera.isNil());
        stage.update(0.5f);
        CHECK(stage.player().camera == camera);
        stage.update(1.0f);
        CHECK(stage.player().camera == camera);
        stage.update(1.0f);
        CHECK(stage.player().camera.isNil());

        // Back to where the cut is, and then stopped: what does not play cuts to nothing.
        stage.player().time = 2.0f;
        stage.update(0.0f);
        CHECK(stage.player().camera == camera);
        stage.player().playing = false;
        stage.update(0.5f);
        CHECK(stage.player().camera.isNil());
        // At its end it stops, and the cut with it.
        stage.player().playing = true;
        stage.player().time = 2.0f;
        stage.update(5.0f);
        CHECK_FALSE(stage.player().playing);
        CHECK(stage.player().camera.isNil());
    }

    TEST_CASE("a cut ends with its sequence, also where its last key cuts to a camera") {
        Stage stage(R"({"kind": "camera", "keys": [{"time": 1, "camera": "Rig/Camera"}]})");
        stage.update(2.0f);
        CHECK(stage.player().camera == stage.world.id(stage.camera));
        stage.update(5.0f);
        CHECK_FALSE(stage.player().playing);
        CHECK(stage.player().camera.isNil());
        // One that loops goes on cutting.
        stage.player().loop = true;
        stage.player().playing = true;
        stage.player().time = 3.0f;
        stage.update(5.0f);
        CHECK(stage.player().playing);
        CHECK(stage.player().camera == stage.world.id(stage.camera));
    }

    TEST_CASE("a camera track that names what has no camera is said once, and cuts to none") {
        Stage stage(R"({"kind": "camera", "keys": [{"time": 0, "camera": "Rig"}]})");
        const LogCapture log(LogLevel::Error);
        stage.update(0.5f);
        stage.update(0.5f);
        CHECK(stage.player().camera.isNil());
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Rig' has no Camera"));
    }

    TEST_CASE("nothing is played that is not playing, switched off, or without a sequence") {
        Stage stage(R"({"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
            "keys": [{"time": 0, "value": 5}]})");
        stage.player().playing = false;
        CHECK(stage.update(0.5f) == SequenceStatistics{});
        CHECK(stage.player().time == 0.0f);
        CHECK(stage.gauge().level == 0.0f);

        stage.player().playing = true;
        stage.world.setEnabled(stage.director, false);
        CHECK(stage.update(0.5f) == SequenceStatistics{});
        CHECK(stage.gauge().level == 0.0f);

        stage.world.setEnabled(stage.director, true);
        stage.player().sequence = Uuid{};
        CHECK(stage.update(0.5f) == SequenceStatistics{});

        // A sequence that cannot be loaded is said once.
        const LogCapture log(LogLevel::Error);
        stage.player().sequence = builtinMeshId(BuiltinMesh::Cube);
        CHECK(stage.update(0.5f) == SequenceStatistics{});
        CHECK(stage.update(0.5f) == SequenceStatistics{});
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(stage.gauge().level == 0.0f);
    }

    TEST_CASE("no time, less than none and what is no time hold a player where it is, and what it says is played") {
        Stage stage(R"({"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
            "keys": [{"time": 0, "value": 0}, {"time": 4, "value": 8}]})");
        stage.player().time = 1.0f;
        for (const float step : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
            CAPTURE(step);
            stage.gauge().level = 0.0f;
            CHECK(stage.update(step).tracks == 1);
            CHECK(stage.player().time == 1.0f);
            CHECK(stage.gauge().level == doctest::Approx(2.0f));
        }
        // A speed of nothing, or of none, holds it too.
        stage.player().speed = 0.0f;
        stage.update(1.0f);
        CHECK(stage.player().time == 1.0f);
        stage.player().speed = std::numeric_limits<float>::quiet_NaN();
        stage.update(1.0f);
        CHECK(stage.player().time == 1.0f);
        // A time that is no number is the beginning.
        stage.player().speed = 1.0f;
        stage.player().time = std::numeric_limits<float>::quiet_NaN();
        stage.update(0.5f);
        CHECK(stage.player().time == 0.5f);
    }

    TEST_CASE("of two sequences that set the same thing, the later in the hierarchy has the last word") {
        Stage stage(R"({"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
            "keys": [{"time": 0, "value": 2}]})");
        REQUIRE(writeTextFile(stage.directory.path() / "other.sequence.json",
                              R"({"format": "cordite.sequence", "version": 1, "duration": 1, "tracks": [
                                  {"kind": "property", "target": "/Lamp", "component": "Gauge", "property": "level",
                                   "keys": [{"time": 0, "value": 6}]},
                                  {"kind": "event", "keys": [{"time": 0, "name": "other"}]}]})")
                    .has_value());
        REQUIRE(stage.assets->refresh().has_value());
        const Entity second = stage.world.createEntity("Second");
        stage.world.add<SequencePlayerComponent>(second).sequence =
            stage.assets->database().findByReference("other.sequence.json")->id;
        stage.update(0.1f);
        CHECK(stage.gauge().level == 6.0f);
        REQUIRE(stage.system.events().size() == 1);
        CHECK(stage.system.events()[0].player == second);
        // Turned round in the hierarchy, the other one has it.
        stage.world.setSiblingIndex(second, 0);
        stage.update(0.1f);
        CHECK(stage.gauge().level == 2.0f);
    }
}

TEST_SUITE("AnimationComponents") {
    TEST_CASE("a sequence player is registered after the camera shake, and saved without its cut") {
        ComponentRegistry registry;
        REQUIRE(registerAnimationComponents(registry).has_value());
        REQUIRE(registry.components().size() == 3);
        CHECK(registry.components()[2]->type.name() == "SequencePlayer");
        const TypeInfo& type = registry.find("SequencePlayer")->type;
        REQUIRE(type.properties().size() == 6);
        for (const PropertyInfo& property : type.properties()) {
            CAPTURE(property.name());
            CHECK_FALSE(property.attributes().description.empty());
        }
        CHECK(type.findProperty("sequence")->attributes().semantic == PropertySemantic::AssetReference);
        CHECK(type.findProperty("sequence")->attributes().referenceType == "Sequence");
        const PropertyAttributes& camera = type.findProperty("camera")->attributes();
        CHECK(camera.semantic == PropertySemantic::EntityReference);
        CHECK(camera.transient);
        CHECK(camera.readOnly);
        CHECK(type.findProperty("time")->attributes().unit == "s");

        SequencePlayerComponent playing;
        playing.camera = *Uuid::parse("0a56c2f1-21e0-819d-8bac-567d1e1c4345");
        CHECK(type.toJson(&playing) == unwrap(parseJson(R"({"sequence": null, "time": 0.0, "speed": 1.0,
                                                           "loop": false, "playing": true})")));
        // The cut is the system's to say, and nobody else's.
        CHECK_FALSE(
            type.parseEdit(unwrap(parseJson(R"({"camera": "0a56c2f1-21e0-819d-8bac-567d1e1c4345"})"))).has_value());
        CHECK_FALSE(type.parseEdit(unwrap(parseJson(R"({"speed": -1})"))).has_value());
    }
}
