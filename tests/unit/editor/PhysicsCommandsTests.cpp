#include "cordite/editor/PhysicsCommands.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"

#include "support/MathTestSupport.h"
#include "unit/editor/EditorTestSupport.h"

#include <memory>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::EditorFixture;
using cordite::test::json;
using cordite::test::unwrap;

namespace {

/// The editor's small scene - Room / Table / Lamp, Chair, Eye - with a physics system
/// and the commands that ask it. The table is a static top of two metres by one, a
/// metre up; the chair a box that the simulation would move; the lamp a ball that is
/// part of the table's body.
struct PhysicsFixture : EditorFixture {
    std::unique_ptr<PhysicsSystem> physics = unwrap(PhysicsSystem::create());
    CommandRegistry commands;

    PhysicsFixture() {
        REQUIRE(registerPhysicsCommands(commands, context, *physics).has_value());
        world.transform(table).position = Vec3(0.0f, 1.0f, 0.0f);
        world.add<ColliderComponent>(table) = {.size = Vec3(2.0f, 0.1f, 1.0f)};
        world.add<RigidBodyComponent>(table).motion = BodyMotion::Static;
        world.transform(lamp).position = Vec3(0.5f, 0.3f, 0.0f);
        world.add<ColliderComponent>(lamp) = {.shape = ShapeType::Sphere, .radius = 0.25f};
        world.transform(chair).position = Vec3(5.0f, 0.5f, 0.0f);
        world.add<ColliderComponent>(chair);
        world.add<RigidBodyComponent>(chair);
    }

    Json run(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));
        return *result;
    }

    Error fail(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_FALSE(result.has_value());
        return result.error();
    }
};

} // namespace

TEST_SUITE("PhysicsCommands") {
    TEST_CASE("physics.raycast says what a ray hits, and where") {
        PhysicsFixture fixture;
        // Straight down on the chair, whose top is a metre up. No frame has gone by: the
        // command brings the bodies in line itself.
        Json hit = fixture.run("physics.raycast", R"({"origin": [5, 10, 0], "direction": [0, -1, 0]})");
        CHECK(hit["hit"] == true);
        CHECK(hit["entity"] == "Room/Chair");
        CHECK(hit["id"] == fixture.world.id(fixture.chair).toString());
        CHECK(hit["distance"].get<float>() == doctest::Approx(9.0f).epsilon(1.0e-4));
        CHECK(hit["position"][1].get<float>() == doctest::Approx(1.0f).epsilon(1.0e-4));
        CHECK(hit["normal"] == json("[0.0, 1.0, 0.0]"));

        // A collider that is part of the body of an entity above it is named as that
        // entity: the lamp, on the table.
        hit = fixture.run("physics.raycast", R"({"origin": [0.5, 10, 0], "direction": [0, -5, 0]})");
        CHECK(hit["entity"] == "Room/Table");
        CHECK(hit["distance"].get<float>() == doctest::Approx(10.0f - 1.55f).epsilon(1.0e-3));

        // Nothing in the way, and too short a ray.
        CHECK(fixture.run("physics.raycast", R"({"origin": [20, 10, 0], "direction": [0, -1, 0]})") ==
              json(R"({"hit": false})"));
        CHECK(
            fixture.run("physics.raycast", R"({"origin": [5, 10, 0], "direction": [0, -1, 0], "maxDistance": 8.9})") ==
            json(R"({"hit": false})"));

        // What was moved since is where it is now.
        fixture.world.transform(fixture.chair).position = Vec3(20.0f, 0.5f, 0.0f);
        hit = fixture.run("physics.raycast", R"({"origin": [20, 10, 0], "direction": [0, -1, 0]})");
        CHECK(hit["entity"] == "Room/Chair");
        // And nothing has changed the scene by being asked.
        CHECK(fixture.world.transform(fixture.chair).position == Vec3(20.0f, 0.5f, 0.0f));
    }

    TEST_CASE("physics.raycast with a radius moves a ball along the ray") {
        PhysicsFixture fixture;
        // Beside the chair, whose side is at 5.5: a ray goes past, a ball of half a metre
        // touches its edge.
        const std::string_view beside = R"("origin": [5.8, 10, 0], "direction": [0, -1, 0])";
        CHECK(fixture.run("physics.raycast", std::string("{") + std::string(beside) + "}") ==
              json(R"({"hit": false})"));
        const Json hit = fixture.run("physics.raycast", std::string("{") + std::string(beside) + R"(, "radius": 0.5})");
        CHECK(hit["hit"] == true);
        CHECK(hit["entity"] == "Room/Chair");
        CHECK(hit["distance"].get<float>() == doctest::Approx(8.6f).epsilon(5.0e-3));
        CHECK(hit["normal"][0].get<float>() > 0.5f);
    }

    TEST_CASE("physics.overlap lists the entities that reach into a ball or a box") {
        PhysicsFixture fixture;
        // Around the lamp: the table, whose body the lamp is part of - once.
        Json found = fixture.run("physics.overlap", R"({"center": [0.5, 1.3, 0], "radius": 0.5})");
        REQUIRE(found["entities"].size() == 1);
        CHECK(found["entities"][0]["entity"] == "Room/Table");
        CHECK(found["entities"][0]["id"] == fixture.world.id(fixture.table).toString());

        // A ball large enough for the chair as well; one that reaches nothing.
        found = fixture.run("physics.overlap", R"({"center": [2.5, 1, 0], "radius": 2.5})");
        REQUIRE(found["entities"].size() == 2);
        CHECK(found["entities"][0]["entity"] == "Room/Table");
        CHECK(found["entities"][1]["entity"] == "Room/Chair");
        CHECK(fixture.run("physics.overlap", R"({"center": [2.5, 5, 0], "radius": 1})") == json(R"({"entities": []})"));

        // A box beside the chair reaches it when it is turned an eighth about Y.
        CHECK(fixture.run("physics.overlap", R"({"center": [6.9, 0.5, 0], "size": [2, 0.5, 2]})") ==
              json(R"({"entities": []})"));
        found =
            fixture.run("physics.overlap",
                        R"({"center": [6.9, 0.5, 0], "size": [2, 0.5, 2], "rotation": [0, 0.38268343, 0, 0.9238795]})");
        REQUIRE(found["entities"].size() == 1);
        CHECK(found["entities"][0]["entity"] == "Room/Chair");
    }

    TEST_CASE("questions that make no sense are refused, and say why") {
        PhysicsFixture fixture;
        // A ray needs a start and a way to go.
        CHECK(fixture.fail("physics.raycast", R"({"direction": [0, -1, 0]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.raycast", R"({"origin": [0, 1, 0]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.raycast", R"({"origin": [0, 1], "direction": [0, -1, 0]})").message ==
              "physics.raycast: 'origin' must be an array of 3 numbers");
        CHECK(fixture.fail("physics.raycast", R"({"origin": [0, 1, 0], "direction": ["a", "b", "c"]})").message ==
              "physics.raycast: 'direction' must be an array of 3 numbers");
        CHECK(fixture.fail("physics.raycast", R"({"origin": [0, 1, 0], "direction": [0, 0, 0]})").message ==
              "physics.raycast: 'direction' has no length, and so points nowhere");
        const std::string ray = R"("origin": [0, 1, 0], "direction": [0, -1, 0])";
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "maxDistance": 0})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "maxDistance": -3})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "radius": -1})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "radius": "wide"})").code == ErrorCode::InvalidArgument);
        // Lengths beyond any scene are refused before they are numbers that are none.
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "maxDistance": 1e300})").message ==
              "physics.raycast: 'maxDistance' must be more than nothing and no more than 1000000, got 1e+300");
        CHECK(fixture.fail("physics.raycast", "{" + ray + R"(, "radius": 1e300})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.raycast", R"({"origin": [0, 1e300, 0], "direction": [0, -1, 0]})").code ==
              ErrorCode::InvalidArgument);
        // The longest ray there is finds what is in its way.
        CHECK(fixture.run("physics.raycast",
                          R"({"origin": [5, 10, 0], "direction": [0, -1, 0], "maxDistance": 1000000})")["hit"] == true);

        // A ball or a box, not neither and not both; a ball is not turned.
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0]})").message ==
              "physics.overlap: give either 'radius', for a ball, or 'size', for a box - one of them, and not both");
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "radius": 1, "size": [1, 1, 1]})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "radius": 1, "rotation": [0, 0, 0, 1]})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.overlap", R"({"radius": 1})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "radius": 0})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "radius": 1e300})").code ==
              ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "size": [1, 1e30, 1]})").code ==
              ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "size": [1, 0, 1]})").code ==
              ErrorCode::OutOfRange);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "size": [1, 1]})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("physics.overlap", R"({"center": [0, 1, 0], "size": [1, 1, 1], "rotation": [0, 0, 0, 0]})")
                  .code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("the physics commands describe themselves for the catalogue") {
        PhysicsFixture fixture;
        const Json catalogue = fixture.commands.describe();
        REQUIRE(catalogue.size() == 2);
        CHECK(catalogue[0]["name"] == "physics.overlap");
        CHECK(catalogue[1]["name"] == "physics.raycast");
        for (const Json& command : catalogue) {
            CHECK_FALSE(command["description"].get<std::string>().empty());
            for (const Json& parameter : command["parameters"]) {
                CHECK_FALSE(parameter["description"].get<std::string>().empty());
            }
        }
    }
}
