#include "cordite/physics/PhysicsSystem.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Mesh.h"
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
#include <span>
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
    Stage() : assets(unwrap(AssetManager::create())), physics(unwrap(PhysicsSystem::create())) {}

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
            last = physics->update(world, *assets, kStep);
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
    /// The meshes the engine brings along, and no others.
    std::unique_ptr<AssetManager> assets;
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
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.5f * kStep).steps == 0);
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.5f * kStep).steps == 1);
        // A frame of a tenth of a second makes six.
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.1f).steps == 6);
        // A pause is not caught up with: eight steps to an update at the most.
        CHECK(stage.physics->update(stage.world, *stage.assets, 5.0f).steps == 8);
        CHECK(stage.physics->update(stage.world, *stage.assets, kStep).steps == 1);
        // Frames that are no time are no steps.
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.0f).steps == 0);
        CHECK(stage.physics->update(stage.world, *stage.assets, -1.0f).steps == 0);
        CHECK(stage.physics->update(stage.world, *stage.assets, kNotANumber).steps == 0);
        CHECK(stage.physics->simulation().statistics().steps == 16);

        // Other steps can be asked for; none at all cannot.
        const std::unique_ptr<PhysicsSystem> coarse =
            unwrap(PhysicsSystem::create({.stepSeconds = 0.1, .maxStepsPerUpdate = 2}));
        CHECK(coarse->update(stage.world, *stage.assets, 0.25f).steps == 2);
        CHECK(coarse->update(stage.world, *stage.assets, 10.0f).steps == 2);
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
        CHECK(stage.physics->synchronize(stage.world, *stage.assets) ==
              PhysicsFrameStatistics{.steps = 0, .bodies = 2, .activeBodies = 1});
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(5.5f).epsilon(1.0e-4));
        for (int index = 0; index < 30; ++index) {
            stage.physics->synchronize(stage.world, *stage.assets);
        }
        CHECK(stage.world.transform(crate).position == Vec3(0.0f, 5.0f, 0.0f));
        // An entity that is moved has its body there at once - dynamic, static or kinematic.
        stage.world.transform(crate).position = Vec3(3.0f, 2.0f, 0.0f);
        const Entity wall = stage.world.createEntity("Wall");
        stage.world.add<ColliderComponent>(wall);
        const Entity lift = stage.world.createEntity("Lift");
        stage.world.add<ColliderComponent>(lift);
        stage.world.add<RigidBodyComponent>(lift).motion = BodyMotion::Kinematic;
        stage.physics->synchronize(stage.world, *stage.assets);
        stage.world.transform(wall).position = Vec3(-6.0f, 4.0f, 0.0f);
        stage.world.transform(lift).position = Vec3(-9.0f, 7.0f, 0.0f);
        stage.physics->synchronize(stage.world, *stage.assets);
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
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.0f) ==
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
            stage.physics->update(stage.world, *stage.assets, frame % 3 == 2 ? 0.0f : kStep);
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
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.under(4.0f, 0.0f) == "Crate");
        CHECK(stage.under(4.0f, 5.0f) == "Wall");

        // What they hang under is moved and turned, as in an editor. Both bodies are
        // there - the dynamic one too, which stays where it is while time passes.
        const Transform before = stage.world.transform(crate);
        stage.world.transform(group) = {.position = Vec3(-10.0f, 1.0f, 0.0f),
                                        .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f))};
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.under(-10.0f, 0.0f) == "Crate");
        CHECK(stage.heightAt(-10.0f, 0.0f) == doctest::Approx(3.5f).epsilon(1.0e-4));
        // Five metres along the group's Z, which a quarter turn about Y has made the world's X.
        CHECK(stage.under(-5.0f, 0.0f) == "Wall");
        CHECK(stage.under(4.0f, 0.0f) == "(nothing)");
        // Nothing was written: the crate is what the scene says it is, to the last digit.
        CHECK(stage.world.transform(crate) == before);
        // Synchronized again, nothing is put anywhere: the body is the one it was.
        const BodyId body = stage.physics->bodyOf(crate);
        stage.physics->synchronize(stage.world, *stage.assets);
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
        stage.physics->synchronize(stage.world, *stage.assets);
        const BodyId body = stage.physics->bodyOf(wall);
        // A thousand frames, each of a movement too small to tell from rounding: five
        // thousandths of a millimetre, and a hundred-thousandth of a radian.
        for (int frame = 1; frame <= 1000; ++frame) {
            stage.world.transform(wall).position.x = 5.0e-6f * static_cast<float>(frame);
            stage.world.transform(wall).rotation =
                glm::angleAxis(1.0e-5f * static_cast<float>(frame), Vec3(0.0f, 1.0f, 0.0f));
            if (frame % 2 == 0) {
                stage.physics->synchronize(stage.world, *stage.assets);
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
        stage.physics->synchronize(stage.world, *stage.assets);
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
        CHECK(stage.physics->update(stage.world, *stage.assets, 3.0f * kStep).steps == 3);
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
            const uint32_t steps = stage.physics->update(stage.world, *stage.assets, 0.5f * kStep).steps;
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

        CHECK(stage.physics->synchronize(stage.world, *stage.assets).bodies == 2);
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
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).bodies == 4);
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
        stage.physics->synchronize(stage.world, *stage.assets);
        const BodyId larger = stage.physics->bodyOf(crate);
        CHECK(larger != before);
        CHECK_FALSE(stage.physics->simulation().isValid(before));
        CHECK(stage.physics->simulation().linearVelocity(larger).y < speed.y);
        CHECK(stage.physics->simulation().statistics().bodies == 1);
        CHECK(stage.under(1.4f, 0.0f) == "Crate");

        // So is a heavier one, a slipperier one, one of another kind, a scaled one.
        stage.world.tryGet<RigidBodyComponent>(crate)->mass = 40.0f;
        stage.physics->synchronize(stage.world, *stage.assets);
        const BodyId heavier = stage.physics->bodyOf(crate);
        CHECK(heavier != larger);
        stage.world.tryGet<ColliderComponent>(crate)->friction = 0.1f;
        stage.physics->synchronize(stage.world, *stage.assets);
        const BodyId slipperier = stage.physics->bodyOf(crate);
        CHECK(slipperier != heavier);
        stage.world.transform(crate).scale = Vec3(2.0f, 1.0f, 1.0f);
        stage.physics->synchronize(stage.world, *stage.assets);
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
        stage.physics->synchronize(stage.world, *stage.assets);

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
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.heightAt(60.0f, 0.0f) == doctest::Approx(11.5f).epsilon(1.0e-4));
        stage.world.transform(hanging).scale = Vec3(1.0f, -1.0f, 1.0f);
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.heightAt(60.0f, 0.0f) == doctest::Approx(9.5f).epsilon(1.0e-4));

        // The centre of a collider is in the space of its entity: scaled and turned with it.
        const Entity offset = stage.world.createEntity("Offset");
        stage.world.transform(offset) = {.position = Vec3(40.0f, 0.0f, 0.0f),
                                         .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f)),
                                         .scale = Vec3(2.0f)};
        stage.world.add<ColliderComponent>(offset) = {
            .shape = ShapeType::Sphere, .radius = 0.25f, .center = Vec3(1.0f, 0.0f, 0.0f)};
        stage.physics->synchronize(stage.world, *stage.assets);
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
            stage.physics->synchronize(stage.world, *stage.assets);
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
        stage.physics->synchronize(stage.world, *stage.assets);
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

    TEST_CASE("a collider is the hull around a mesh, or the triangles of one") {
        Stage stage;
        const auto shaped = [&](const char* name, ShapeType shape, BuiltinMesh mesh, float x) {
            const Entity entity = stage.world.createEntity(name);
            stage.world.transform(entity).position = Vec3(x, 0.0f, 0.0f);
            stage.world.add<ColliderComponent>(entity) = {.shape = shape, .mesh = builtinMeshId(mesh)};
            return entity;
        };
        // A cone of a metre, its tip at the top: half a metre across half way up.
        shaped("Cone", ShapeType::ConvexHull, BuiltinMesh::Cone, 0.0f);
        // A ring that lies flat: a quarter of a metre high, with a hole of half a metre.
        // Its triangles have the hole; the hull around them has not.
        shaped("Ring", ShapeType::Mesh, BuiltinMesh::Torus, 10.0f);
        shaped("Disc", ShapeType::ConvexHull, BuiltinMesh::Torus, 20.0f);
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).bodies == 3);

        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(stage.heightAt(0.25f, 0.0f) == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(stage.under(0.55f, 0.0f) == "(nothing)");
        CHECK(stage.under(10.0f, 0.0f) == "(nothing)");
        CHECK(stage.under(10.375f, 0.0f) == "Ring");
        CHECK(stage.heightAt(10.375f, 0.0f) == doctest::Approx(0.125f).epsilon(0.02));
        CHECK(stage.under(20.0f, 0.0f) == "Disc");
        CHECK(stage.heightAt(20.0f, 0.0f) == doctest::Approx(0.125f).epsilon(0.02));

        // What was made of a mesh serves every collider of that mesh and shape: three
        // more cones, of other sizes, and still three geometries.
        CHECK(stage.physics->simulation().statistics().geometries == 3);
        const Entity wide = shaped("Wide cone", ShapeType::ConvexHull, BuiltinMesh::Cone, 30.0f);
        stage.world.transform(wide).scale = Vec3(4.0f, 1.0f, 4.0f);
        // Upside down: mirrored along Y, its base is at the top.
        const Entity hanging = shaped("Hanging cone", ShapeType::ConvexHull, BuiltinMesh::Cone, 40.0f);
        stage.world.transform(hanging).scale = Vec3(1.0f, -1.0f, 1.0f);
        // And one whose middle is not where its entity is.
        const Entity raised = shaped("Raised cone", ShapeType::ConvexHull, BuiltinMesh::Cone, 50.0f);
        stage.world.tryGet<ColliderComponent>(raised)->center = Vec3(0.0f, 3.0f, 0.0f);
        CHECK(stage.physics->synchronize(stage.world, *stage.assets).bodies == 6);
        CHECK(stage.physics->simulation().statistics().geometries == 3);
        CHECK(stage.heightAt(31.0f, 0.0f) == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(stage.under(31.9f, 0.0f) == "Wide cone");
        CHECK(stage.heightAt(40.4f, 0.0f) == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(stage.heightAt(50.0f, 0.0f) == doctest::Approx(3.5f).epsilon(0.02));

        // The system says what it has made of a mesh, for whoever wants to show it.
        const GeometryId cone = stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Cone), ShapeType::ConvexHull);
        CHECK(stage.physics->simulation().isValid(cone));
        CHECK(stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Cone), ShapeType::Mesh).isNull());
        CHECK(stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Cube), ShapeType::ConvexHull).isNull());
        CHECK(stage.physics->geometryOf(Uuid(), ShapeType::ConvexHull).isNull());
    }

    TEST_CASE("a hull can be moved by the simulation, and the triangles of a mesh only by hand") {
        Stage stage;
        // A floor of the triangles of the plane, ten metres wide, and a cylinder that
        // falls on it and comes to rest: a hull has an inside.
        const Entity floor = stage.world.createEntity("Floor");
        stage.world.transform(floor).scale = Vec3(10.0f, 1.0f, 10.0f);
        stage.world.add<ColliderComponent>(floor) = {.shape = ShapeType::Mesh,
                                                     .mesh = builtinMeshId(BuiltinMesh::Plane)};
        const Entity drum = stage.world.createEntity("Drum");
        stage.world.transform(drum).position = Vec3(0.0f, 3.0f, 0.0f);
        stage.world.add<ColliderComponent>(drum) = {.shape = ShapeType::ConvexHull,
                                                    .mesh = builtinMeshId(BuiltinMesh::Cylinder)};
        stage.world.add<RigidBodyComponent>(drum).mass = 20.0f;
        CHECK(stage.run(240) == PhysicsFrameStatistics{.steps = 1, .bodies = 2, .activeBodies = 0, .geometries = 2});
        CHECK(stage.world.transform(drum).position.y == doctest::Approx(0.5f).epsilon(0.03));

        // A kinematic body of triangles is moved by its entity, and lifts the drum.
        stage.world.add<RigidBodyComponent>(floor).motion = BodyMotion::Kinematic;
        stage.run(1);
        for (int frame = 1; frame <= 60; ++frame) {
            stage.world.transform(floor).position.y = static_cast<float>(frame) / 60.0f;
            stage.run(1);
        }
        stage.run(30);
        CHECK(stage.world.transform(drum).position.y == doctest::Approx(1.5f).epsilon(0.03));

        // A dynamic one is none, and it is said why; the drum falls for want of a floor.
        const LogCapture log;
        stage.world.tryGet<RigidBodyComponent>(floor)->motion = BodyMotion::Dynamic;
        CHECK(stage.run(3).bodies == 1);
        CHECK(stage.physics->bodyOf(floor).isNull());
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("'Floor' has no body: a dynamic body cannot be made of the triangles of a mesh, which have "
                           "no inside: a convex hull has one"));
        CHECK(stage.world.transform(drum).position.y < 1.5f);
    }

    TEST_CASE("a collider of which no shape can be made is left out of its body, and named once") {
        Stage stage;
        const LogCapture log;
        // A table whose top is a box - and whose legs are to be the hull around a mesh
        // that is not named, and the triangles of one that there is not.
        const Entity table = stage.world.createEntity("Table");
        stage.world.transform(table).position = Vec3(0.0f, 1.0f, 0.0f);
        stage.world.add<ColliderComponent>(table) = {.size = Vec3(2.0f, 0.1f, 1.0f)};
        stage.world.add<RigidBodyComponent>(table).motion = BodyMotion::Static;
        const Entity unnamed = stage.world.createEntity("Unnamed leg");
        stage.world.add<ColliderComponent>(unnamed).shape = ShapeType::ConvexHull;
        REQUIRE(stage.world.setParent(unnamed, table, false).has_value());
        const Entity missing = stage.world.createEntity("Missing leg");
        stage.world.add<ColliderComponent>(missing) = {.shape = ShapeType::Mesh,
                                                       .mesh = *Uuid::parse("11111111-1111-4111-8111-111111111111")};
        REQUIRE(stage.world.setParent(missing, table, false).has_value());
        // The plane has no volume for a hull to be around.
        const Entity flat = stage.world.createEntity("Flat thing");
        stage.world.add<ColliderComponent>(flat) = {.shape = ShapeType::ConvexHull,
                                                    .mesh = builtinMeshId(BuiltinMesh::Plane)};

        // The table is a body of its top alone; the flat thing, left without a shape, is none.
        CHECK(stage.run(3).bodies == 1);
        CHECK(stage.heightAt(0.0f, 0.0f) == doctest::Approx(1.05f).epsilon(1.0e-3));
        CHECK(stage.physics->bodyOf(flat).isNull());
        CHECK(log.count(LogLevel::Error) == 3);
        CHECK(log.contains("'Unnamed leg' has a collider that is no shape: a collider of the shape convexHull needs a "
                           "mesh, and names none"));
        CHECK(log.contains("'Missing leg' has a collider that is no shape: "));
        CHECK(log.contains("11111111-1111-4111-8111-111111111111"));
        CHECK(log.contains("'Flat thing' has a collider that is no shape: the 4 points for a convex hull lie in one "
                           "plane: there is no volume between them"));

        // Given a mesh, a leg is part of the table - and nothing more is said.
        stage.world.transform(unnamed).position = Vec3(0.0f, -0.55f, 0.0f);
        stage.world.tryGet<ColliderComponent>(unnamed)->mesh = builtinMeshId(BuiltinMesh::Cube);
        CHECK(stage.run(2).bodies == 1);
        CHECK(stage.physics->bodyOf(unnamed).isNull());
        const std::optional<RayHit> leg =
            stage.physics->simulation().raycast(Ray{Vec3(5.0f, 0.3f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f)}, 100.0f);
        REQUIRE(leg.has_value());
        CHECK(stage.physics->entityOf(leg->body) == table);
        CHECK(leg->position.x == doctest::Approx(0.5f).epsilon(1.0e-3));
        CHECK(log.count(LogLevel::Error) == 3);

        // An entity whose collider is no shape and whose body cannot be made either is
        // told both, each once - not one after the other, frame for frame.
        stage.world.add<RigidBodyComponent>(missing).mass = 0.0f;
        const Entity weight = stage.world.createEntity("Weight");
        stage.world.add<ColliderComponent>(weight);
        REQUIRE(stage.world.setParent(weight, missing, false).has_value());
        stage.run(5);
        CHECK(log.count(LogLevel::Error) == 4);
        CHECK(log.contains("'Missing leg' has no body: a dynamic body needs a mass of more than nothing, not 0"));
    }

    TEST_CASE("what was made of meshes is made again when the assets have been refreshed") {
        Stage stage;
        const Entity rock = stage.addCrate(Vec3(0.0f, 50.0f, 0.0f), "Rock");
        *stage.world.tryGet<ColliderComponent>(rock) = {.shape = ShapeType::ConvexHull,
                                                        .mesh = builtinMeshId(BuiltinMesh::Sphere)};
        stage.run(30);
        const BodyId body = stage.physics->bodyOf(rock);
        const GeometryId geometry =
            stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Sphere), ShapeType::ConvexHull);
        const Vec3 speed = stage.world.tryGet<RigidBodyComponent>(rock)->linearVelocity;
        REQUIRE(stage.physics->simulation().isValid(geometry));

        // The files may be others now: the geometry is forgotten and prepared anew,
        // and the body that was made of it with it - which falls on as it fell.
        REQUIRE(stage.assets->refresh().has_value());
        stage.run(1);
        CHECK_FALSE(stage.physics->simulation().isValid(geometry));
        const GeometryId again = stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Sphere), ShapeType::ConvexHull);
        CHECK(stage.physics->simulation().isValid(again));
        CHECK(again != geometry);
        CHECK(stage.physics->simulation().statistics().geometries == 1);
        CHECK(stage.physics->bodyOf(rock) != body);
        CHECK(stage.world.tryGet<RigidBodyComponent>(rock)->linearVelocity.y < speed.y);
        // Without a refresh, nothing is made again.
        const BodyId remade = stage.physics->bodyOf(rock);
        stage.run(5);
        CHECK(stage.physics->bodyOf(rock) == remade);

        // clear() forgets the geometry with the bodies.
        stage.physics->clear();
        CHECK(stage.physics->simulation().statistics().geometries == 0);
        CHECK(stage.physics->geometryOf(builtinMeshId(BuiltinMesh::Sphere), ShapeType::ConvexHull).isNull());
        CHECK(stage.run(1).bodies == 1);
        CHECK(stage.physics->simulation().statistics().geometries == 1);
    }

    TEST_CASE("what begins to touch and what stops is said of the entities, once for each pair") {
        Stage stage;
        const Entity floor = stage.addFloor();
        const Entity crate = stage.addCrate(Vec3(0.0f, 3.0f, 0.0f));
        stage.world.tryGet<RigidBodyComponent>(crate)->linearDamping = 0.0f;
        // A lid on the crate that is part of its body: one body, and so one pair.
        const Entity lid = stage.world.createEntity("Lid");
        stage.world.transform(lid) = {.position = Vec3(0.0f, 0.55f, 0.0f), .scale = Vec3(1.2f, 0.1f, 1.2f)};
        stage.world.add<ColliderComponent>(lid);
        REQUIRE(stage.world.setParent(lid, crate, false).has_value());

        // Nothing while it falls; then it has landed, and that is said in the update it
        // happens in, and in no other.
        std::vector<CollisionEvent> heard;
        int landedAt = 0;
        for (int frame = 1; frame <= 60; ++frame) {
            stage.run(1);
            const std::span<const CollisionEvent> events = stage.physics->events();
            if (!events.empty()) {
                landedAt = frame;
            }
            heard.insert(heard.end(), events.begin(), events.end());
        }
        REQUIRE(heard.size() == 1);
        // Two and a half metres take seven tenths of a second: forty-three steps.
        CHECK(landedAt >= 42);
        CHECK(landedAt <= 44);
        CHECK(heard[0].type == CollisionEventType::CollisionBegan);
        CHECK(heard[0].first == floor);
        CHECK(heard[0].second == crate);
        CHECK_NEAR(heard[0].normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-2f);
        CHECK(heard[0].position.y == doctest::Approx(0.0f).epsilon(0.05));
        CHECK(heard[0].speed == doctest::Approx(7.0f).epsilon(0.03));

        // Asleep on the floor it touches on, and nothing is said - also not when no
        // time passes, which says nothing at all.
        stage.run(300);
        CHECK(stage.physics->events().empty());
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.physics->events().empty());

        // A frame of several steps says what happened in all of them: lifted off and
        // put down beside the floor, the crate has left it.
        stage.world.transform(crate).position = Vec3(100.0f, 0.5f, 0.0f);
        CHECK(stage.physics->update(stage.world, *stage.assets, 3.0f * kStep).steps == 3);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0] ==
              CollisionEvent{.type = CollisionEventType::CollisionEnded, .first = floor, .second = crate});

        // An entity that is destroyed stops touching what it touched, and is still named.
        stage.world.transform(crate).position = Vec3(0.0f, 0.5f, 0.0f);
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0].type == CollisionEventType::CollisionBegan);
        stage.world.destroyEntity(crate);
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0].type == CollisionEventType::CollisionEnded);
        CHECK(stage.physics->events()[0].second == crate);
        CHECK_FALSE(stage.world.isValid(stage.physics->events()[0].second));
        // clear() forgets what was said with everything else.
        stage.physics->clear();
        CHECK(stage.physics->events().empty());
    }

    TEST_CASE("a trigger stops nothing, and says what moves into it and out of it") {
        Stage stage;
        const Entity floor = stage.addFloor();
        // A doorway two metres up that a crate falls through.
        const Entity doorway = stage.world.createEntity("Doorway");
        stage.world.transform(doorway).position = Vec3(0.0f, 3.0f, 0.0f);
        stage.world.add<ColliderComponent>(doorway) = {.size = Vec3(3.0f, 1.0f, 3.0f), .trigger = true};
        const Entity crate = stage.addCrate(Vec3(0.0f, 6.0f, 0.0f));
        stage.world.tryGet<RigidBodyComponent>(crate)->linearDamping = 0.0f;
        stage.physics->synchronize(stage.world, *stage.assets);

        // A trigger is a body of its own: one that is moved by hand, and notices.
        const BodyId trigger = stage.physics->triggerOf(doorway);
        REQUIRE(stage.physics->simulation().isValid(trigger));
        CHECK(stage.physics->simulation().isSensor(trigger));
        CHECK(stage.physics->simulation().motion(trigger) == BodyMotion::Kinematic);
        CHECK(stage.physics->entityOf(trigger) == doorway);
        CHECK(stage.physics->bodyOf(doorway).isNull());
        CHECK(stage.physics->triggerOf(crate).isNull());
        // Nothing that looks for bodies finds it.
        CHECK(stage.under(1.2f, 1.2f) == "Floor");

        // The crate falls through it to the floor: in, out, and down - no slower for it.
        std::vector<CollisionEvent> heard;
        for (int frame = 0; frame < 120; ++frame) {
            stage.run(1);
            const std::span<const CollisionEvent> events = stage.physics->events();
            heard.insert(heard.end(), events.begin(), events.end());
        }
        REQUIRE(heard.size() == 3);
        CHECK(heard[0].type == CollisionEventType::TriggerEntered);
        CHECK(heard[0].first == doorway);
        CHECK(heard[0].second == crate);
        CHECK(heard[1] == CollisionEvent{.type = CollisionEventType::TriggerLeft, .first = doorway, .second = crate});
        CHECK(heard[2].type == CollisionEventType::CollisionBegan);
        CHECK(heard[2].first == floor);
        CHECK(heard[2].second == crate);
        CHECK(heard[2].speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 5.5f)).epsilon(0.03));

        // A trigger goes where its entity goes: brought down around the crate, which
        // has fallen asleep, it has the crate in it - and the floor, which stands
        // still, is not something that enters anything.
        stage.run(300);
        stage.world.transform(doorway).position = Vec3(0.0f, 0.5f, 0.0f);
        stage.run(2);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0].type == CollisionEventType::TriggerEntered);
        CHECK(stage.physics->events()[0].second == crate);
        CHECK(stage.physics->triggerOf(doorway) == trigger);
        // While the crate sleeps in it, it is in it.
        stage.run(120);
        CHECK(stage.physics->events().empty());
        // Switched off, a trigger has nothing in it any more.
        stage.world.setEnabled(doorway, false);
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0] ==
              CollisionEvent{.type = CollisionEventType::TriggerLeft, .first = doorway, .second = crate});
        CHECK(stage.physics->triggerOf(doorway).isNull());
    }

    TEST_CASE("a trigger is a body of its own wherever it is, and what hangs beside it is not part of it") {
        Stage stage;
        stage.physics->simulation().setGravity(Vec3(0.0f));
        // A wagon that the simulation moves: its own collider, a trigger of its own on
        // the same entity, a trigger ahead of it on an entity below, and a bumper below
        // that trigger which is a part of the wagon like any collider under it.
        const Entity wagon = stage.addCrate(Vec3(0.0f, 0.0f, 0.0f), "Wagon");
        *stage.world.tryGet<RigidBodyComponent>(wagon) = {.linearDamping = 0.0f,
                                                          .linearVelocity = Vec3(2.0f, 0.0f, 0.0f)};
        const Entity feeler = stage.world.createEntity("Feeler");
        stage.world.transform(feeler).position = Vec3(3.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(feeler) = {.shape = ShapeType::Sphere, .radius = 1.0f, .trigger = true};
        REQUIRE(stage.world.setParent(feeler, wagon, false).has_value());
        const Entity bumper = stage.world.createEntity("Bumper");
        stage.world.transform(bumper).position = Vec3(-2.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(bumper) = {.size = Vec3(0.2f, 1.0f, 1.0f)};
        REQUIRE(stage.world.setParent(bumper, feeler, false).has_value());
        // Something in its way, six metres ahead, that is moved by hand.
        const Entity gate = stage.world.createEntity("Gate");
        stage.world.transform(gate).position = Vec3(8.0f, 0.0f, 0.0f);
        stage.world.add<ColliderComponent>(gate);
        stage.world.add<RigidBodyComponent>(gate).motion = BodyMotion::Kinematic;

        // Three bodies: the wagon with its bumper, the feeler, the gate.
        CHECK(stage.run(1).bodies == 3);
        CHECK(stage.physics->bodyOf(feeler).isNull());
        CHECK(stage.physics->bodyOf(bumper).isNull());
        CHECK(stage.physics->simulation().isSensor(stage.physics->triggerOf(feeler)));
        // The bumper is where it hangs - a metre ahead of the wagon - and is the wagon's.
        const std::optional<RayHit> hit =
            stage.physics->simulation().raycast(Ray{Vec3(1.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(stage.physics->entityOf(hit->body) == wagon);

        // The feeler goes ahead of the wagon and reaches the gate before the wagon
        // does: that is said of the feeler's entity, and nothing of the wagon's - whose
        // own body the feeler does not notice either, though it hangs in it.
        std::vector<CollisionEvent> heard;
        for (int frame = 0; frame < 150 && heard.empty(); ++frame) {
            stage.run(1);
            const std::span<const CollisionEvent> events = stage.physics->events();
            heard.assign(events.begin(), events.end());
        }
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].type == CollisionEventType::TriggerEntered);
        CHECK(heard[0].first == feeler);
        CHECK(heard[0].second == gate);
        // The middle of the feeler was three and a half metres short of the gate then,
        // give or take a step: the wagon half a metre behind that.
        CHECK(stage.positionOf(wagon).x == doctest::Approx(3.5f).epsilon(0.03));

        // A trigger on the entity of a rigid body is a body beside that one.
        stage.world.add<ColliderComponent>(gate).trigger = true;
        CHECK(stage.run(1).bodies == 3);
        CHECK(stage.physics->bodyOf(gate).isNull());
        const Entity bar = stage.world.createEntity("Bar");
        stage.world.add<ColliderComponent>(bar) = {.size = Vec3(0.2f, 3.0f, 0.2f)};
        REQUIRE(stage.world.setParent(bar, gate, false).has_value());
        CHECK(stage.run(1).bodies == 4);
        CHECK(stage.physics->simulation().isValid(stage.physics->bodyOf(gate)));
        CHECK(stage.physics->simulation().isValid(stage.physics->triggerOf(gate)));
        CHECK(stage.physics->bodyOf(gate) != stage.physics->triggerOf(gate));
        CHECK(stage.physics->entityOf(stage.physics->bodyOf(gate)) == gate);
        CHECK(stage.physics->entityOf(stage.physics->triggerOf(gate)) == gate);
        // The speeds of a rigid body are those of its body, not of its trigger.
        stage.world.transform(gate).position.x += 1.0f;
        stage.run(1);
        CHECK(stage.world.tryGet<RigidBodyComponent>(gate)->linearVelocity.x == doctest::Approx(60.0f).epsilon(1.0e-3));
    }

    TEST_CASE("a body that is made anew does not stop and begin to touch what it still touches") {
        Stage stage;
        const Entity floor = stage.addFloor();
        const Entity crate = stage.addCrate(Vec3(0.0f, 0.5f, 0.0f));
        const Entity zone = stage.world.createEntity("Zone");
        stage.world.transform(zone).position = Vec3(0.0f, 0.5f, 0.0f);
        stage.world.add<ColliderComponent>(zone) = {.size = Vec3(4.0f), .trigger = true};
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 2);
        CHECK(stage.physics->events()[0].type == CollisionEventType::CollisionBegan);
        CHECK(stage.physics->events()[1].type == CollisionEventType::TriggerEntered);
        stage.run(10);

        // Heavier, it is another body to the simulation - and the same crate to
        // everybody else: on the floor and in the zone as it was.
        const BodyId before = stage.physics->bodyOf(crate);
        stage.world.tryGet<RigidBodyComponent>(crate)->mass = 50.0f;
        stage.run(1);
        CHECK(stage.physics->bodyOf(crate) != before);
        CHECK(stage.physics->events().empty());
        // Also when it is made anew in a frame without a step, or while no time passes,
        // and the step that tells of it comes later.
        stage.world.tryGet<RigidBodyComponent>(crate)->mass = 60.0f;
        CHECK(stage.physics->update(stage.world, *stage.assets, 0.0f).steps == 0);
        CHECK(stage.physics->events().empty());
        stage.world.tryGet<ColliderComponent>(crate)->friction = 0.9f;
        stage.physics->synchronize(stage.world, *stage.assets);
        CHECK(stage.physics->bodyOf(crate) != before);
        stage.run(1);
        CHECK(stage.physics->events().empty());
        stage.run(1);
        CHECK(stage.physics->events().empty());
        // So is a trigger that is given another size and still has the crate in it.
        stage.world.tryGet<ColliderComponent>(zone)->size = Vec3(3.0f);
        stage.run(1);
        CHECK(stage.physics->events().empty());

        // Made so small that the crate is no longer in it, the crate has left it.
        stage.world.tryGet<ColliderComponent>(zone)->size = Vec3(0.2f);
        stage.world.transform(zone).position = Vec3(0.0f, 5.0f, 0.0f);
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 1);
        CHECK(stage.physics->events()[0] ==
              CollisionEvent{.type = CollisionEventType::TriggerLeft, .first = zone, .second = crate});
        // And what a remade body newly touches is said like anything else: a crate
        // made wide enough to reach a post beside it.
        const Entity post = stage.world.createEntity("Post");
        stage.world.transform(post).position = Vec3(1.5f, 1.0f, 0.0f);
        stage.world.add<ColliderComponent>(post) = {.size = Vec3(0.2f, 2.0f, 0.2f)};
        stage.run(2);
        CHECK(stage.physics->events().empty());
        stage.world.tryGet<ColliderComponent>(crate)->size = Vec3(2.9f, 1.0f, 1.0f);
        stage.run(1);
        REQUIRE(stage.physics->events().size() == 1);
        const CollisionEvent reached = stage.physics->events()[0];
        CHECK(reached.type == CollisionEventType::CollisionBegan);
        // Which of two bodies that collide is named first follows from the places they
        // have in the simulation, which a body that is made anew does not keep.
        CHECK(
            ((reached.first == crate && reached.second == post) || (reached.first == post && reached.second == crate)));
        CHECK(stage.world.isValid(floor));
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
                stage.physics->update(stage.world, *stage.assets, frame % 3 == 0 ? 0.021f : 0.013f);
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
    TEST_CASE("colliders that no shape or body can be made of are found without making any") {
        World world;
        // Nothing wrong: boxes, a hull that names its mesh, triangles that stand still
        // and triangles that are moved by hand.
        const Entity crate = world.createEntity("Crate");
        world.add<ColliderComponent>(crate);
        world.add<RigidBodyComponent>(crate);
        const Entity rock = world.createEntity("Rock");
        world.add<ColliderComponent>(rock) = {.shape = ShapeType::ConvexHull,
                                              .mesh = builtinMeshId(BuiltinMesh::Sphere)};
        world.add<RigidBodyComponent>(rock);
        const Entity ground = world.createEntity("Ground");
        world.add<ColliderComponent>(ground) = {.shape = ShapeType::Mesh, .mesh = builtinMeshId(BuiltinMesh::Plane)};
        const Entity lift = world.createEntity("Lift");
        world.add<ColliderComponent>(lift) = {.shape = ShapeType::Mesh, .mesh = builtinMeshId(BuiltinMesh::Plane)};
        world.add<RigidBodyComponent>(lift).motion = BodyMotion::Kinematic;
        // A mesh that is named but not there is another check's to find.
        const Entity ghost = world.createEntity("Ghost");
        world.add<ColliderComponent>(ghost) = {.shape = ShapeType::ConvexHull,
                                               .mesh = *Uuid::parse("11111111-1111-4111-8111-111111111111")};
        CHECK(findColliderProblems(world).empty());

        // A shape of a mesh that names none.
        world.tryGet<ColliderComponent>(rock)->mesh = Uuid();
        // The triangles of a mesh in a dynamic body: by its own rigid body, and by one
        // above it - also where that is switched off, and also without a mesh.
        world.tryGet<RigidBodyComponent>(lift)->motion = BodyMotion::Dynamic;
        const Entity wagon = world.createEntity("Wagon");
        world.add<RigidBodyComponent>(wagon);
        world.setEnabled(wagon, false);
        const Entity axle = world.createEntity("Axle");
        REQUIRE(world.setParent(axle, wagon, false).has_value());
        const Entity wheel = world.createEntity("Wheel");
        world.add<ColliderComponent>(wheel).shape = ShapeType::Mesh;
        REQUIRE(world.setParent(wheel, axle, false).has_value());
        // A trigger of the triangles of a mesh is a body of its own that follows its
        // entity: nothing wrong with it under a body that the simulation moves.
        const Entity feeler = world.createEntity("Feeler");
        world.add<ColliderComponent>(feeler) = {
            .shape = ShapeType::Mesh, .mesh = builtinMeshId(BuiltinMesh::Plane), .trigger = true};
        REQUIRE(world.setParent(feeler, wagon, false).has_value());
        // Under a rigid body that stands still, a dynamic one further up does not count.
        const Entity anchor = world.createEntity("Anchor");
        world.add<RigidBodyComponent>(anchor).motion = BodyMotion::Static;
        REQUIRE(world.setParent(anchor, wagon, false).has_value());
        const Entity plate = world.createEntity("Plate");
        world.add<ColliderComponent>(plate) = {.shape = ShapeType::Mesh, .mesh = builtinMeshId(BuiltinMesh::Plane)};
        REQUIRE(world.setParent(plate, anchor, false).has_value());

        const std::vector<ColliderProblem> problems = findColliderProblems(world);
        REQUIRE(problems.size() == 4);
        CHECK(problems[0].entity == rock);
        CHECK(problems[0].message ==
              "Collider.mesh names no mesh, and a collider of the shape convexHull is made of one");
        CHECK(problems[1].entity == lift);
        CHECK(problems[1].message ==
              "Collider.shape is mesh, and the body it is part of - 'Lift' - is dynamic: the triangles of a mesh "
              "have no inside, and only a body that the simulation does not move is made of them");
        CHECK(problems[2].entity == wheel);
        CHECK(problems[2].message == "Collider.mesh names no mesh, and a collider of the shape mesh is made of one");
        CHECK(problems[3].entity == wheel);
        CHECK(problems[3].message.find("the body it is part of - 'Wagon' - is dynamic") != std::string::npos);
    }

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
                                         .mesh = *Uuid::parse("0a56c2f1-21e0-819d-8bac-567d1e1c4345"),
                                         .trigger = true,
                                         .center = Vec3(0.0f, 0.75f, 0.0f),
                                         .friction = 0.25f,
                                         .restitution = 0.5f};
        const Json savedCollider = colliderType.toJson(&collider);
        CHECK(savedCollider ==
              unwrap(parseJson(R"({"shape": "capsule", "size": [1.0, 2.0, 3.0], "radius": 0.25, "height": 1.5,
                                  "mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345", "trigger": true,
                                  "center": [0.0, 0.75, 0.0], "friction": 0.25, "restitution": 0.5})")));
        ColliderComponent loadedCollider;
        REQUIRE(colliderType.fromJson(&loadedCollider, savedCollider).has_value());
        CHECK(loadedCollider.shape == ShapeType::Capsule);
        CHECK(loadedCollider.trigger);
        CHECK(loadedCollider.mesh == collider.mesh);
        // A collider that names no mesh says so, and the shapes made of meshes have
        // the names files use.
        CHECK(colliderType.toJson(&loadedCollider)["mesh"].is_string());
        CHECK(colliderType.toJson(std::make_unique<ColliderComponent>().get())["mesh"].is_null());
        for (const char* shape : {"convexHull", "mesh"}) {
            Json named = savedCollider;
            named["shape"] = shape;
            REQUIRE(colliderType.fromJson(&loadedCollider, named).has_value());
            CHECK(nameOf(loadedCollider.shape) == shape);
        }
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
              R"({"friction": -0.5})", R"({"restitution": 1.5})", R"({"trigger": 1})", R"({"solid": true})"}) {
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
