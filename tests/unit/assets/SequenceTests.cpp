#include "cordite/assets/Sequence.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// A sequence file of four seconds with the given tracks.
Json sequenceFile(std::string_view tracks) {
    return json(std::string(R"({"format": "cordite.sequence", "version": 1, "duration": 4.0, "tracks": [)") +
                std::string(tracks) + "]}");
}

/// What reading a sequence of the given tracks says is wrong with it.
std::string mistakeIn(std::string_view tracks) {
    const Result<SequenceData> sequence = parseSequenceFile(sequenceFile(tracks));
    REQUIRE_FALSE(sequence.has_value());
    CHECK(sequence.error().code == ErrorCode::ParseError);
    return sequence.error().message;
}

} // namespace

TEST_SUITE("Sequence") {
    TEST_CASE("a sequence file says how long it is and what happens on its tracks") {
        const SequenceData sequence = unwrap(parseSequenceFile(sequenceFile(R"(
            {"kind": "camera", "keys": [{"time": 0.0, "camera": "Cinema camera"}, {"time": 3.5, "camera": null}]},
            {"kind": "transform", "target": "Cinema camera", "interpolation": "smooth", "lookAt": "/Stage/Sentry",
             "keys": [{"time": 0.0, "position": [6, 2, 6]}, {"time": 2.0, "position": [0, 3, 8]},
                      {"time": 4.0, "position": [-4, 3, 5]}]},
            {"kind": "transform", "target": "", "keys": [{"time": 1.0, "rotation": [0, 0, 0, 1], "scale": [2, 2, 2]}]},
            {"kind": "property", "target": "/Lamp", "component": "Light", "property": "intensity",
             "keys": [{"time": 1.0, "value": 0.0}, {"time": 2.0, "value": 6.0}]},
            {"kind": "property", "target": "/Lamp", "component": "Light", "property": "color", "interpolation": "step",
             "keys": [{"time": 0.0, "value": [1, 0.5, 0.25]}]},
            {"kind": "property", "target": "", "component": "Camera", "property": "primary",
             "keys": [{"time": 0.0, "value": false}, {"time": 3.0, "value": true}]},
            {"kind": "event", "keys": [{"time": 3.0, "name": "late"}]},
            {"kind": "event", "keys": [{"time": 1.0, "name": "early"}, {"time": 3.0, "name": "late too"}]})")));
        CHECK(sequence.duration == 4.0f);

        REQUIRE(sequence.cuts.size() == 2);
        CHECK(sequence.cuts[0] == SequenceCut{.time = 0.0f, .camera = "Cinema camera"});
        CHECK(sequence.cuts[1] == SequenceCut{.time = 3.5f, .camera = std::nullopt});

        REQUIRE(sequence.transforms.size() == 2);
        const SequenceTransformTrack& flight = sequence.transforms[0];
        CHECK(flight.target == "Cinema camera");
        CHECK(flight.interpolation == SequenceInterpolation::Smooth);
        CHECK(flight.lookAt == "/Stage/Sentry");
        CHECK(flight.positions.keys().size() == 3);
        CHECK(flight.rotations.empty());
        CHECK(flight.scales.empty());
        // Smooth: through every key, without corners - not straight from one to the next.
        CHECK_NEAR(flight.positions.evaluate(2.0f), Vec3(0.0f, 3.0f, 8.0f), 1e-6f);
        const Vec3 between = flight.positions.evaluate(1.0f);
        CHECK(glm::distance(between, Vec3(3.0f, 2.5f, 7.0f)) > 0.1f);
        // A track without an interpolation goes straight from key to key; one that
        // targets nothing is the entity that plays the sequence.
        const SequenceTransformTrack& turn = sequence.transforms[1];
        CHECK(turn.target.empty());
        CHECK(turn.interpolation == SequenceInterpolation::Linear);
        CHECK(turn.rotations.keys().size() == 1);
        CHECK(turn.scales.keys().size() == 1);
        CHECK(turn.positions.empty());

        REQUIRE(sequence.properties.size() == 3);
        const SequencePropertyTrack& intensity = sequence.properties[0];
        CHECK(intensity.target == "/Lamp");
        CHECK(intensity.component == "Light");
        CHECK(intensity.property == "intensity");
        CHECK(intensity.kind == SequenceValueKind::Number);
        CHECK(intensity.values.evaluate(1.5f).x == doctest::Approx(3.0f));
        CHECK(sequence.properties[1].kind == SequenceValueKind::Vector3);
        CHECK(sequence.properties[1].values.evaluate(2.0f) == Vec4(1.0f, 0.5f, 0.25f, 0.0f));
        // Yes or no has nothing between: whatever the track says, it steps.
        const SequencePropertyTrack& primary = sequence.properties[2];
        CHECK(primary.kind == SequenceValueKind::Boolean);
        CHECK(primary.values.evaluate(2.9f).x == 0.0f);
        CHECK(primary.values.evaluate(3.0f).x == 1.0f);

        // The events of every track, in the order of their times - and of the file.
        REQUIRE(sequence.events.size() == 3);
        CHECK(sequence.events[0] == SequenceEventKey{.time = 1.0f, .name = "early"});
        CHECK(sequence.events[1] == SequenceEventKey{.time = 3.0f, .name = "late"});
        CHECK(sequence.events[2] == SequenceEventKey{.time = 3.0f, .name = "late too"});
    }

    TEST_CASE("rotations are read as [x, y, z, w], brought to length one, each the same way round as the one before") {
        const SequenceData sequence = unwrap(parseSequenceFile(sequenceFile(R"(
            {"kind": "transform", "target": "", "keys": [
                {"time": 0.0, "rotation": [0, 0, 0, 2]},
                {"time": 1.0, "rotation": [0, -0.38268343, 0, -0.9238795]},
                {"time": 2.0, "rotation": [0, 0.70710678, 0, 0.70710678]}]})")));
        const std::span<const Keyframe<Vec4>> keys = sequence.transforms[0].rotations.keys();
        REQUIRE(keys.size() == 3);
        CHECK(keys[0].value == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        // Turned round to lie on the same side as the first; the same rotation.
        CHECK_NEAR(keys[1].value, Vec4(0.0f, 0.38268343f, 0.0f, 0.9238795f), 1e-7f);
        CHECK_NEAR(keys[2].value, Vec4(0.0f, 0.70710678f, 0.0f, 0.70710678f), 1e-7f);
    }

    TEST_CASE("the interpolations of a track are step, linear and smooth") {
        for (const auto& [name, interpolation] :
             std::initializer_list<std::pair<std::string_view, SequenceInterpolation>>{
                 {"step", SequenceInterpolation::Step},
                 {"linear", SequenceInterpolation::Linear},
                 {"smooth", SequenceInterpolation::Smooth}}) {
            CAPTURE(std::string(name));
            CHECK(nameOf(interpolation) == name);
            const SequenceData sequence = unwrap(parseSequenceFile(sequenceFile(
                std::string(R"({"kind": "transform", "target": "", "interpolation": ")") + std::string(name) +
                R"(", "keys": [{"time": 0.0, "position": [0, 0, 0]}, {"time": 2.0, "position": [2, 0, 0]}]})")));
            const float halfWay = sequence.transforms[0].positions.evaluate(1.0f).x;
            CHECK(sequence.transforms[0].interpolation == interpolation);
            CHECK(halfWay == doctest::Approx(interpolation == SequenceInterpolation::Step ? 0.0f : 1.0f));
        }
    }

    TEST_CASE("a sequence file that is wrong is refused, saying which track and which key") {
        for (const auto& [tracks, fragment] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {R"({"kind": "dance", "keys": []})", "track 0: 'kind' is 'dance'"},
                 {R"({"target": "", "keys": []})", "track 0: it has no 'kind'"},
                 {R"(3)", "track 0: it is not an object"},
                 {R"({"kind": "transform", "keys": [{"time": 0, "position": [0, 0, 0]}]})", "no 'target'"},
                 {R"({"kind": "transform", "target": "", "keys": []})", "'keys' is not a list of at least one key"},
                 {R"({"kind": "transform", "target": "", "keys": [{"position": [0, 0, 0]}]})",
                  "key 0: it has no 'time'"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 5, "position": [0, 0, 0]}]})",
                  "key 0: 'time' 5 is outside the sequence"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": -1, "position": [0, 0, 0]}]})",
                  "outside the sequence"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 1, "scale": [1, 1, 1]},
                                                                  {"time": 1, "scale": [2, 2, 2]}]})",
                  "key 1: 'time' 1 does not come after 1"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 0}]})",
                  "key 0: it has none of 'position', 'rotation' and 'scale'"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 0, "position": [0, 0]}]})", "'position'"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 0, "rotation": [0, 0, 0, 0]}]})",
                  "'rotation' is not a rotation"},
                 {R"({"kind": "transform", "target": "", "keys": [{"time": 0, "speed": 3}]})", "speed"},
                 {R"({"kind": "transform", "target": "", "lookAt": "Sentry",
                      "keys": [{"time": 0, "rotation": [0, 0, 0, 1]}]})",
                  "'rotation' keys and to face 'lookAt' both"},
                 {R"({"kind": "transform", "target": "", "lookAt": "", "keys": [{"time": 0, "position": [0, 0, 0]}]})",
                  "'lookAt' names nothing"},
                 {R"({"kind": "transform", "target": "", "interpolation": "bouncy",
                      "keys": [{"time": 0, "position": [0, 0, 0]}]})",
                  "'interpolation' is 'bouncy'"},
                 {R"({"kind": "property", "target": "", "property": "intensity", "keys": [{"time": 0, "value": 1}]})",
                  "no 'component'"},
                 {R"({"kind": "property", "target": "", "component": "Light", "property": "",
                      "keys": [{"time": 0, "value": 1}]})",
                  "must name something"},
                 {R"({"kind": "property", "target": "", "component": "Light", "property": "intensity",
                      "keys": [{"time": 0}]})",
                  "key 0: it has no 'value'"},
                 {R"({"kind": "property", "target": "", "component": "Light", "property": "intensity",
                      "keys": [{"time": 0, "value": 1}, {"time": 1, "value": [1, 2, 3]}]})",
                  "key 1: 'value' is three numbers where the first key of the track has a number"},
                 {R"({"kind": "property", "target": "", "component": "Light", "property": "intensity",
                      "keys": [{"time": 0, "value": "bright"}]})",
                  "'value' is none of a number"},
                 {R"({"kind": "property", "target": "", "component": "Light", "property": "intensity",
                      "keys": [{"time": 0, "value": [1, 2, 3, 4, 5]}]})",
                  "'value' is none of a number"},
                 {R"({"kind": "camera", "keys": [{"time": 0}]})", "key 0: it has no 'camera'"},
                 {R"({"kind": "camera", "keys": [{"time": 0, "camera": 7}]})", "'camera' is not text"},
                 {R"({"kind": "camera", "keys": [{"time": 0, "camera": "A"}]},
                     {"kind": "camera", "keys": [{"time": 1, "camera": "B"}]})",
                  "track 1: it is the second camera track"},
                 {R"({"kind": "camera", "target": "A", "keys": [{"time": 0, "camera": "A"}]})", "target"},
                 {R"({"kind": "event", "keys": [{"time": 0, "name": ""}]})", "'name' is empty"},
                 {R"({"kind": "event", "keys": [{"time": 0}]})", "it has no 'name'"},
             }) {
            CAPTURE(std::string(tracks));
            const std::string message = mistakeIn(tracks);
            CAPTURE(message);
            CHECK(message.find(fragment) != std::string::npos);
        }
    }

    TEST_CASE("what a sequence file is as a whole is checked too") {
        const auto refused = [](std::string_view text) {
            const Result<SequenceData> sequence = parseSequenceFile(json(text));
            return sequence.has_value() ? std::string() : sequence.error().message;
        };
        CHECK_FALSE(refused(R"({"format": "cordite.material", "version": 1})").empty());
        CHECK_FALSE(refused(R"({"format": "cordite.sequence", "version": 2, "duration": 1, "tracks": []})").empty());
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "tracks": []})").find("no 'duration'") !=
              std::string::npos);
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 0, "tracks": []})")
                  .find("not more than 0") != std::string::npos);
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 3601, "tracks": []})")
                  .find("at most 3600") != std::string::npos);
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 1})").find("'tracks'") !=
              std::string::npos);
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 1, "tracks": [], "loop": true})")
                  .find("loop") != std::string::npos);
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 1e39, "tracks": []})")
                  .find("'duration'") != std::string::npos);
        // A sequence of no tracks is a pause.
        CHECK(refused(R"({"format": "cordite.sequence", "version": 1, "duration": 1, "tracks": []})").empty());
        // Text is bounded.
        const std::string longName(kMaxSequenceText + 1, 'n');
        CHECK(mistakeIn(R"({"kind": "event", "keys": [{"time": 0, "name": ")" + longName + R"("}]})")
                  .find("longer than") != std::string::npos);
    }

    TEST_CASE("the assets of a project know sequences by their files, and load them by id") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "flyby.sequence.json",
                              R"({"format": "cordite.sequence", "version": 1, "duration": 2.5, "tracks": [
                                     {"kind": "event", "keys": [{"time": 1.0, "name": "boom"}]}]})")
                    .has_value());
        REQUIRE(writeTextFile(directory.path() / "broken.sequence.json",
                              R"({"format": "cordite.sequence", "version": 1, "duration": -1, "tracks": []})")
                    .has_value());
        const auto assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        const AssetInfo* flyby = assets->database().findByReference("flyby.sequence.json");
        REQUIRE(flyby != nullptr);
        CHECK(flyby->type == AssetType::Sequence);
        CHECK(nameOf(AssetType::Sequence) == "sequence");
        CHECK(displayNameOf(AssetType::Sequence) == "Sequence");
        CHECK(assetTypeFromName("Sequence") == AssetType::Sequence);
        const std::shared_ptr<const SequenceData> loaded = unwrap(assets->loadSequence(flyby->id));
        CHECK(loaded->duration == 2.5f);
        REQUIRE(loaded->events.size() == 1);
        CHECK(loaded->events[0].name == "boom");
        // Loaded once, and kept.
        CHECK(unwrap(assets->loadSequence(flyby->id)) == loaded);

        // A file that is wrong is listed, and is a problem of the project.
        const AssetInfo* broken = assets->database().findByReference("broken.sequence.json");
        REQUIRE(broken != nullptr);
        bool reported = false;
        for (const std::string& problem : assets->database().problems()) {
            reported = reported || problem.find("broken.sequence.json") != std::string::npos;
        }
        CHECK(reported);
        const Result<std::shared_ptr<const SequenceData>> failed = assets->loadSequence(broken->id);
        REQUIRE_FALSE(failed.has_value());
        CHECK(test::errorOf(failed).find("broken.sequence.json") != std::string::npos);
        // Another kind of asset is not a sequence.
        CHECK_FALSE(assets->loadSequence(builtinMeshId(BuiltinMesh::Cube)).has_value());
    }

    TEST_CASE("a sequence file larger than a sequence may be is refused before it is read") {
        const TempDirectory directory;
        std::string padded = R"({"format": "cordite.sequence", "version": 1, "duration": 1, "tracks": []})";
        padded.append(kMaxSequenceFileBytes, ' ');
        REQUIRE(writeTextFile(directory.path() / "huge.sequence.json", padded).has_value());
        const auto assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        const AssetInfo* huge = assets->database().findByReference("huge.sequence.json");
        REQUIRE(huge != nullptr);
        bool reported = false;
        for (const std::string& problem : assets->database().problems()) {
            reported = reported || (problem.find("huge.sequence.json") != std::string::npos &&
                                    problem.find("at most") != std::string::npos);
        }
        CHECK(reported);
        const Result<std::shared_ptr<const SequenceData>> loaded = assets->loadSequence(huge->id);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == ErrorCode::InvalidArgument);
    }
}
