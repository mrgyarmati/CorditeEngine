#include "cordite/physics/PhysicsSystem.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();
constexpr float kStep = 1.0f / 60.0f;

/// A world, and the physics that goes with it.
struct Stage {
    Stage() : physics(unwrap(PhysicsSystem::create())) {}

    /// A floor whose top is the plane y = 0: a collider alone, which stands still.
    Entity addFloor() {
        const Entity floor = world.createEntity("Floor");
        world.add<ColliderComponent>(floor) = {.size = Vec3(50.0f, 1.0f, 50.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
        return floor;
    }

    /// A box of one metre that the simulation moves.
    Entity addCrate(const Vec3& position, const char* name = "Crate") {
        const Entity crate = world.createEntity(name);
        world.transform(crate).position = position;
        world.add<ColliderComponent>(crate);
        world.add<RigidBodyComponent>(crate);
        return crate;
    }

    /// Lets frames of a sixtieth of a second pass.
    PhysicsFrameStatistics run(int frames) {
        PhysicsFrameStatistics last;
        for (int frame = 0; frame < frames; ++frame) {
            last = physics->update(world, kStep);
        }
        return last;
    }

    /// Where an entity is in the world, worked out from the transforms as they are now.
    [[nodiscard]] Vec3 positionOf(Entity entity) const { return Vec3(world.computeWorldMatrix(entity)[3]); }

    /// What a ray straight down from high above (x, z) hits, as the name of its entity.
    [[nodiscard]] std::string under(float x, float z) const {
        const std::optional<RayHit> hit =
            physics->simulation().raycast(Ray{Vec3(x, 100.0f, z), Vec3(0.0f, -1.0f, 0.0f)}, 1000.0f);
        if (!hit) {
            return "(nothing)";
        }
        const Entity entity = physics->entityOf(hit->body);
        return entity.isNull() ? "(no entity)" : world.name(entity);
    }

    /// How high above the ground a ray straight down from high above (x, z) hits.
    [[nodiscard]] float heightAt(float x, float z) const {
        const std::optional<RayHit> hit =
            physics->simulation().raycast(Ray{Vec3(x, 100.0f, z), Vec3(0.0f, -1.0f, 0.0f)}, 1000.0f);
        return hit ? hit->position.y : -1000.0f;
    }

    World world;
    std::unique_ptr<PhysicsSystem> physics;
};

} // namespace

TEST_SUITE("PhysicsSystem") {
    TEST_CASE("a collider alone stands still, and with a rigid body it falls and comes to rest") {
        Stage stage;
        const Entity floor = stage.addFloor();
        const Entity crate = stage.addCrate(Vec3(1.0f, 3.0f, 2.0f));

        PhysicsFrameStatistics statistics = stage.run(1);
        CHECK(statistics == PhysicsFrameStatistics{.steps = 1, .bodies = 2, .activeBodies = 1});
        // One step: the crate has gained speed, and moved by it. Its entity is where it is.
        CHECK(stage.world.transform(crate).position.y == doctest::Approx(3.0f - 9.81f * kStep * kStep).epsilon(1.0e-5));
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity.y < -0.16f);
        CHECK(stage.world.transform(floor).position == Vec3(0.0f));

        statistics = stage.run(240);
        CHECK(stage.world.transform(crate).position.y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(std::abs(stage.world.transform(crate).position.x - 1.0f) < 0.01f);
        // At rest, the crate sleeps: nothing is simulated, and its entity is left alone.
        CHECK(statistics == PhysicsFrameStatistics{.steps = 1, .bodies = 2, .activeBodies = 0});
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity == Vec3(0.0f));
        const Transform atRest = stage.world.transform(crate);
        stage.run(10);
        CHECK(stage.world.transform(crate) == atRest);

        // Entities and bodies know each other.
        const BodyId body = stage.physics->bodyOf(crate);
        CHECK(stage.physics->simulation().isValid(body));
        CHECK(stage.physics->entityOf(body) == crate);
        CHECK(stage.physics->entityOf(stage.physics->bodyOf(floor)) == floor);
        CHECK(stage.physics->simulation().motion(body) == BodyMotion::Dynamic);
        CHECK(stage.physics->simulation().motion(stage.physics->bodyOf(floor)) == BodyMotion::Static);
        CHECK(stage.under(1.0f, 2.0f) == "Crate");
        CHECK(stage.under(10.0f, 10.0f) == "Floor");
        CHECK(stage.physics->entityOf(BodyId()).isNull());
        CHECK(stage.physics->bodyOf(Entity{}).isNull());
    }

    TEST_CASE("time passes in steps of one length, however long the frames are") {
        Stage stage;
        stage.addCrate(Vec3(0.0f, 100.0f, 0.0f));
        // Two frames of half a step make one step.
        CHECK(stage.physics->update(stage.world, 0.5f * kStep).steps == 0);
        CHECK(stage.physics->update(stage.world, 0.5f * kStep).steps == 1);
        // A frame of a tenth of a second makes six.
        CHECK(stage.physics->update(stage.world, 0.1f).steps == 6);
        // A pause is not caught up with: eight steps to an update at the most.
        CHECK(stage.physics->update(stage.world, 5.0f).steps == 8);
        CHECK(stage.physics->update(stage.world, kStep).steps == 1);
        // Frames that are no time are no steps.
        CHECK(stage.physics->update(stage.world, 0.0f).steps == 0);
        CHECK(stage.physics->update(stage.world, -1.0f).steps == 0);
        CHECK(stage.physics->update(stage.world, kNotANumber).steps == 0);
        CHECK(stage.physics->simulation().statistics().steps == 16);

        // Other steps can be asked for; none at all cannot.
        const std::unique_ptr<PhysicsSystem> coarse =
            unwrap(PhysicsSystem::create({.stepSeconds = 0.1, .maxStepsPerUpdate = 2}));
        CHECK(coarse->update(stage.world, 0.25f).steps == 2);
        CHECK(coarse->update(stage.world, 10.0f).steps == 2);
        for (const PhysicsSystemDesc& desc :
             {PhysicsSystemDesc{.stepSeconds = 0.0}, PhysicsSystemDesc{.stepSeconds = -1.0},
              PhysicsSystemDesc{.stepSeconds = std::numeric_limits<double>::quiet_NaN()},
              PhysicsSystemDesc{.maxStepsPerUpdate = 0}, PhysicsSystemDesc{.world = {.maxBodies = 0}}}) {
            const auto refused = PhysicsSystem::create(desc);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == ErrorCode::InvalidArgument);
        }
    }

    TEST_CASE("synchronizing lets no time pass: bodies are where the entities are, and nothing falls") {
        Stage stage;
        stage.addFloor();
        const Entity crate = stage.addCrate(Vec3(0.0f, 5.0f, 0.0f));
        CHECK(stage.physics->synchronize(stage.world) ==
              PhysicsFrameStatistics{.steps = 0, .bodies = 2, .activeBodies = 1});
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(5.5f).epsilon(1.0e-4));
        for (int index = 0; index < 30; ++index) {
            stage.physics->synchronize(stage.world);
        }
        CHECK(stage.world.transform(crate).position == Vec3(0.0f, 5.0f, 0.0f));
        // An entity that is moved has its body there at once - dynamic, static or kinematic.
        stage.world.transform(crate).position = Vec3(3.0f, 2.0f, 0.0f);
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.add<ColliderComponent>(wall);
        const Entity lift = stage.world.createEntity("Lift");
        stage.world.add<ColliderComponent>(lift);
        stage.world.add<RigidBodyComponent>(lift).motion = BodyMotion::Kinematic;
        stage.physics->synchronize(stage.world);
        stage.world.transform(wall).position = Vec3(-6.0f, 4.0f, 0.0f);
        stage.world.transform(lift).position = Vec3(-9.0f, 7.0f, 0.0f);
        stage.physics->synchronize(stage.world);
        CHECK(stage.under(3.0f, 0.0f) == "Crate");
        CHECK(stage.heightAt(3.0f, 0.0f) == doctest::Approx(2.5f).epsilon(1.0e-4));
        CHECK(stage.heightAt(-6.0f, 0.0f) == doctest::Approx(4.5f).epsilon(1.0e-4));
        CHECK(stage.heightAt(-9.0f, 0.0f) == doctest::Approx(7.5f).epsilon(1.0e-4));
        CHECK(stage.under(0.0f, 0.0f) == "Floor");
    }

    TEST_CASE("an entity follows its body under a parent that is placed, turned and scaled") {
        Stage stage;
        stage.addFloor();
        const Entity group = stage.world.createEntity("Group");
        stage.world.transform(group) = {.position = Vec3(10.0f, 0.0f, -4.0f),
                                        .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f)),
                                        .scale = Vec3(2.0f)};
        const Entity crate = stage.addCrate(Vec3(0.0f, 3.0f, 0.0f));
        REQUIRE(stage.world.setParent(crate, group, false).has_value());
        // In the world it is six metres up, and twice as large: a box of two metres.
        stage.run(300);
        CHECK(stage.positionOf(crate).y == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(std::abs(stage.positionOf(crate).x - 10.0f) < 0.02f);
        CHECK(std::abs(stage.positionOf(crate).z + 4.0f) < 0.02f);
        // Against its parent that is half a metre up, and its own scale is untouched.
        CHECK(stage.world.transform(crate).position.y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(stage.world.transform(crate).scale == Vec3(1.0f));
        CHECK(stage.heightAt(10.0f, -4.0f) == doctest::Approx(2.0f).epsilon(0.02));
        // The entity is where the body is.
        CHECK_NEAR(stage.positionOf(crate), stage.physics->simulation().position(stage.physics->bodyOf(crate)),
                   1.0e-4f);

        // A body is not carried along by what its entity hangs under: moving the group
        // leaves the crate where it lies in the world - the body, which sleeps on, and
        // the entity with it.
        const BodyId body = stage.physics->bodyOf(crate);
        const Vec3 lies = stage.physics->simulation().position(body);
        const Vec3 up = stage.physics->simulation().rotation(body) * Vec3(0.0f, 1.0f, 0.0f);
        stage.world.transform(group).position = Vec3(-30.0f, 0.0f, 0.0f);
        CHECK(stage.run(1).activeBodies == 0);
        CHECK_NEAR(stage.physics->simulation().position(body), lies, 1.0e-3f);
        CHECK_NEAR(stage.positionOf(crate), lies, 1.0e-3f);
        // In a frame without a step too, and turned as the body is: the entity's Y axis,
        // two long under a group of twice the size, is the body's.
        stage.world.transform(group).rotation = glm::angleAxis(1.0f, Vec3(0.0f, 0.0f, 1.0f));
        CHECK(stage.physics->update(stage.world, 0.0f) ==
              PhysicsFrameStatistics{.steps = 0, .bodies = 2, .activeBodies = 0});
        CHECK_NEAR(stage.positionOf(crate), lies, 1.0e-3f);
        CHECK_NEAR(0.5f * Vec3(stage.world.computeWorldMatrix(crate)[1]), up, 1.0e-3f);
        CHECK(stage.world.transform(crate).scale == Vec3(1.0f));
    }

    TEST_CASE("a body at rest stays where it is while the body its entity hangs under moves") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        const Entity carrier = stage.addCrate(Vec3(0.0f), "Carrier");
        *stage.world.tryGet<RigidBodyComponent>(carrier) = {.linearDamping = 0.0f,
                                                            .angularDamping = 0.0f,
                                                            .linearVelocity = Vec3(3.0f, 0.0f, 0.0f),
                                                            .angularVelocity = Vec3(0.0f, 0.0f, 2.0f)};
        // Something that hangs under it in the scene, and is a body of its own.
        const Entity loose = stage.addCrate(Vec3(0.0f, 5.0f, 0.0f), "Loose");
        REQUIRE(stage.world.setParent(loose, carrier, false).has_value());

        // In every frame - with a step, and without one - it is where its body is: five
        // metres up, and upright. Not one frame behind what it hangs under, either.
        for (int frame = 0; frame < 90; ++frame) {
            stage.physics->update(stage.world, frame % 3 == 2 ? 0.0f : kStep);
            REQUIRE(glm::distance(stage.positionOf(loose), Vec3(0.0f, 5.0f, 0.0f)) < 1.0e-3f);
            REQUIRE(glm::distance(Vec3(stage.world.computeWorldMatrix(loose)[1]), Vec3(0.0f, 1.0f, 0.0f)) < 1.0e-3f);
        }
        // Sixty steps at three metres a second and two radians a second: the carrier
        // has gone three metres, and turned.
        CHECK(stage.positionOf(carrier).x == doctest::Approx(3.0f).epsilon(1.0e-3));
        CHECK(glm::angle(stage.world.transform(carrier).rotation) == doctest::Approx(2.0f).epsilon(1.0e-3));
        // Against it, what hangs under it is somewhere else every frame.
        CHECK(glm::distance(stage.world.transform(loose).position, Vec3(0.0f, 5.0f, 0.0f)) > 1.0f);
    }

    TEST_CASE("while no time passes a body goes where its entity is taken, and the world is left as it is") {
        Stage stage;
        const Entity group = stage.world.createEntity("Group");
        stage.world.transform(group).position = Vec3(4.0f, 0.0f, 0.0f);
        const Entity crate = stage.addCrate(Vec3(0.0f, 2.0f, 0.0f));
        REQUIRE(stage.world.setParent(crate, group, false).has_value());
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.transform(wall).position = Vec3(0.0f, 2.0f, 5.0f);
        stage.world.add<ColliderComponent>(wall);
        REQUIRE(stage.world.setParent(wall, group, false).has_value());
        stage.physics->synchronize(stage.world);
        CHECK(stage.under(4.0f, 0.0f) == "Crate");
        CHECK(stage.under(4.0f, 5.0f) == "Wall");

        // What they hang under is moved and turned, as in an editor. Both bodies are
        // there - the dynamic one too, which stays where it is while time passes.
        const Transform before = stage.world.transform(crate);
        stage.world.transform(group) = {.position = Vec3(-10.0f, 1.0f, 0.0f),
                                        .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f))};
        stage.physics->synchronize(stage.world);
        CHECK(stage.under(-10.0f, 0.0f) == "Crate");
        CHECK(stage.heightAt(-10.0f, 0.0f) == doctest::Approx(3.5f).epsilon(1.0e-4));
        // Five metres along the group's Z, which a quarter turn about Y has made the world's X.
        CHECK(stage.under(-5.0f, 0.0f) == "Wall");
        CHECK(stage.under(4.0f, 0.0f) == "(nothing)");
        // Nothing was written: the crate is what the scene says it is, to the last digit.
        CHECK(stage.world.transform(crate) == before);
        // Synchronized again, nothing is put anywhere: the body is the one it was.
        const BodyId body = stage.physics->bodyOf(crate);
        stage.physics->synchronize(stage.world);
        CHECK(stage.physics->bodyOf(crate) == body);
        CHECK(stage.world.transform(crate) == before);

        // When time passes again, the crate falls from where it was taken.
        stage.run(1);
        CHECK(stage.positionOf(crate).y < 3.0f);
        CHECK(stage.positionOf(crate).y > 2.99f);
        CHECK(std::abs(stage.positionOf(crate).x + 10.0f) < 1.0e-3f);
    }

    TEST_CASE("a body that stands still follows its entity however slowly that is moved") {
        Stage stage;
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.add<ColliderComponent>(wall);
        stage.physics->synchronize(stage.world);
        const BodyId body = stage.physics->bodyOf(wall);
        // A thousand frames, each of a movement too small to tell from rounding: five
        // thousandths of a millimetre, and a hundred-thousandth of a radian.
        for (int frame = 1; frame <= 1000; ++frame) {
            stage.world.transform(wall).position.x = 5.0e-6f * static_cast<float>(frame);
            stage.world.transform(wall).rotation =
                glm::angleAxis(1.0e-5f * static_cast<float>(frame), Vec3(0.0f, 1.0f, 0.0f));
            if (frame % 2 == 0) {
                stage.physics->synchronize(stage.world);
            } else {
                stage.run(1);
            }
        }
        // Five millimetres and a hundredth of a radian in all, and the body is there.
        CHECK(stage.physics->bodyOf(wall) == body);
        CHECK_NEAR(stage.physics->simulation().position(body), Vec3(5.0e-3f, 0.0f, 0.0f), 2.0e-5f);
        const Vec3 along = stage.physics->simulation().rotation(body) * Vec3(1.0f, 0.0f, 0.0f);
        CHECK(along.z == doctest::Approx(-std::sin(1.0e-2f)).epsilon(0.01));
    }

    TEST_CASE("a dynamic body goes where its entity is put, and keeps its speed") {
        Stage stage;
        const Entity crate = stage.addCrate(Vec3(0.0f, 50.0f, 0.0f));
        stage.run(30);
        const Vec3 speed = stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity;
        CHECK(speed.y == doctest::Approx(-9.81f * 0.5f).epsilon(0.02));

        stage.world.transform(crate).position = Vec3(7.0f, 80.0f, -3.0f);
        stage.world.transform(crate).rotation = glm::angleAxis(1.0f, Vec3(0.0f, 0.0f, 1.0f));
        stage.physics->synchronize(stage.world);
        const BodyId body = stage.physics->bodyOf(crate);
        CHECK(stage.physics->simulation().position(body) == Vec3(7.0f, 80.0f, -3.0f));
        CHECK(stage.physics->simulation().linearVelocity(body) == speed);
        stage.run(1);
        CHECK(stage.world.transform(crate).position.x == doctest::Approx(7.0f));
        CHECK(stage.world.transform(crate).position.y < 80.0f);
        CHECK(stage.world.transform(crate).position.y > 79.8f);
    }

    TEST_CASE("the speeds of a rigid body are kept up to date, and setting them sets the body's") {
        Stage stage;
        const Entity crate = stage.addCrate(Vec3(0.0f, 50.0f, 0.0f));
        stage.world.tryGet<RigidBodyComponent>(crate)->gravityScale = 0.0f;
        stage.world.tryGet<RigidBodyComponent>(crate)->linearDamping = 0.0f;
        stage.world.tryGet<RigidBodyComponent>(crate)->angularDamping = 0.0f;
        // Given a speed before there is a body, the body starts with it.
        stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(3.0f, 0.0f, 0.0f);
        stage.run(60);
        CHECK(stage.world.transform(crate).position.x == doctest::Approx(3.0f).epsilon(1.0e-3));
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity == Vec3(3.0f, 0.0f, 0.0f));

        // Given another while it moves, it moves at that from the next step on.
        stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(0.0f, 0.0f, -6.0f);
        stage.world.tryGet<RigidBodyComponent>(crate)->angularVelocity = Vec3(0.0f, 2.0f, 0.0f);
        stage.run(60);
        CHECK(stage.world.transform(crate).position.x == doctest::Approx(3.0f).epsilon(1.0e-3));
        CHECK(stage.world.transform(crate).position.z == doctest::Approx(-6.0f).epsilon(1.0e-3));
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->angularVelocity.y == doctest::Approx(2.0f));
        // Two radians a second for a second: turned two radians about Y.
        CHECK(glm::angle(stage.world.transform(crate).rotation) == doctest::Approx(2.0f).epsilon(1.0e-3));

        // What the simulation does to the body shows in the component.
        stage.physics->simulation().addImpulse(stage.physics->bodyOf(crate), Vec3(0.0f, 4.0f, 0.0f));
        stage.run(1);
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity.y == doctest::Approx(4.0f));
        // A speed that is no number is not passed on.
        stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(kNotANumber);
        stage.run(1);
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity.y == doctest::Approx(4.0f));
    }

    TEST_CASE("a kinematic body follows its entity, and pushes what is in its way") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        const Entity platform = stage.world.createEntity("Platform");
        stage.world.transform(platform).position = Vec3(-5.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(platform) = {.size = Vec3(1.0f, 4.0f, 4.0f)};
        stage.world.add<RigidBodyComponent>(platform).motion = BodyMotion::Kinematic;
        const Entity crate = stage.addCrate(Vec3(-3.5f, 0.0f, 0.0f));
        stage.run(1);

        // Moved a tenth of a metre a frame: six metres a second.
        for (int frame = 1; frame <= 30; ++frame) {
            stage.world.transform(platform).position.x = -5.0f + 0.1f * static_cast<float>(frame);
            stage.run(1);
        }
        const BodyId body = stage.physics->bodyOf(platform);
        CHECK_NEAR(stage.physics->simulation().position(body), Vec3(-2.0f, 0.0f, 0.0f), 1.0e-3f);
        CHECK(stage.world.tryGet<RigidBodyComponent>(platform)->linearVelocity.x ==
              doctest::Approx(6.0f).epsilon(1.0e-3));
        // Its entity is its own to place: the simulation does not write it.
        CHECK(stage.world.transform(platform).position == Vec3(-2.0f, 0.0f, 0.0f));
        // The crate was pushed ahead of it.
        CHECK(stage.world.transform(crate).position.x > -1.1f);
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity.x > 5.0f);

        // Left where it is, it comes to a halt there.
        stage.run(2);
        CHECK(stage.world.tryGet<RigidBodyComponent>(platform)->linearVelocity == Vec3(0.0f));
        CHECK_NEAR(stage.physics->simulation().position(body), Vec3(-2.0f, 0.0f, 0.0f), 1.0e-3f);

        // In a frame of several steps it goes the way in all of them together.
        stage.world.transform(platform).position.x = 4.0f;
        CHECK(stage.physics->update(stage.world, 3.0f * kStep).steps == 3);
        CHECK_NEAR(stage.physics->simulation().position(body), Vec3(4.0f, 0.0f, 0.0f), 1.0e-3f);

        // Its speeds follow from how its entity is moved: given others, it does not take them.
        stage.world.tryGet<RigidBodyComponent>(platform)->linearVelocity = Vec3(0.0f, 5.0f, 0.0f);
        stage.world.tryGet<RigidBodyComponent>(platform)->angularVelocity = Vec3(0.0f, 5.0f, 0.0f);
        stage.run(3);
        CHECK(stage.world.tryGet<RigidBodyComponent>(platform)->linearVelocity == Vec3(0.0f));
        CHECK(stage.world.tryGet<RigidBodyComponent>(platform)->angularVelocity == Vec3(0.0f));
        CHECK_NEAR(stage.physics->simulation().position(body), Vec3(4.0f, 0.0f, 0.0f), 1.0e-3f);
    }

    TEST_CASE("a kinematic body moves in steps: a frame without one leaves it, the next takes it all the way") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        const Entity platform = stage.world.createEntity("Platform");
        stage.world.add<ColliderComponent>(platform) = {.size = Vec3(1.0f, 4.0f, 4.0f)};
        stage.world.add<RigidBodyComponent>(platform).motion = BodyMotion::Kinematic;
        const Entity crate = stage.addCrate(Vec3(1.5f, 0.0f, 0.0f));
        stage.run(1);
        const BodyId body = stage.physics->bodyOf(platform);

        // Frames of half a step, as on a display twice as fast as the simulation, and
        // the entity moved in every one of them: every other frame has a step.
        for (int frame = 1; frame <= 40; ++frame) {
            stage.world.transform(platform).position.x = 0.05f * static_cast<float>(frame);
            const uint32_t steps = stage.physics->update(stage.world, 0.5f * kStep).steps;
            REQUIRE(steps == (frame % 2 == 0 ? 1U : 0U));
            // Without a step the body is where the last one left it. Put ahead at once,
            // it would be inside what it is to push.
            const float expected = 0.05f * static_cast<float>(steps == 1 ? frame : frame - 1);
            REQUIRE(stage.physics->simulation().position(body).x == doctest::Approx(expected).epsilon(1.0e-3));
        }
        // A tenth of a metre to a step: six metres a second, and not three.
        CHECK(stage.world.tryGet<RigidBodyComponent>(platform)->linearVelocity.x ==
              doctest::Approx(6.0f).epsilon(1.0e-3));
        // The crate was pushed ahead all the way, at that speed.
        CHECK(stage.world.transform(crate).position.x > 2.9f);
        CHECK(stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity.x > 5.0f);
    }

    TEST_CASE("a rigid body is made of its own collider and of the colliders below it") {
        Stage stage;
        // A table: the top is the body's own collider, its legs hang below it.
        const Entity table = stage.world.createEntity("Table");
        stage.world.transform(table).position = Vec3(0.0f, 0.85f, 0.0f);
        stage.world.add<ColliderComponent>(table) = {.size = Vec3(2.0f, 0.1f, 1.0f)};
        stage.world.add<RigidBodyComponent>(table).motion = BodyMotion::Static;
        const Entity legs = stage.world.createEntity("Legs");
        REQUIRE(stage.world.setParent(legs, table, false).has_value());
        std::vector<Entity> made;
        for (const float x : {-0.9f, 0.9f}) {
            const Entity leg = stage.world.createEntity("Leg");
            stage.world.transform(leg).position = Vec3(x, -0.45f, 0.0f);
            stage.world.add<ColliderComponent>(leg) = {.size = Vec3(0.1f, 0.8f, 0.1f)};
            REQUIRE(stage.world.setParent(leg, legs, false).has_value());
            made.push_back(leg);
        }
        // Something on the table with a rigid body of its own is a body of its own, and
        // what hangs below that is part of it, not of the table.
        const Entity lamp = stage.world.createEntity("Lamp");
        stage.world.transform(lamp).position = Vec3(0.0f, 0.3f, 0.0f);
        stage.world.add<ColliderComponent>(lamp) = {.shape = ShapeType::Sphere, .radius = 0.2f};
        stage.world.add<RigidBodyComponent>(lamp).motion = BodyMotion::Static;
        REQUIRE(stage.world.setParent(lamp, table, false).has_value());
        const Entity shade = stage.world.createEntity("Shade");
        stage.world.transform(shade).position = Vec3(0.0f, 0.4f, 0.0f);
        stage.world.add<ColliderComponent>(shade) = {.size = Vec3(0.5f, 0.1f, 0.5f)};
        REQUIRE(stage.world.setParent(shade, lamp, false).has_value());

        CHECK(stage.physics->synchronize(stage.world).bodies == 2);
        const BodyId body = stage.physics->bodyOf(table);
        CHECK(stage.physics->simulation().isValid(body));
        // The legs have no bodies of their own: they are parts of the table's.
        CHECK(stage.physics->bodyOf(made[0]).isNull());
        CHECK(stage.physics->bodyOf(legs).isNull());
        CHECK(stage.physics->bodyOf(shade).isNull());
        // From the side, at the height of the legs: the table is hit, where a leg is.
        std::optional<RayHit> hit =
            stage.physics->simulation().raycast(Ray{Vec3(10.0f, 0.4f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(stage.physics->entityOf(hit->body) == table);
        CHECK(hit->position.x == doctest::Approx(0.95f).epsilon(1.0e-4));
        // From above: the shade, which is part of the lamp.
        CHECK(stage.under(0.2f, 0.2f) == "Lamp");
        CHECK(stage.heightAt(0.2f, 0.2f) == doctest::Approx(1.6f).epsilon(1.0e-4));

        // A collider under a collider that has no rigid body above it is a body of its
        // own, standing still.
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.transform(wall).position = Vec3(20.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(wall);
        const Entity ledge = stage.world.createEntity("Ledge");
        stage.world.transform(ledge).position = Vec3(2.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(ledge);
        REQUIRE(stage.world.setParent(ledge, wall, false).has_value());
        CHECK(stage.physics->synchronize(stage.world).bodies == 4);
        CHECK(stage.under(20.0f, 0.0f) == "Wall");
        CHECK(stage.under(22.0f, 0.0f) == "Ledge");
        CHECK(stage.physics->simulation().motion(stage.physics->bodyOf(ledge)) == BodyMotion::Static);
    }

    TEST_CASE("a body is made anew when what it is made of changes, and keeps moving as it did") {
        Stage stage;
        const Entity crate = stage.addCrate(Vec3(0.0f, 50.0f, 0.0f));
        stage.run(30);
        const BodyId before = stage.physics->bodyOf(crate);
        const Vec3 speed = stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity;
        // Nothing changed: the same body.
        stage.run(1);
        CHECK(stage.physics->bodyOf(crate) == before);

        // A larger collider is another shape, and so another body - that falls on.
        stage.world.tryGet<ColliderComponent>(crate)->size = Vec3(3.0f, 1.0f, 1.0f);
        stage.physics->synchronize(stage.world);
        const BodyId larger = stage.physics->bodyOf(crate);
        CHECK(larger != before);
        CHECK_FALSE(stage.physics->simulation().isValid(before));
        CHECK(stage.physics->simulation().linearVelocity(larger).y < speed.y);
        CHECK(stage.physics->simulation().statistics().bodies == 1);
        CHECK(stage.under(1.4f, 0.0f) == "Crate");

        // So is a heavier one, a slipperier one, one of another kind, a scaled one.
        stage.world.tryGet<RigidBodyComponent>(crate)->mass = 40.0f;
        stage.physics->synchronize(stage.world);
        const BodyId heavier = stage.physics->bodyOf(crate);
        CHECK(heavier != larger);
        stage.world.tryGet<ColliderComponent>(crate)->friction = 0.1f;
        stage.physics->synchronize(stage.world);
        const BodyId slipperier = stage.physics->bodyOf(crate);
        CHECK(slipperier != heavier);
        stage.world.transform(crate).scale = Vec3(2.0f, 1.0f, 1.0f);
        stage.physics->synchronize(stage.world);
        CHECK(stage.physics->bodyOf(crate) != slipperier);
        CHECK(stage.under(2.9f, 0.0f) == "Crate");
        CHECK(stage.under(3.1f, 0.0f) == "(nothing)");

        // Made static, it stops; made dynamic again, it falls from rest.
        stage.world.tryGet<RigidBodyComponent>(crate)->motion = BodyMotion::Static;
        stage.run(5);
        const float height = stage.world.transform(crate).position.y;
        stage.run(5);
        CHECK(stage.world.transform(crate).position.y == height);
        CHECK(stage.physics->simulation().statistics().bodies == 1);
    }

    TEST_CASE("a body that moves and turns stays the body it is") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        // A crate with a second collider hanging under it, off centre and turned -
        // under a group that is itself turned and scaled.
        const Entity group = stage.world.createEntity("Group");
        stage.world.transform(group) = {.position = Vec3(3.0f, 1.0f, -2.0f),
                                        .rotation = glm::angleAxis(0.7f, glm::normalize(Vec3(1.0f, 2.0f, 3.0f))),
                                        .scale = Vec3(1.5f, 0.75f, 1.25f)};
        const Entity crate = stage.addCrate(Vec3(0.3f, 0.2f, 0.1f));
        REQUIRE(stage.world.setParent(crate, group, false).has_value());
        const Entity handle = stage.world.createEntity("Handle");
        stage.world.transform(handle) = {.position = Vec3(0.0f, 0.8f, 0.2f),
                                         .rotation = glm::angleAxis(0.4f, Vec3(0.0f, 0.0f, 1.0f)),
                                         .scale = Vec3(0.5f)};
        stage.world.add<ColliderComponent>(handle) = {.shape = ShapeType::Capsule, .radius = 0.2f, .height = 1.0f};
        REQUIRE(stage.world.setParent(handle, crate, false).has_value());
        stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(1.0f, 2.0f, -0.5f);
        stage.world.tryGet<RigidBodyComponent>(crate)->angularVelocity = Vec3(3.0f, -2.0f, 1.0f);

        stage.run(1);
        const BodyId body = stage.physics->bodyOf(crate);
        REQUIRE(stage.physics->simulation().isValid(body));
        // Tumbling through the air for two seconds, it is the same body all the while:
        // what it is made of does not depend on where it is.
        for (int frame = 0; frame < 120; ++frame) {
            stage.run(1);
            REQUIRE(stage.physics->bodyOf(crate) == body);
        }
        CHECK(stage.physics->simulation().statistics().bodies == 1);
        CHECK(glm::length(stage.world.tryGet<RigidBodyComponent>(crate)->angularVelocity) > 1.0f);
    }

    TEST_CASE("a shape is as large as its entity is scaled") {
        Stage stage;
        const auto shaped = [&](const char* name, ShapeType shape, float x, const Vec3& scale) {
            const Entity entity = stage.world.createEntity(name);
            stage.world.transform(entity) = {.position = Vec3(x, 0.0f, 0.0f), .scale = scale};
            stage.world.add<ColliderComponent>(entity) = {
                .shape = shape, .size = Vec3(1.0f), .radius = 0.5f, .height = 2.0f};
            return entity;
        };
        // A box by each axis: three metres tall.
        shaped("Box", ShapeType::Box, 0.0f, Vec3(1.0f, 3.0f, 2.0f));
        // A ball by the largest of the three: a radius of a metre and a half.
        shaped("Ball", ShapeType::Sphere, 10.0f, Vec3(1.0f, 3.0f, 2.0f));
        // A capsule by Y in height and by the larger of X and Z across: six metres from
        // end to end, a metre thick.
        shaped("Capsule", ShapeType::Capsule, 20.0f, Vec3(1.0f, 3.0f, 2.0f));
        // Mirrored is as large as not.
        shaped("Mirrored", ShapeType::Box, 30.0f, Vec3(-2.0f, -1.0f, 1.0f));
        stage.physics->synchronize(stage.world);

        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(1.5f).epsilon(1.0e-4));
        CHECK(stage.under(0.0f, 0.9f) == "Box");
        CHECK(stage.under(0.0f, 1.1f) == "(nothing)");
        CHECK(stage.heightAt(10.0f, 0.0f) == doctest::Approx(1.5f).epsilon(1.0e-4));
        CHECK(stage.under(11.4f, 0.0f) == "Ball");
        CHECK(stage.heightAt(20.0f, 0.0f) == doctest::Approx(3.0f).epsilon(1.0e-4));
        CHECK(stage.under(20.9f, 0.0f) == "Capsule");
        CHECK(stage.under(21.1f, 0.0f) == "(nothing)");
        CHECK(stage.under(30.9f, 0.0f) == "Mirrored");
        CHECK(stage.heightAt(30.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1.0e-4));

        // Mirrored along Y, an entity is upside down: a collider a metre above its
        // middle is a metre below it in the world. Mirrored after its body was made, too.
        const Entity hanging = stage.world.createEntity("Hanging");
        stage.world.transform(hanging).position = Vec3(60.0f, 10.0f, 0.0f);
        stage.world.add<ColliderComponent>(hanging) = {.center = Vec3(0.0f, 1.0f, 0.0f)};
        stage.physics->synchronize(stage.world);
        CHECK(stage.heightAt(60.0f, 0.0f) == doctest::Approx(11.5f).epsilon(1.0e-4));
        stage.world.transform(hanging).scale = Vec3(1.0f, -1.0f, 1.0f);
        stage.physics->synchronize(stage.world);
        CHECK(stage.heightAt(60.0f, 0.0f) == doctest::Approx(9.5f).epsilon(1.0e-4));

        // The centre of a collider is in the space of its entity: scaled and turned with it.
        const Entity offset = stage.world.createEntity("Offset");
        stage.world.transform(offset) = {.position = Vec3(40.0f, 0.0f, 0.0f),
                                         .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f)),
                                         .scale = Vec3(2.0f)};
        stage.world.add<ColliderComponent>(offset) = {
            .shape = ShapeType::Sphere, .radius = 0.25f, .center = Vec3(1.0f, 0.0f, 0.0f)};
        stage.physics->synchronize(stage.world);
        // A metre along the entity's X, which points up and is two metres long.
        CHECK(stage.heightAt(40.0f, 0.0f) == doctest::Approx(2.5f).epsilon(1.0e-4));
    }

    TEST_CASE("a mirrored entity collides where it is, and stays mirrored while its body moves it") {
        // What an entity hangs under is scaled by the first, the entity itself by the
        // second: mirrored along each axis, along all three, above and below.
        struct Mirroring {
            Vec3 above;
            Vec3 own;
        };

        const std::vector<Mirroring> mirrorings = {
            {Vec3(1.0f), Vec3(-1.0f, 1.0f, 1.0f)},
            {Vec3(1.0f), Vec3(1.0f, -1.0f, 1.0f)},
            {Vec3(1.0f), Vec3(1.0f, 1.0f, -1.0f)},
            {Vec3(1.0f), Vec3(-1.0f, -1.0f, -1.0f)},
            {Vec3(1.0f), Vec3(-1.0f, -1.0f, 1.0f)},
            {Vec3(1.0f, -1.0f, 1.0f), Vec3(1.0f)},
            {Vec3(-1.0f, 1.0f, 1.0f), Vec3(1.0f, 1.0f, -1.0f)},
            {Vec3(2.0f, -2.0f, 2.0f), Vec3(-0.5f)},
        };
        for (size_t index = 0; index < mirrorings.size(); ++index) {
            CAPTURE(index);
            const Mirroring& mirroring = mirrorings[index];
            Stage stage;
            stage.physics->simulation().setGravity(Vec3(0.0f));
            const Entity group = stage.world.createEntity("Group");
            stage.world.transform(group) = {.position = Vec3(4.0f, 30.0f, -2.0f),
                                            .rotation = glm::angleAxis(0.5f, Vec3(0.0f, 0.0f, 1.0f)),
                                            .scale = mirroring.above};
            // A ball at the end of an arm: a shape that looks the same however it is
            // turned, in a place that tells every mirroring apart.
            const Vec3 end(1.0f, 2.0f, 3.0f);
            const Entity arm = stage.world.createEntity("Arm");
            stage.world.transform(arm) = {.position = Vec3(1.0f, 0.5f, 0.0f),
                                          .rotation = glm::angleAxis(0.3f, glm::normalize(Vec3(1.0f, 1.0f, 0.0f))),
                                          .scale = mirroring.own};
            stage.world.add<ColliderComponent>(arm) = {.shape = ShapeType::Sphere, .radius = 0.25f, .center = end};
            stage.world.add<RigidBodyComponent>(arm) = {.linearDamping = 0.0f,
                                                        .angularDamping = 0.0f,
                                                        .linearVelocity = Vec3(0.5f, 0.0f, -1.0f),
                                                        .angularVelocity = Vec3(1.0f, 2.0f, 0.5f)};
            REQUIRE(stage.world.setParent(arm, group, false).has_value());
            const float radius = 0.25f * std::abs(mirroring.above.x * mirroring.own.x);

            // Where the entity has the ball, a ray from above finds it.
            const auto ballIsWhereTheEntityHasIt = [&] {
                const Vec3 middle(stage.world.computeWorldMatrix(arm) * Vec4(end, 1.0f));
                const std::optional<RayHit> hit = stage.physics->simulation().raycast(
                    Ray{middle + Vec3(0.0f, 50.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
                return hit.has_value() && std::abs(hit->position.y - (middle.y + radius)) < 2.0e-3f;
            };
            stage.physics->synchronize(stage.world);
            REQUIRE(ballIsWhereTheEntityHasIt());

            // Tumbling for a second, it stays there - and the entity stays the way
            // round it was: its scale is its own, and the body turns it, no more.
            const bool mirrored = glm::determinant(Mat3(stage.world.computeWorldMatrix(arm))) < 0.0f;
            const BodyId body = stage.physics->bodyOf(arm);
            for (int frame = 0; frame < 60; ++frame) {
                stage.run(1);
                REQUIRE(ballIsWhereTheEntityHasIt());
            }
            CHECK(stage.physics->bodyOf(arm) == body);
            CHECK(stage.world.transform(arm).scale == mirroring.own);
            CHECK((glm::determinant(Mat3(stage.world.computeWorldMatrix(arm))) < 0.0f) == mirrored);
            CHECK(glm::distance(stage.physics->simulation().position(body), stage.positionOf(arm)) < 1.0e-3f);
            CHECK(glm::length(stage.world.tryGet<RigidBodyComponent>(arm)->angularVelocity) > 1.0f);
        }
    }

    TEST_CASE("what is switched off, removed or destroyed has no body any more") {
        Stage stage;
        stage.addFloor();
        const Entity group = stage.world.createEntity("Group");
        const Entity crate = stage.addCrate(Vec3(0.0f, 0.5f, 0.0f));
        const Entity other = stage.addCrate(Vec3(5.0f, 0.5f, 0.0f), "Other");
        REQUIRE(stage.world.setParent(crate, group, false).has_value());
        CHECK(stage.run(1).bodies == 3);

        // Switched off - itself, or what it hangs under - an entity has no body.
        stage.world.setEnabled(group, false);
        CHECK(stage.run(1).bodies == 2);
        CHECK(stage.physics->bodyOf(crate).isNull());
        CHECK(stage.under(0.0f, 0.0f) == "Floor");
        stage.world.setEnabled(group, true);
        CHECK(stage.run(1).bodies == 3);
        CHECK(stage.under(0.0f, 0.0f) == "Crate");

        // Without its collider a rigid body has no shape, and is no body; a collider
        // without a rigid body stands still.
        stage.world.remove<ColliderComponent>(other);
        CHECK(stage.run(1).bodies == 2);
        stage.world.add<ColliderComponent>(other);
        stage.world.remove<RigidBodyComponent>(other);
        CHECK(stage.run(1).bodies == 3);
        CHECK(stage.physics->simulation().motion(stage.physics->bodyOf(other)) == BodyMotion::Static);

        // An entity that is destroyed takes its body along; so does a world that is emptied.
        const BodyId body = stage.physics->bodyOf(crate);
        stage.world.destroyEntity(crate);
        CHECK(stage.run(1).bodies == 2);
        CHECK_FALSE(stage.physics->simulation().isValid(body));
        CHECK(stage.physics->entityOf(body).isNull());
        stage.world.clear();
        CHECK(stage.run(1) == PhysicsFrameStatistics{.steps = 1});

        // clear() removes the bodies at once; the next update makes them again.
        stage.addFloor();
        stage.addCrate(Vec3(0.0f, 0.5f, 0.0f));
        CHECK(stage.run(1).bodies == 2);
        stage.physics->clear();
        CHECK(stage.physics->simulation().statistics().bodies == 0);
        CHECK(stage.run(1).bodies == 2);
    }

    TEST_CASE("an entity whose body cannot be made is named once, and has none until it can") {
        Stage stage;
        const Entity flat = stage.world.createEntity("Flat thing");
        stage.world.transform(flat).scale = Vec3(1.0f, 0.0f, 1.0f);
        stage.world.add<ColliderComponent>(flat);
        stage.world.add<RigidBodyComponent>(flat).mass = 0.0f;
        const LogCapture log;
        CHECK(stage.run(3).bodies == 0);
        CHECK(stage.physics->bodyOf(flat).isNull());
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Flat thing' has no body: a dynamic body needs a mass of more than nothing, not 0"));

        // One thing put right, the next is found - and said, once.
        stage.world.tryGet<RigidBodyComponent>(flat)->mass = 1.0f;
        CHECK(stage.run(3).bodies == 0);
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("'Flat thing' has no body: a box of 1 by 0 by 1 metres has no volume to collide with"));
        // clear() forgets what was said, with everything else.
        stage.physics->clear();
        CHECK(stage.run(3).bodies == 0);
        CHECK(log.count(LogLevel::Error) == 3);

        stage.world.transform(flat).scale = Vec3(1.0f);
        CHECK(stage.run(1).bodies == 1);
        CHECK(log.count(LogLevel::Error) == 3);
        // What was put right and goes wrong again is said again.
        stage.world.transform(flat).scale = Vec3(1.0f, 0.0f, 1.0f);
        CHECK(stage.run(3).bodies == 0);
        CHECK(log.count(LogLevel::Error) == 4);
        // So is what was not asked for in between: switched off, the entity wants no body.
        stage.world.setEnabled(flat, false);
        stage.run(1);
        stage.world.setEnabled(flat, true);
        stage.run(3);
        CHECK(log.count(LogLevel::Error) == 5);
        // An entity that is gone is not remembered for it.
        stage.world.destroyEntity(flat);
        CHECK(stage.run(1).bodies == 0);
        CHECK(log.count(LogLevel::Error) == 5);
    }

    TEST_CASE("an entity that is nowhere leaves its body where it was, and is named once") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.transform(wall).position = Vec3(0.0f, 2.0f, 0.0f);
        stage.world.add<ColliderComponent>(wall);
        const Entity lift = stage.world.createEntity("Lift");
        stage.world.transform(lift).position = Vec3(5.0f, 2.0f, 0.0f);
        stage.world.add<ColliderComponent>(lift);
        stage.world.add<RigidBodyComponent>(lift).motion = BodyMotion::Kinematic;
        const Entity crate = stage.addCrate(Vec3(10.0f, 2.0f, 0.0f));
        stage.run(2);

        // A transform that is no number - a script that divided by nothing. Nothing
        // asserts, while time passes and while it does not; the bodies are where they were.
        const LogCapture log;
        for (const Entity entity : {wall, lift, crate}) {
            stage.world.transform(entity).position.x = kNotANumber;
        }
        stage.run(3);
        stage.physics->synchronize(stage.world);
        CHECK(stage.under(0.0f, 0.0f) == "Wall");
        CHECK(stage.under(5.0f, 0.0f) == "Lift");
        CHECK(stage.under(10.0f, 0.0f) == "Crate");
        CHECK(log.count(LogLevel::Error) == 3);
        CHECK(log.contains("'Lift' is nowhere - its place in the world is not made of numbers - and its body stays "
                           "where it was"));

        // Somewhere again, they are followed again.
        stage.world.transform(wall).position = Vec3(0.0f, 4.0f, 0.0f);
        stage.world.transform(lift).position = Vec3(5.0f, 6.0f, 0.0f);
        stage.world.transform(crate).position = Vec3(10.0f, 8.0f, 0.0f);
        stage.run(1);
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(4.5f).epsilon(1.0e-3));
        CHECK(stage.heightAt(5.0f, 0.0f) == doctest::Approx(6.5f).epsilon(1.0e-3));
        CHECK(stage.heightAt(10.0f, 0.0f) == doctest::Approx(8.5f).epsilon(1.0e-3));
        CHECK(log.count(LogLevel::Error) == 3);

        // A body that the simulation moves takes its entity out of nowhere with it.
        stage.world.transform(crate).position.y = kNotANumber;
        stage.world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(0.0f, -1.0f, 0.0f);
        stage.run(1);
        CHECK(stage.world.transform(crate).position.y < 8.0f);
        CHECK(stage.world.transform(crate).position.y > 7.9f);

        // What starts nowhere has no body until it is somewhere.
        const Entity lost = stage.addCrate(Vec3(kNotANumber, 0.0f, 0.0f), "Lost");
        CHECK(stage.run(2).bodies == 3);
        CHECK(stage.physics->bodyOf(lost).isNull());
        CHECK(log.contains("'Lost' has no body: "));
        stage.world.transform(lost).position = Vec3(20.0f, 0.0f, 0.0f);
        CHECK(stage.run(1).bodies == 4);
    }

    TEST_CASE("under something scaled to nothing an entity stays as it is while its body falls") {
        Stage stage;
        const Entity flat = stage.world.createEntity("Flat");
        stage.world.transform(flat).scale = Vec3(1.0f, 0.0f, 1.0f);
        const Entity ball = stage.world.createEntity("Ball");
        stage.world.transform(ball).position = Vec3(0.0f, 3.0f, 0.0f);
        stage.world.add<ColliderComponent>(ball) = {.shape = ShapeType::Sphere};
        stage.world.add<RigidBodyComponent>(ball);
        REQUIRE(stage.world.setParent(ball, flat, false).has_value());
        const Transform before = stage.world.transform(ball);

        // A ball is as large as the largest scale makes it, and so there is a body.
        // It falls; no place under its parent is where it is, and its entity is left
        // alone rather than given numbers that are none.
        CHECK(stage.run(30).bodies == 1);
        CHECK(stage.physics->simulation().position(stage.physics->bodyOf(ball)).y < -1.0f);
        CHECK(stage.world.transform(ball) == before);
    }

    TEST_CASE("two worlds given the same frames end up the same") {
        const auto run = [] {
            Stage stage;
            stage.addFloor();
            std::vector<Entity> crates;
            for (int index = 0; index < 5; ++index) {
                crates.push_back(stage.addCrate(
                    Vec3(0.25f * static_cast<float>(index % 2), 0.5f + 1.01f * static_cast<float>(index), 0.0f)));
            }
            const Entity ball = stage.world.createEntity("Ball");
            stage.world.transform(ball).position = Vec3(-8.0f, 2.0f, 0.1f);
            stage.world.add<ColliderComponent>(ball) = {.shape = ShapeType::Sphere, .radius = 0.4f};
            stage.world.add<RigidBodyComponent>(ball) = {.mass = 10.0f, .linearVelocity = Vec3(15.0f, 0.0f, 0.0f)};
            crates.push_back(ball);
            // Frames of uneven length, as a game has them.
            for (int frame = 0; frame < 200; ++frame) {
                stage.physics->update(stage.world, frame % 3 == 0 ? 0.021f : 0.013f);
            }
            std::vector<Transform> places;
            for (const Entity crate : crates) {
                places.push_back(stage.world.transform(crate));
            }
            return places;
        };
        const std::vector<Transform> first = run();
        const std::vector<Transform> second = run();
        CHECK(first[4].position.y < 4.0f);
        CHECK(first == second);
    }
}

TEST_SUITE("PhysicsComponents") {
    TEST_CASE("the physics components are registered under the names scene files use, and described") {
        ComponentRegistry registry;
        REQUIRE(registerPhysicsComponents(registry).has_value());
        REQUIRE(registry.components().size() == 2);
        for (const char* name : {"Collider", "RigidBody"}) {
            const ComponentDescriptor* descriptor = registry.find(name);
            REQUIRE(descriptor != nullptr);
            CHECK(descriptor->traits.optional);
            CHECK_FALSE(descriptor->traits.description.empty());
            for (const PropertyInfo& property : descriptor->type.properties()) {
                CAPTURE(property.name());
                CHECK_FALSE(property.attributes().description.empty());
            }
        }
        CHECK_FALSE(registerPhysicsComponents(registry).has_value());
        const TypeInfo& rigidBody = registry.find("RigidBody")->type;
        CHECK(rigidBody.findProperty("mass")->attributes().unit == "kg");
        CHECK(rigidBody.findProperty("linearVelocity")->attributes().transient);
        CHECK(rigidBody.findProperty("angularVelocity")->attributes().transient);
        CHECK_FALSE(rigidBody.findProperty("mass")->attributes().transient);
    }

    TEST_CASE("the physics components are saved with every property but what only a running simulation has") {
        ComponentRegistry registry;
        REQUIRE(registerPhysicsComponents(registry).has_value());
        const TypeInfo& colliderType = registry.find("Collider")->type;
        const TypeInfo& rigidBodyType = registry.find("RigidBody")->type;

        const ColliderComponent collider{.shape = ShapeType::Capsule,
                                         .size = Vec3(1.0f, 2.0f, 3.0f),
                                         .radius = 0.25f,
                                         .height = 1.5f,
                                         .center = Vec3(0.0f, 0.75f, 0.0f),
                                         .friction = 0.25f,
                                         .restitution = 0.5f};
        const Json savedCollider = colliderType.toJson(&collider);
        CHECK(savedCollider == unwrap(parseJson(R"({"shape": "capsule", "size": [1.0, 2.0, 3.0], "radius": 0.25,
                                                   "height": 1.5, "center": [0.0, 0.75, 0.0], "friction": 0.25,
                                                   "restitution": 0.5})")));
        ColliderComponent loadedCollider;
        REQUIRE(colliderType.fromJson(&loadedCollider, savedCollider).has_value());
        CHECK(loadedCollider.shape == ShapeType::Capsule);
        CHECK(loadedCollider.size == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(loadedCollider.center == Vec3(0.0f, 0.75f, 0.0f));
        CHECK(loadedCollider.restitution == 0.5f);

        const RigidBodyComponent rigidBody{.motion = BodyMotion::Kinematic,
                                           .mass = 12.5f,
                                           .linearDamping = 0.25f,
                                           .angularDamping = 0.5f,
                                           .gravityScale = -1.0f,
                                           .continuous = true,
                                           .allowSleep = false,
                                           .linearVelocity = Vec3(1.0f, 2.0f, 3.0f),
                                           .angularVelocity = Vec3(4.0f, 5.0f, 6.0f)};
        const Json savedRigidBody = rigidBodyType.toJson(&rigidBody);
        // The speeds are what a body has while it is simulated: not part of a scene.
        CHECK(savedRigidBody == unwrap(parseJson(R"({"motion": "kinematic", "mass": 12.5, "linearDamping": 0.25,
                                                    "angularDamping": 0.5, "gravityScale": -1.0,
                                                    "continuous": true, "allowSleep": false})")));
        RigidBodyComponent loadedRigidBody;
        REQUIRE(rigidBodyType.fromJson(&loadedRigidBody, savedRigidBody).has_value());
        CHECK(loadedRigidBody.motion == BodyMotion::Kinematic);
        CHECK(loadedRigidBody.mass == 12.5f);
        CHECK(loadedRigidBody.gravityScale == -1.0f);
        CHECK(loadedRigidBody.continuous);
        CHECK_FALSE(loadedRigidBody.allowSleep);
        CHECK(loadedRigidBody.linearVelocity == Vec3(0.0f));

        // An edit - by a person, a script or an agent - may set the speeds.
        const Result<PropertyPatch> edit =
            rigidBodyType.parseEdit(unwrap(parseJson(R"({"linearVelocity": [0, 5, 0]})")));
        REQUIRE(edit.has_value());
        edit->applyTo(&loadedRigidBody);
        CHECK(loadedRigidBody.linearVelocity == Vec3(0.0f, 5.0f, 0.0f));

        // What makes no sense is refused.
        for (const char* text :
             {R"({"shape": "cylinder"})", R"({"size": [1, 0, 1]})", R"({"radius": 0})", R"({"height": -1})",
              R"({"friction": -0.5})", R"({"restitution": 1.5})", R"({"trigger": true})"}) {
            CAPTURE(std::string(text));
            CHECK_FALSE(colliderType.fromJson(&loadedCollider, unwrap(parseJson(text))).has_value());
        }
        for (const char* text : {R"({"motion": "ghost"})", R"({"mass": 0})", R"({"mass": -3})",
                                 R"({"linearDamping": -1})", R"({"gravityScale": 1000})", R"({"continuous": 1})"}) {
            CAPTURE(std::string(text));
            CHECK_FALSE(rigidBodyType.fromJson(&loadedRigidBody, unwrap(parseJson(text))).has_value());
        }
    }
}
