#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"
#include "unit/physics/PhysicsTestSupport.h"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::kPhysicsStep;
using cordite::test::LogCapture;
using cordite::test::unwrap;
using Stage = cordite::test::PhysicsStage;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// An entity that is a character, with its soles at `position`.
Entity addCharacter(Stage& stage, const Vec3& position, const char* name = "Guard") {
    const Entity entity = stage.world.createEntity(name);
    stage.world.transform(entity).position = position;
    stage.world.add<CharacterControllerComponent>(entity);
    return entity;
}

CharacterControllerComponent& controllerOf(Stage& stage, Entity entity) {
    return *stage.world.tryGet<CharacterControllerComponent>(entity);
}

/// Everything that was said in the frames that are let pass.
std::vector<CollisionEvent> runAndListen(Stage& stage, int frames) {
    std::vector<CollisionEvent> heard;
    for (int frame = 0; frame < frames; ++frame) {
        stage.run(1);
        const std::span<const CollisionEvent> events = stage.physics->events();
        heard.insert(heard.end(), events.begin(), events.end());
    }
    return heard;
}

} // namespace

TEST_SUITE("CharacterController") {
    TEST_CASE("an entity with a character controller stands on the ground, and says what it stands on") {
        Stage stage;
        const Entity floor = stage.addFloor();
        const Entity guard = addCharacter(stage, Vec3(1.0f, 0.0f, 2.0f));
        const Quat turned = glm::angleAxis(1.0f, Vec3(0.0f, 1.0f, 0.0f));
        stage.world.transform(guard).rotation = turned;
        CHECK(stage.physics->characterOf(guard).isNull());

        // While no time passes there is a character, where the entity is; nothing is
        // said of it yet.
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).characters == 1);
        const CharacterId character = stage.physics->characterOf(guard);
        REQUIRE(stage.physics->simulation().isValid(character));
        CHECK(stage.physics->entityOf(stage.physics->simulation().characterBody(character)) == guard);
        CHECK(stage.physics->characterOf(floor).isNull());
        CHECK(stage.physics->bodyOf(guard).isNull());
        CHECK(controllerOf(stage, guard).ground == GroundState::InAir);
        CHECK(controllerOf(stage, guard).groundEntity.isNil());

        // The floor, and the body that stands for the character.
        const PhysicsFrameStatistics statistics = stage.run(10);
        CHECK(statistics.characters == 1);
        CHECK(statistics.bodies == 2);
        CHECK(stage.physics->characterOf(guard) == character);
        const CharacterControllerComponent& controller = controllerOf(stage, guard);
        CHECK(controller.ground == GroundState::OnGround);
        CHECK(controller.groundEntity == stage.world.id(floor));
        CHECK_NEAR(controller.groundNormal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-3f);
        CHECK_NEAR(controller.velocity, Vec3(0.0f), 1.0e-3f);
        // The entity is where the soles of the character are, and turned as it was.
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(1.0f, 0.0f, 2.0f), 2.0e-3f);
        CHECK(stage.world.transform(guard).rotation == turned);
        CHECK(stage.world.transform(guard).scale == Vec3(1.0f));
        // A ray finds the entity.
        CHECK(stage.under(1.0f, 2.0f) == "Guard");
    }

    TEST_CASE("a character walks as its component says, and its entity follows without being turned") {
        Stage stage;
        stage.addFloor();
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        stage.run(2);
        const CharacterId character = stage.physics->characterOf(guard);

        // What of the speed points up is left out.
        controllerOf(stage, guard).movement = Vec3(2.0f, 9.0f, -1.0f);
        stage.run(60);
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(2.0f, 0.0f, -1.0f), 5.0e-3f);
        CHECK_NEAR(controllerOf(stage, guard).velocity, Vec3(2.0f, 0.0f, -1.0f), 1.0e-3f);
        CHECK(controllerOf(stage, guard).movement == Vec3(2.0f, 9.0f, -1.0f));

        // Whoever walks it turns it - every frame, here - and that moves nothing: the
        // character goes on as it went, and is the same character.
        for (int frame = 1; frame <= 60; ++frame) {
            stage.world.transform(guard).rotation =
                glm::angleAxis(0.05f * static_cast<float>(frame), Vec3(0.0f, 1.0f, 0.0f));
            stage.run(1);
        }
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(4.0f, 0.0f, -2.0f), 1.0e-2f);
        CHECK(stage.world.transform(guard).rotation == glm::angleAxis(3.0f, Vec3(0.0f, 1.0f, 0.0f)));
        CHECK(stage.physics->characterOf(guard) == character);

        // A speed that is no number is not passed on: it goes on as it was last told.
        controllerOf(stage, guard).movement = Vec3(kNotANumber, 0.0f, 0.0f);
        stage.run(60);
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(6.0f, 0.0f, -3.0f), 2.0e-2f);
        // Told to stand, it stands.
        controllerOf(stage, guard).movement = Vec3(0.0f);
        stage.run(30);
        CHECK_NEAR(controllerOf(stage, guard).velocity, Vec3(0.0f), 1.0e-3f);
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(6.0f, 0.0f, -3.0f), 2.0e-2f);
    }

    TEST_CASE("a jump is taken from the component, and made in the step that follows") {
        Stage stage;
        stage.addFloor();
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        stage.run(5);

        controllerOf(stage, guard).jump = 4.0f;
        stage.run(1);
        CHECK(controllerOf(stage, guard).jump == 0.0f);
        CHECK(controllerOf(stage, guard).ground == GroundState::InAir);
        CHECK(controllerOf(stage, guard).groundEntity.isNil());
        CHECK(controllerOf(stage, guard).velocity.y == doctest::Approx(4.0f - 9.81f * kPhysicsStep).epsilon(0.001));
        float highest = 0.0f;
        for (int frame = 0; frame < 120; ++frame) {
            stage.run(1);
            highest = std::max(highest, stage.world.transform(guard).position.y);
        }
        CHECK(highest == doctest::Approx(4.0f * 4.0f / (2.0f * 9.81f)).epsilon(0.05));
        CHECK(controllerOf(stage, guard).ground == GroundState::OnGround);
        CHECK(stage.world.transform(guard).position.y == doctest::Approx(0.0f).epsilon(0.01));

        // In a frame that is too short to be a step the jump is taken all the same, and
        // not lost: it is made in the step that comes.
        controllerOf(stage, guard).jump = 4.0f;
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.25f * kPhysicsStep).steps == 0);
        CHECK(controllerOf(stage, guard).jump == 0.0f);
        CHECK(controllerOf(stage, guard).ground == GroundState::OnGround);
        CHECK(stage.run(1).steps == 1);
        CHECK(controllerOf(stage, guard).ground == GroundState::InAir);
        CHECK(controllerOf(stage, guard).velocity.y > 3.0f);

        // In the air it does not jump, and does not make up for it when it lands. A
        // speed that is no number is no jump.
        controllerOf(stage, guard).jump = 4.0f;
        stage.run(1);
        CHECK(controllerOf(stage, guard).jump == 0.0f);
        stage.run(120);
        controllerOf(stage, guard).jump = kNotANumber;
        stage.run(1);
        CHECK(controllerOf(stage, guard).jump == 0.0f);
        stage.run(30);
        CHECK(controllerOf(stage, guard).ground == GroundState::OnGround);
        CHECK(stage.world.transform(guard).position.y == doctest::Approx(0.0f).epsilon(0.01));
    }

    TEST_CASE("a character is as large as its entity is scaled") {
        Stage stage;
        stage.addFloor();
        // Three times as tall, and twice as far around: by Y in height, and by the larger
        // of X and Z around.
        const Entity giant = addCharacter(stage, Vec3(0.0f), "Giant");
        stage.world.transform(giant).scale = Vec3(2.0f, 3.0f, 1.5f);
        stage.run(5);
        // Its body is nine tenths of it, about its middle: of 5.4 metres, and 0.6 around.
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(2.7f + 0.9f * 2.7f).epsilon(0.005));
        CHECK(stage.under(0.5f, 0.0f) == "Giant");
        CHECK(stage.under(0.6f, 0.0f) == "Floor");
        CHECK(stage.world.transform(giant).scale == Vec3(2.0f, 3.0f, 1.5f));
        CHECK(stage.world.transform(giant).position.y == doctest::Approx(0.0f).epsilon(0.01));

        // Scaled otherwise it is another character, where the one before it was.
        const CharacterId before = stage.physics->characterOf(giant);
        stage.world.transform(giant).scale = Vec3(1.0f);
        stage.run(2);
        CHECK(stage.physics->characterOf(giant) != before);
        CHECK_FALSE(stage.physics->simulation().isValid(before));
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(0.9f + 0.9f * 0.9f).epsilon(0.005));

        // What it hangs under scales it as well - and the step it climbs with it.
        const Entity group = stage.world.createEntity("Group");
        stage.world.transform(group).scale = Vec3(2.0f);
        REQUIRE(stage.world.setParent(giant, group, false).has_value());
        stage.run(2);
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(1.8f + 0.9f * 1.8f).epsilon(0.005));
        const CharacterState state = stage.physics->simulation().characterState(stage.physics->characterOf(giant));
        CHECK(state.ground == GroundState::OnGround);
        const Entity kerb = stage.world.createEntity("Kerb");
        stage.world.transform(kerb).position = Vec3(3.0f, 0.3f, 0.0f);
        stage.world.add<ColliderComponent>(kerb) = {.size = Vec3(2.0f, 0.6f, 4.0f)};
        controllerOf(stage, giant).movement = Vec3(2.0f, 0.0f, 0.0f);
        stage.run(90);
        // Sixty centimetres: more than the 35 it climbs at its own size, less than the 70
        // it climbs at twice that.
        CHECK(stage.positionOf(giant).y == doctest::Approx(0.6f).epsilon(0.02));
    }

    TEST_CASE("under something that is moved, turned and scaled the entity is where the character is") {
        Stage stage;
        stage.addFloor();
        const Entity group = stage.world.createEntity("Group");
        const Quat quarterTurn = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f));
        stage.world.transform(group) = {
            .position = Vec3(10.0f, 0.0f, 5.0f), .rotation = quarterTurn, .scale = Vec3(2.0f)};
        const Entity guard = stage.world.createEntity("Guard");
        REQUIRE(stage.world.setParent(guard, group, false).has_value());
        // A metre along the group's X, which is two metres along -Z in the world.
        stage.world.transform(guard).position = Vec3(1.0f, 0.0f, 0.0f);
        const Quat own = glm::angleAxis(0.3f, Vec3(0.0f, 1.0f, 0.0f));
        stage.world.transform(guard).rotation = own;
        stage.world.add<CharacterControllerComponent>(guard) = {.radius = 0.15f, .height = 0.9f};
        stage.run(5);
        CHECK_NEAR(stage.positionOf(guard), Vec3(10.0f, 0.0f, 3.0f), 5.0e-3f);
        CHECK(controllerOf(stage, guard).ground == GroundState::OnGround);

        // It walks in the world's directions, whatever it hangs under: a metre along X.
        controllerOf(stage, guard).movement = Vec3(1.0f, 0.0f, 0.0f);
        stage.run(60);
        CHECK_NEAR(stage.positionOf(guard), Vec3(11.0f, 0.0f, 3.0f), 1.0e-2f);
        // Which is half a metre along the group's Z, seen from the group.
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(1.0f, 0.0f, 0.5f), 1.0e-2f);
        CHECK(stage.world.transform(guard).rotation == own);
        CHECK(stage.world.transform(guard).scale == Vec3(1.0f));

        // What it hangs under does not carry it along while time passes: the group is
        // moved, and the character is where it was - and its entity with it.
        controllerOf(stage, guard).movement = Vec3(0.0f);
        stage.run(2);
        const Vec3 stoodAt = stage.positionOf(guard);
        stage.world.transform(group).position += Vec3(0.0f, 0.0f, 7.0f);
        stage.run(1);
        CHECK_NEAR(stage.positionOf(guard), stoodAt, 2.0e-3f);
        // While no time passes the world has the say, and the character goes where the
        // group has taken its entity.
        stage.world.transform(group).position += Vec3(3.0f, 0.0f, 0.0f);
        stage.physics->synchronize(stage.world, *stage.assets);
        const CharacterState state = stage.physics->simulation().characterState(stage.physics->characterOf(guard));
        CHECK_NEAR(state.position, stoodAt + Vec3(3.0f, 0.0f, 0.0f), 2.0e-3f);
        CHECK_NEAR(stage.positionOf(guard), stoodAt + Vec3(3.0f, 0.0f, 0.0f), 2.0e-3f);
    }

    TEST_CASE("an entity that is put somewhere else takes its character along") {
        Stage stage;
        stage.addFloor();
        const LogCapture log;
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        stage.run(5);
        const CharacterId character = stage.physics->characterOf(guard);

        stage.world.transform(guard).position = Vec3(5.0f, 3.0f, -5.0f);
        stage.run(1);
        CHECK(stage.physics->characterOf(guard) == character);
        // It is there, and has begun to fall from there.
        const CharacterState state = stage.physics->simulation().characterState(character);
        CHECK(state.position.x == doctest::Approx(5.0f));
        CHECK(state.position.z == doctest::Approx(-5.0f));
        CHECK(state.position.y == doctest::Approx(3.0f).epsilon(0.01));
        CHECK(state.position.y < 3.0f);
        CHECK(controllerOf(stage, guard).ground == GroundState::InAir);
        stage.run(120);
        CHECK_NEAR(stage.world.transform(guard).position, Vec3(5.0f, 0.0f, -5.0f), 5.0e-3f);

        // Put nowhere, the entity is named once - and its character stays where it was.
        stage.world.transform(guard).position = Vec3(kNotANumber, 0.0f, 0.0f);
        stage.run(3);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Guard' is nowhere - its place in the world is not made of numbers - and its character "
                           "stays where it was"));
        CHECK_NEAR(stage.physics->simulation().characterState(character).position, Vec3(5.0f, 0.0f, -5.0f), 5.0e-3f);
        // Put somewhere again, it is there.
        stage.world.transform(guard).position = Vec3(1.0f, 0.0f, 1.0f);
        stage.run(3);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK_NEAR(stage.physics->simulation().characterState(character).position, Vec3(1.0f, 0.0f, 1.0f), 5.0e-3f);
    }

    TEST_CASE("a character that is to be made of other things is made anew where it is, and goes on as it did") {
        Stage stage;
        stage.addFloor();
        // It stands in a zone that notices it.
        const Entity zone = stage.world.createEntity("Zone");
        stage.world.transform(zone).position = Vec3(0.0f, 2.0f, 0.0f);
        stage.world.add<ColliderComponent>(zone) = {.size = Vec3(6.0f, 4.0f, 6.0f), .trigger = true};
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        controllerOf(stage, guard).movement = Vec3(1.0f, 0.0f, 0.0f);
        std::vector<CollisionEvent> heard = runAndListen(stage, 5);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].type == CollisionEventType::TriggerEntered);
        CHECK(heard[0].first == zone);
        CHECK(heard[0].second == guard);

        // It jumps, and is made smaller on the way up.
        controllerOf(stage, guard).jump = 5.0f;
        stage.run(6);
        const CharacterId before = stage.physics->characterOf(guard);
        const Vec3 was = stage.world.transform(guard).position;
        const Vec3 went = controllerOf(stage, guard).velocity;
        REQUIRE(went.y > 3.0f);
        controllerOf(stage, guard).height = 1.2f;
        heard = runAndListen(stage, 1);
        const CharacterId after = stage.physics->characterOf(guard);
        CHECK(after != before);
        CHECK_FALSE(stage.physics->simulation().isValid(before));
        // On its way as before: where a step from there has taken it, at the speed that
        // gravity has left it.
        CHECK(controllerOf(stage, guard).velocity.x == doctest::Approx(1.0f));
        CHECK(controllerOf(stage, guard).velocity.y == doctest::Approx(went.y - 9.81f * kPhysicsStep).epsilon(0.001));
        CHECK_NEAR(stage.world.transform(guard).position, was + controllerOf(stage, guard).velocity * kPhysicsStep,
                   1.0e-3f);
        // It has not left the zone and entered it again.
        CHECK(heard.empty());
        // Nor is it another character while nothing about it changes.
        stage.run(30);
        CHECK(stage.physics->characterOf(guard) == after);
        CHECK(stage.heightAt(stage.positionOf(guard).x, 0.0f) - stage.positionOf(guard).y ==
              doctest::Approx(0.6f + 0.9f * 0.6f).epsilon(0.01));
    }

    TEST_CASE("what is switched off, removed or destroyed is no character any more") {
        Stage stage;
        stage.addFloor();
        const Entity group = stage.world.createEntity("Group");
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        REQUIRE(stage.world.setParent(guard, group, false).has_value());
        CHECK(stage.run(2).characters == 1);
        const CharacterId character = stage.physics->characterOf(guard);

        stage.world.setEnabled(group, false);
        CHECK(stage.run(1).characters == 0);
        CHECK(stage.physics->characterOf(guard).isNull());
        CHECK_FALSE(stage.physics->simulation().isValid(character));
        CHECK(stage.under(0.0f, 0.0f) == "Floor");
        stage.world.setEnabled(group, true);
        CHECK(stage.run(1).characters == 1);

        stage.world.remove<CharacterControllerComponent>(guard);
        CHECK(stage.run(1).characters == 0);
        stage.world.add<CharacterControllerComponent>(guard);
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).characters == 1);
        // clear() forgets the characters with the bodies, and the next update makes them again.
        stage.physics->clear();
        CHECK(stage.physics->simulation().statistics().characters == 0);
        CHECK(stage.physics->characterOf(guard).isNull());
        CHECK(stage.run(1).characters == 1);

        stage.world.destroyEntity(guard);
        const PhysicsFrameStatistics statistics = stage.run(1);
        CHECK(statistics.characters == 0);
        CHECK(statistics.bodies == 1);
    }

    TEST_CASE("a character enters and leaves triggers, and a trigger says nothing of what it hangs on") {
        Stage stage;
        stage.addFloor();
        const Entity zone = stage.world.createEntity("Zone");
        stage.world.transform(zone).position = Vec3(3.0f, 1.0f, 0.0f);
        stage.world.add<ColliderComponent>(zone) = {.size = Vec3(1.0f, 2.0f, 4.0f), .trigger = true};
        // A character with a trigger about it - what it can reach - and a crate ahead.
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        const Entity reach = stage.world.createEntity("Reach");
        REQUIRE(stage.world.setParent(reach, guard, false).has_value());
        stage.world.transform(reach).position = Vec3(0.0f, 0.9f, 0.0f);
        stage.world.add<ColliderComponent>(reach) = {.shape = ShapeType::Sphere, .radius = 1.5f, .trigger = true};
        const Entity crate = stage.addCrate(Vec3(8.0f, 0.5f, 0.0f));
        // And a wagon that the simulation moves, with a trigger on an entity below it
        // that reaches into the wagon for as long as it is there.
        const Entity wagon = stage.addCrate(Vec3(0.0f, 0.5f, 20.0f), "Wagon");
        const Entity sensor = stage.world.createEntity("Sensor");
        REQUIRE(stage.world.setParent(sensor, wagon, false).has_value());
        stage.world.add<ColliderComponent>(sensor) = {.size = Vec3(2.0f), .trigger = true};

        controllerOf(stage, guard).movement = Vec3(2.0f, 0.0f, 0.0f);
        std::vector<CollisionEvent> heard = runAndListen(stage, 200);
        // What the crates say of the floor they stand on is not what is asked about here.
        std::erase_if(heard, [](const CollisionEvent& event) {
            return event.type == CollisionEventType::CollisionBegan || event.type == CollisionEventType::CollisionEnded;
        });
        // The zone says that the character came and went; the reach of the character
        // says that the crate came - and nobody says anything of what it hangs on.
        REQUIRE(heard.size() == 3);
        CHECK(heard[0].type == CollisionEventType::TriggerEntered);
        CHECK(heard[0].first == zone);
        CHECK(heard[0].second == guard);
        CHECK(heard[1].type == CollisionEventType::TriggerLeft);
        CHECK(heard[1].first == zone);
        CHECK(heard[1].second == guard);
        CHECK(heard[2].type == CollisionEventType::TriggerEntered);
        CHECK(heard[2].first == reach);
        CHECK(heard[2].second == crate);
        // The trigger moves with the character it hangs on.
        CHECK_NEAR(stage.physics->simulation().position(stage.physics->triggerOf(reach)),
                   stage.positionOf(guard) + Vec3(0.0f, 0.9f, 0.0f), 0.05f);
    }

    TEST_CASE("a character says which entity it stands on: a body, or another character") {
        Stage stage;
        const Entity floor = stage.addFloor();
        const Entity crate = stage.addCrate(Vec3(4.0f, 0.5f, 0.0f));
        const Entity onFloor = addCharacter(stage, Vec3(0.0f), "Lower");
        const Entity onCrate = addCharacter(stage, Vec3(4.0f, 1.0f, 0.0f), "Rider");
        const Entity onHead = addCharacter(stage, Vec3(0.0f, 2.0f, 0.0f), "Upper");
        stage.run(60);
        CHECK(controllerOf(stage, onFloor).groundEntity == stage.world.id(floor));
        CHECK(controllerOf(stage, onCrate).groundEntity == stage.world.id(crate));
        CHECK(controllerOf(stage, onHead).groundEntity == stage.world.id(onFloor));
        CHECK(controllerOf(stage, onHead).ground == GroundState::OnGround);

        // What it stood on is destroyed: it stands on nothing, and then on the floor.
        stage.world.destroyEntity(crate);
        stage.run(1);
        CHECK(controllerOf(stage, onCrate).groundEntity.isNil());
        CHECK(controllerOf(stage, onCrate).ground == GroundState::InAir);
        stage.run(60);
        CHECK(controllerOf(stage, onCrate).groundEntity == stage.world.id(floor));
    }

    TEST_CASE("an entity is not a character and a body, and only triggers and rigid bodies hang on a character") {
        Stage stage;
        stage.addFloor();
        const LogCapture log;
        // A character that was given a collider and a rigid body, as a body would be.
        const Entity guard = addCharacter(stage, Vec3(0.0f, 1.0f, 0.0f));
        stage.world.add<ColliderComponent>(guard) = {.shape = ShapeType::Capsule, .radius = 0.3f, .height = 1.8f};
        stage.world.add<RigidBodyComponent>(guard);
        PhysicsFrameStatistics statistics = stage.run(3);
        // It is the body, and no character.
        CHECK(statistics.characters == 0);
        CHECK(statistics.bodies == 2);
        CHECK(stage.physics->characterOf(guard).isNull());
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Guard' is no character: it is a body as well - it has a RigidBody, or a Collider that "
                           "is no trigger - and cannot be both"));
        std::vector<PhysicsProblem> problems = findPhysicsProblems(stage.world);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].entity == guard);
        CHECK(problems[0].message == "CharacterController: the entity is a body as well - it has a RigidBody - and "
                                     "cannot be both. A character has a capsule of its own");
        // The collider alone is a body as well: one that stands still.
        stage.world.remove<RigidBodyComponent>(guard);
        CHECK(stage.run(1).characters == 0);
        problems = findPhysicsProblems(stage.world);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message == "CharacterController: the entity is a body as well - it has a Collider that is "
                                     "no trigger - and cannot be both. A character has a capsule of its own");
        // Without either, it is a character - and one whose collider is a trigger is as well.
        stage.world.tryGet<ColliderComponent>(guard)->trigger = true;
        statistics = stage.run(1);
        CHECK(statistics.characters == 1);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(findPhysicsProblems(stage.world).empty());
        stage.world.remove<ColliderComponent>(guard);

        // A shield that hangs on the character is left out: it would stand still, and be
        // in the way of the character wherever that goes.
        const Entity shield = stage.world.createEntity("Shield");
        REQUIRE(stage.world.setParent(shield, guard, false).has_value());
        stage.world.transform(shield).position = Vec3(0.5f, 1.0f, 0.0f);
        stage.world.add<ColliderComponent>(shield) = {.size = Vec3(0.1f, 1.0f, 1.0f)};
        statistics = stage.run(3);
        CHECK(statistics.bodies == 2);
        CHECK(stage.physics->bodyOf(shield).isNull());
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("'Shield' has a collider that is left out: it hangs on the character 'Guard' and is "
                           "part of no rigid body, and only triggers and rigid bodies hang on a character"));
        problems = findPhysicsProblems(stage.world);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].entity == shield);
        CHECK(problems[0].message ==
              "Collider: it hangs on the character 'Guard' and is part of no rigid body. It would stand still, be "
              "carried about, and be in the way of the character wherever that goes: only triggers and rigid "
              "bodies hang on a character");
        // And the character walks, with nothing in its way.
        controllerOf(stage, guard).movement = Vec3(1.0f, 0.0f, 0.0f);
        stage.run(60);
        CHECK(stage.positionOf(guard).x == doctest::Approx(1.0f).epsilon(0.02));
        controllerOf(stage, guard).movement = Vec3(0.0f);

        // As a part of a rigid body it is that body's, whatever the body hangs on; and
        // a trigger is a trigger anywhere.
        stage.world.add<RigidBodyComponent>(shield) = {.motion = BodyMotion::Kinematic};
        statistics = stage.run(1);
        CHECK(statistics.bodies == 3);
        CHECK(stage.physics->simulation().isValid(stage.physics->bodyOf(shield)));
        CHECK(findPhysicsProblems(stage.world).empty());
        stage.world.remove<RigidBodyComponent>(shield);
        stage.world.tryGet<ColliderComponent>(shield)->trigger = true;
        statistics = stage.run(1);
        CHECK(statistics.bodies == 3);
        CHECK(stage.physics->simulation().isValid(stage.physics->triggerOf(shield)));
        CHECK(findPhysicsProblems(stage.world).empty());
        CHECK(log.count(LogLevel::Error) == 2);
    }

    TEST_CASE("a character that cannot be made is named once, and made when it can be") {
        Stage stage;
        stage.addFloor();
        const LogCapture log;
        const Entity guard = addCharacter(stage, Vec3(0.0f));
        // Scaled to next to nothing, it is thinner than a character can be.
        stage.world.transform(guard).scale = Vec3(0.01f);
        CHECK(stage.run(3).characters == 0);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Guard' has no character: a character is at least 0.05 m around"));
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).characters == 0);
        CHECK(log.count(LogLevel::Error) == 1);

        stage.world.transform(guard).scale = Vec3(1.0f);
        CHECK(stage.run(1).characters == 1);
        // Given a value that no character has - which a file or an editor would refuse -
        // the one it was is gone, and it is named again.
        controllerOf(stage, guard).mass = -1.0f;
        CHECK(stage.run(3).characters == 0);
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("'Guard' has no character: a character weighs more than nothing, not -1 kg"));
        controllerOf(stage, guard).mass = 70.0f;
        CHECK(stage.run(1).characters == 1);
        CHECK(log.count(LogLevel::Error) == 2);
    }

    TEST_CASE("characters and bodies under each other are each put where they are") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        // A crate that the simulation moves, a character below it in the hierarchy, and
        // another crate below that: none carries what hangs under it, and each is where
        // the simulation has it, whatever order they are written in.
        const Entity crate = stage.addCrate(Vec3(0.0f, 10.0f, 0.0f), "Outer");
        *stage.world.tryGet<RigidBodyComponent>(crate) = {.linearDamping = 0.0f,
                                                          .linearVelocity = Vec3(3.0f, 0.0f, 0.0f)};
        const Entity guard = stage.world.createEntity("Guard");
        REQUIRE(stage.world.setParent(guard, crate, false).has_value());
        stage.world.transform(guard).position = Vec3(0.0f, 5.0f, 0.0f);
        stage.world.add<CharacterControllerComponent>(guard) = {.gravityScale = 0.0f};
        const Entity inner = stage.world.createEntity("Inner");
        REQUIRE(stage.world.setParent(inner, guard, false).has_value());
        stage.world.transform(inner).position = Vec3(0.0f, 5.0f, 0.0f);
        stage.world.add<ColliderComponent>(inner);
        stage.world.add<RigidBodyComponent>(inner) = {.linearDamping = 0.0f, .linearVelocity = Vec3(0.0f, 0.0f, -2.0f)};
        controllerOf(stage, guard).movement = Vec3(0.0f, 0.0f, 1.0f);

        stage.run(60);
        CHECK_NEAR(stage.positionOf(crate), Vec3(3.0f, 10.0f, 0.0f), 1.0e-2f);
        CHECK_NEAR(stage.positionOf(guard), Vec3(0.0f, 15.0f, 1.0f), 1.0e-2f);
        CHECK_NEAR(stage.positionOf(inner), Vec3(0.0f, 20.0f, -2.0f), 1.0e-2f);
        // And so the world's own matrices say, once they are worked out.
        stage.world.updateWorldTransforms();
        CHECK_NEAR(Vec3(stage.world.worldMatrix(guard)[3]), Vec3(0.0f, 15.0f, 1.0f), 1.0e-2f);
        CHECK_NEAR(Vec3(stage.world.worldMatrix(inner)[3]), Vec3(0.0f, 20.0f, -2.0f), 1.0e-2f);
    }

    TEST_CASE("the character controller is registered, described, and saved with what it is made of") {
        ComponentRegistry registry;
        REQUIRE(registerPhysicsComponents(registry).has_value());
        const ComponentDescriptor* descriptor = registry.find("CharacterController");
        REQUIRE(descriptor != nullptr);
        const TypeInfo& type = descriptor->type;
        CHECK(type.findProperty("radius")->attributes().unit == "m");
        CHECK(type.findProperty("maxSlope")->attributes().unit == "deg");
        CHECK(type.findProperty("groundEntity")->attributes().semantic == PropertySemantic::EntityReference);

        CharacterControllerComponent controller{.radius = 0.4f,
                                                .height = 2.0f,
                                                .mass = 90.0f,
                                                .maxSlope = 35.0f,
                                                .stepHeight = 0.5f,
                                                .pushForce = 250.0f,
                                                .gravityScale = 0.5f,
                                                .airControl = 0.25f,
                                                .movement = Vec3(1.0f, 0.0f, 2.0f),
                                                .jump = 4.0f,
                                                .velocity = Vec3(3.0f),
                                                .ground = GroundState::OnSteepGround,
                                                .groundNormal = Vec3(1.0f, 0.0f, 0.0f),
                                                .groundEntity = *Uuid::parse("10000000-0000-4000-8000-000000000002")};
        // What it is told and what the simulation says of it are not part of a scene.
        const Json saved = type.toJson(&controller);
        CHECK(saved == unwrap(parseJson(R"({"radius": 0.4, "height": 2.0, "mass": 90.0, "maxSlope": 35.0,
                                           "stepHeight": 0.5, "pushForce": 250.0, "gravityScale": 0.5,
                                           "airControl": 0.25})")));
        CharacterControllerComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.radius == 0.4f);
        CHECK(loaded.height == 2.0f);
        CHECK(loaded.mass == 90.0f);
        CHECK(loaded.maxSlope == 35.0f);
        CHECK(loaded.stepHeight == 0.5f);
        CHECK(loaded.pushForce == 250.0f);
        CHECK(loaded.gravityScale == 0.5f);
        CHECK(loaded.airControl == 0.25f);
        CHECK(loaded.movement == Vec3(0.0f));
        CHECK(loaded.ground == GroundState::InAir);

        // An edit - by a person, a script or an agent - tells it how to walk and to jump...
        const Result<PropertyPatch> told = type.parseEdit(unwrap(parseJson(R"({"movement": [0, 0, 1.5], "jump": 3})")));
        REQUIRE(told.has_value());
        told->applyTo(&loaded);
        CHECK(loaded.movement == Vec3(0.0f, 0.0f, 1.5f));
        CHECK(loaded.jump == 3.0f);
        // ...and does not say what only the simulation has to say.
        for (const char* text : {R"({"velocity": [1, 0, 0]})", R"({"ground": "onGround"})",
                                 R"({"groundNormal": [0, 1, 0]})", R"({"groundEntity": null})"}) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.parseEdit(unwrap(parseJson(text))).has_value());
        }
        // What no character is made of is refused.
        for (const char* text :
             {R"({"radius": 0.01})", R"({"height": 0})", R"({"mass": 0})", R"({"maxSlope": 90})", R"({"maxSlope": -1})",
              R"({"stepHeight": -0.1})", R"({"pushForce": -1})", R"({"airControl": 1.5})", R"({"airControl": -0.1})",
              R"({"gravityScale": 1000})", R"({"crouching": true})"}) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&loaded, unwrap(parseJson(text))).has_value());
        }
        CHECK_FALSE(type.parseEdit(unwrap(parseJson(R"({"jump": -1})"))).has_value());
    }
}
