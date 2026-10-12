#include "cordite/physics/PhysicsWorld.h"

#include "cordite/core/Hash.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();
/// The step every test takes: a sixtieth of a second.
constexpr float kStep = 1.0f / 60.0f;

void stepFor(PhysicsWorld& physics, int steps) {
    for (int step = 0; step < steps; ++step) {
        physics.step(kStep);
    }
}

/// A floor: a slab whose top is the plane y = 0.
BodyId addFloor(PhysicsWorld& physics) {
    return unwrap(physics.createBody({.motion = BodyMotion::Static,
                                      .shape = {.type = ShapeType::Box, .size = Vec3(50.0f, 1.0f, 50.0f)},
                                      .position = Vec3(0.0f, -0.5f, 0.0f)}));
}

/// A body that nothing slows down but what it hits.
BodyDesc undamped(const ShapeDesc& shape, const Vec3& position) {
    return {.shape = shape, .position = position, .linearDamping = 0.0f, .angularDamping = 0.0f};
}

} // namespace

TEST_SUITE("PhysicsWorld") {
    TEST_CASE("the names of the kinds of motion and of the shapes") {
        CHECK(nameOf(BodyMotion::Static) == "static");
        CHECK(nameOf(BodyMotion::Kinematic) == "kinematic");
        CHECK(nameOf(BodyMotion::Dynamic) == "dynamic");
        CHECK(nameOf(ShapeType::Box) == "box");
        CHECK(nameOf(ShapeType::Sphere) == "sphere");
        CHECK(nameOf(ShapeType::Capsule) == "capsule");
        CHECK(kBodyMotions.size() == 3);
        CHECK(kShapeTypes.size() == 3);
    }

    TEST_CASE("a new world is empty, and pulls down as hard as the earth does") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        CHECK(physics->statistics() == PhysicsStatistics());
        CHECK(physics->gravity() == Vec3(0.0f, -9.81f, 0.0f));
        CHECK_FALSE(physics->isValid(BodyId()));
        CHECK(BodyId().isNull());
        // Nothing to simulate is nothing to stop a step.
        physics->step(kStep);
        CHECK(physics->statistics().steps == 1);
    }

    TEST_CASE("a world is made with room for something and a gravity made of numbers") {
        CHECK_FALSE(PhysicsWorld::create({.maxBodies = 0}).has_value());
        CHECK_FALSE(PhysicsWorld::create({.maxBodyPairs = 0}).has_value());
        CHECK_FALSE(PhysicsWorld::create({.maxContactConstraints = 0}).has_value());
        const auto refused = PhysicsWorld::create({.gravity = Vec3(0.0f, kNotANumber, 0.0f)});
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        // Several worlds at once, and one after the last is gone, are no trouble.
        {
            const std::unique_ptr<PhysicsWorld> first = unwrap(PhysicsWorld::create());
            const std::unique_ptr<PhysicsWorld> second = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
            CHECK(second->gravity() == Vec3(0.0f));
        }
        CHECK(PhysicsWorld::create().has_value());
    }

    TEST_CASE("a body falls as fast as gravity makes it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId ball = unwrap(
            physics->createBody(undamped({.type = ShapeType::Sphere, .radius = 0.5f}, Vec3(1.0f, 100.0f, 2.0f))));
        CHECK(physics->isValid(ball));
        CHECK(physics->motion(ball) == BodyMotion::Dynamic);
        CHECK(physics->position(ball) == Vec3(1.0f, 100.0f, 2.0f));
        CHECK(physics->rotation(ball) == kIdentityQuat);
        CHECK(physics->linearVelocity(ball) == Vec3(0.0f));
        CHECK(physics->isActive(ball));

        // After a second of sixty steps it falls at 9.81 metres a second. Each step it
        // first gains speed and then moves at it, so it has fallen what the speeds of
        // the sixty steps add up to: 9.81 / 3600 * (1 + 2 + ... + 60) metres.
        stepFor(*physics, 60);
        CHECK(physics->linearVelocity(ball).y == doctest::Approx(-9.81f).epsilon(1.0e-4));
        const float fallen = 9.81f / 3600.0f * (60.0f * 61.0f / 2.0f);
        CHECK(physics->position(ball).y == doctest::Approx(100.0f - fallen).epsilon(1.0e-5));
        CHECK(physics->position(ball).x == 1.0f);
        CHECK(physics->position(ball).z == 2.0f);
        CHECK(physics->statistics() == PhysicsStatistics{.bodies = 1, .activeBodies = 1, .steps = 60});

        // Gravity can be changed, and scaled for one body: half as much is half the gain.
        physics->setGravity(Vec3(0.0f, -20.0f, 0.0f));
        BodyDesc light = undamped({.type = ShapeType::Sphere, .radius = 0.5f}, Vec3(10.0f, 100.0f, 0.0f));
        light.gravityScale = 0.5f;
        const BodyId feather = unwrap(physics->createBody(light));
        physics->step(kStep);
        CHECK(physics->linearVelocity(feather).y == doctest::Approx(-10.0f * kStep).epsilon(1.0e-4));
        CHECK(physics->linearVelocity(ball).y == doctest::Approx(-9.81f - 20.0f * kStep).epsilon(1.0e-4));
    }

    TEST_CASE("what falls on the floor comes to rest on it, and falls asleep") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        CHECK(physics->motion(floor) == BodyMotion::Static);
        CHECK_FALSE(physics->isActive(floor));

        const BodyId box = unwrap(physics->createBody(
            {.shape = {.type = ShapeType::Box, .size = Vec3(1.0f, 2.0f, 1.0f)}, .position = Vec3(0.0f, 3.0f, 0.0f)}));
        const BodyId ball = unwrap(physics->createBody(
            {.shape = {.type = ShapeType::Sphere, .radius = 0.25f}, .position = Vec3(3.0f, 2.0f, 0.0f)}));
        const BodyId capsule =
            unwrap(physics->createBody({.shape = {.type = ShapeType::Capsule, .radius = 0.3f, .height = 1.8f},
                                        .position = Vec3(-3.0f, 2.0f, 0.0f)}));
        stepFor(*physics, 240);

        // Each rests with its lowest point on the floor: half its height above it.
        CHECK(physics->position(box).y == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(physics->position(ball).y == doctest::Approx(0.25f).epsilon(0.05));
        CHECK(physics->position(capsule).y == doctest::Approx(0.9f).epsilon(0.02));
        // None has wandered, and the floor is where it was.
        CHECK(std::abs(physics->position(box).x) < 0.01f);
        CHECK(std::abs(physics->position(ball).x - 3.0f) < 0.01f);
        CHECK(physics->position(floor) == Vec3(0.0f, -0.5f, 0.0f));
        // At rest for a while, a body sleeps: it is not simulated until something wakes it.
        CHECK_FALSE(physics->isActive(box));
        CHECK_FALSE(physics->isActive(ball));
        CHECK(physics->statistics().activeBodies == 0);
        CHECK(physics->statistics().bodies == 4);

        physics->activate(box);
        CHECK(physics->isActive(box));
        // A push wakes too, and moves: a kilogram given one newton second goes a metre a second.
        physics->addImpulse(ball, Vec3(0.0f, 0.0f, 1.0f));
        CHECK(physics->isActive(ball));
        CHECK(physics->linearVelocity(ball).z == doctest::Approx(1.0f));
    }

    TEST_CASE("a body that may not sleep stays awake at rest") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId box = unwrap(physics->createBody(
            {.shape = {.type = ShapeType::Box}, .position = Vec3(0.0f, 0.6f, 0.0f), .allowSleep = false}));
        stepFor(*physics, 240);
        CHECK(physics->position(box).y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(physics->isActive(box));
    }

    TEST_CASE("a body bounces as much as it is made to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        BodyDesc floor{.motion = BodyMotion::Static,
                       .shape = {.type = ShapeType::Box, .size = Vec3(50.0f, 1.0f, 50.0f)},
                       .position = Vec3(0.0f, -0.5f, 0.0f)};
        floor.restitution = 0.0f;
        REQUIRE(physics->createBody(floor).has_value());
        // Of two bodies that touch, the larger restitution counts: one ball keeps all of
        // its speed, the other none.
        BodyDesc lively = undamped({.type = ShapeType::Sphere, .radius = 0.5f}, Vec3(-2.0f, 3.0f, 0.0f));
        lively.restitution = 1.0f;
        BodyDesc dull = undamped({.type = ShapeType::Sphere, .radius = 0.5f}, Vec3(2.0f, 3.0f, 0.0f));
        const BodyId bouncing = unwrap(physics->createBody(lively));
        const BodyId stopping = unwrap(physics->createBody(dull));

        // Both reach the floor after about 0.7 seconds. Followed for another second and
        // a half, the one comes back up nearly to where it was let go, the other stays.
        float highestAfterTheFloor = 0.0f;
        for (int step = 0; step < 150; ++step) {
            physics->step(kStep);
            if (step > 60) {
                highestAfterTheFloor = std::max(highestAfterTheFloor, physics->position(bouncing).y);
            }
        }
        CHECK(highestAfterTheFloor > 2.5f);
        CHECK(highestAfterTheFloor < 3.1f);
        CHECK(physics->position(stopping).y == doctest::Approx(0.5f).epsilon(0.05));
    }

    TEST_CASE("friction decides whether a body slides down a slope") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A slope of twenty degrees; a box holds on to it when its friction is more than
        // the tangent of that, 0.36, and slides when it is less.
        const Quat tilt = glm::angleAxis(toRadians(20.0f), Vec3(0.0f, 0.0f, 1.0f));
        BodyDesc slope{.motion = BodyMotion::Static,
                       .shape = {.type = ShapeType::Box, .size = Vec3(40.0f, 1.0f, 40.0f)},
                       .rotation = tilt};
        slope.friction = 1.0f;
        REQUIRE(physics->createBody(slope).has_value());
        const auto boxOnSlope = [&](float z, float friction) {
            // Lying on the slope: half a metre of slab and half a metre of box above its middle.
            BodyDesc box{.shape = {.type = ShapeType::Box}, .position = tilt * Vec3(0.0f, 1.01f, z), .rotation = tilt};
            box.friction = friction;
            return unwrap(physics->createBody(box));
        };
        // Of two bodies that touch, the geometric mean of their frictions counts.
        const BodyId holding = boxOnSlope(-3.0f, 0.8f);
        const BodyId sliding = boxOnSlope(3.0f, 0.01f);
        const Vec3 holdingFrom = physics->position(holding);
        const Vec3 slidingFrom = physics->position(sliding);
        stepFor(*physics, 120);
        CHECK(glm::distance(physics->position(holding), holdingFrom) < 0.05f);
        CHECK(glm::distance(physics->position(sliding), slidingFrom) > 2.0f);
        // Downhill is towards -X for a slope that rises with X.
        CHECK(physics->position(sliding).x < slidingFrom.x - 1.5f);
    }

    TEST_CASE("forces, impulses and torques move a dynamic body, and nothing else") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        BodyDesc heavy = undamped({.type = ShapeType::Box}, Vec3(0.0f));
        heavy.mass = 4.0f;
        const BodyId box = unwrap(physics->createBody(heavy));

        // Twelve newtons on four kilograms for a sixtieth of a second: three metres per
        // second squared, for one step, and then forgotten.
        physics->addForce(box, Vec3(12.0f, 0.0f, 0.0f));
        physics->step(kStep);
        CHECK(physics->linearVelocity(box).x == doctest::Approx(3.0f * kStep).epsilon(1.0e-4));
        physics->step(kStep);
        CHECK(physics->linearVelocity(box).x == doctest::Approx(3.0f * kStep).epsilon(1.0e-4));

        // Eight newton seconds on four kilograms: two metres a second, at once.
        physics->addImpulse(box, Vec3(0.0f, 8.0f, 0.0f));
        CHECK(physics->linearVelocity(box).y == doctest::Approx(2.0f));

        // A torque sets it spinning about the axis it is about.
        physics->addTorque(box, Vec3(0.0f, 0.0f, 5.0f));
        physics->step(kStep);
        CHECK(physics->angularVelocity(box).z > 0.0f);
        CHECK(physics->angularVelocity(box).x == doctest::Approx(0.0f));

        // Speeds can be set outright.
        physics->setLinearVelocity(box, Vec3(0.0f, 0.0f, -7.0f));
        physics->setAngularVelocity(box, Vec3(0.0f, 1.5f, 0.0f));
        CHECK(physics->linearVelocity(box) == Vec3(0.0f, 0.0f, -7.0f));
        CHECK(physics->angularVelocity(box) == Vec3(0.0f, 1.5f, 0.0f));
        const Vec3 before = physics->position(box);
        physics->step(kStep);
        CHECK(physics->position(box).z == doctest::Approx(before.z - 7.0f * kStep).epsilon(1.0e-4));

        // A static body is moved by none of it, and a kinematic one only by its own speed.
        const BodyId wall = unwrap(physics->createBody(
            {.motion = BodyMotion::Static, .shape = {.type = ShapeType::Box}, .position = Vec3(20.0f, 0.0f, 0.0f)}));
        const BodyId lift = unwrap(physics->createBody(
            {.motion = BodyMotion::Kinematic, .shape = {.type = ShapeType::Box}, .position = Vec3(40.0f, 0.0f, 0.0f)}));
        for (const BodyId body : {wall, lift}) {
            physics->addForce(body, Vec3(100.0f));
            physics->addImpulse(body, Vec3(100.0f));
            physics->addTorque(body, Vec3(100.0f));
        }
        physics->setLinearVelocity(wall, Vec3(1.0f));
        physics->setAngularVelocity(wall, Vec3(1.0f));
        physics->setLinearVelocity(lift, Vec3(0.0f, 6.0f, 0.0f));
        stepFor(*physics, 60);
        CHECK(physics->position(wall) == Vec3(20.0f, 0.0f, 0.0f));
        CHECK(physics->linearVelocity(wall) == Vec3(0.0f));
        CHECK_NEAR(physics->position(lift), Vec3(40.0f, 6.0f, 0.0f), 1.0e-3f);
    }

    TEST_CASE("damping takes speed away by itself") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        BodyDesc desc{.shape = {.type = ShapeType::Sphere}};
        desc.linearDamping = 0.5f;
        desc.angularDamping = 0.5f;
        const BodyId slowed = unwrap(physics->createBody(desc));
        const BodyId free = unwrap(physics->createBody(undamped({.type = ShapeType::Sphere}, Vec3(10.0f, 0.0f, 0.0f))));
        for (const BodyId body : {slowed, free}) {
            physics->setLinearVelocity(body, Vec3(0.0f, 0.0f, 4.0f));
            physics->setAngularVelocity(body, Vec3(0.0f, 2.0f, 0.0f));
        }
        stepFor(*physics, 60);
        // Half of it a second, compounded sixty times: 4 * (1 - 0.5 / 60) ^ 60.
        CHECK(physics->linearVelocity(slowed).z ==
              doctest::Approx(4.0f * std::pow(1.0f - 0.5f * kStep, 60.0f)).epsilon(1.0e-3));
        CHECK(physics->angularVelocity(slowed).y < 1.3f);
        CHECK(physics->linearVelocity(free).z == doctest::Approx(4.0f));
        CHECK(physics->angularVelocity(free).y == doctest::Approx(2.0f));
    }

    TEST_CASE("a body is put somewhere else at once, and a kinematic one moved there through what is in its way") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        const BodyId box = unwrap(physics->createBody(undamped({.type = ShapeType::Box}, Vec3(0.0f))));
        physics->setLinearVelocity(box, Vec3(1.0f, 0.0f, 0.0f));
        const Quat turned = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f));
        // Of any length: it is brought to unit length.
        physics->setPlacement(box, Vec3(5.0f, 6.0f, 7.0f), turned * 3.0f);
        CHECK(physics->position(box) == Vec3(5.0f, 6.0f, 7.0f));
        const Quat rotation = physics->rotation(box);
        CHECK_NEAR(Vec4(rotation.x, rotation.y, rotation.z, rotation.w), Vec4(turned.x, turned.y, turned.z, turned.w),
                   1.0e-6f);
        // It keeps its speed.
        CHECK(physics->linearVelocity(box) == Vec3(1.0f, 0.0f, 0.0f));

        // A platform that moves a metre in a step pushes what lies before it.
        const BodyId platform =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                        .shape = {.type = ShapeType::Box, .size = Vec3(1.0f, 4.0f, 4.0f)},
                                        .position = Vec3(-20.0f, 0.0f, 0.0f)}));
        const BodyId crate = unwrap(physics->createBody(undamped({.type = ShapeType::Box}, Vec3(-18.5f, 0.0f, 0.0f))));
        for (int step = 1; step <= 30; ++step) {
            physics->moveKinematic(platform, Vec3(-20.0f + 0.1f * static_cast<float>(step), 0.0f, 0.0f), kIdentityQuat,
                                   kStep);
            physics->step(kStep);
        }
        CHECK_NEAR(physics->position(platform), Vec3(-17.0f, 0.0f, 0.0f), 1.0e-3f);
        CHECK(physics->linearVelocity(platform).x == doctest::Approx(6.0f).epsilon(1.0e-3));
        // The crate is ahead of the platform's front face, and moving away from it.
        CHECK(physics->position(crate).x > -16.1f);
        CHECK(physics->linearVelocity(crate).x > 5.0f);

        // Moving a body that is not kinematic that way does nothing.
        const BodyId bystander =
            unwrap(physics->createBody(undamped({.type = ShapeType::Box}, Vec3(0.0f, 50.0f, 0.0f))));
        physics->moveKinematic(bystander, Vec3(0.0f), kIdentityQuat, kStep);
        physics->step(kStep);
        CHECK(physics->position(bystander) == Vec3(0.0f, 50.0f, 0.0f));
        CHECK(physics->linearVelocity(bystander) == Vec3(0.0f));
    }

    TEST_CASE("a small, fast body goes through a thin wall unless it is watched between steps") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        REQUIRE(physics
                    ->createBody({.motion = BodyMotion::Static,
                                  .shape = {.type = ShapeType::Box, .size = Vec3(0.1f, 10.0f, 10.0f)},
                                  .position = Vec3(5.0f, 0.0f, 0.0f)})
                    .has_value());
        // Two hundred metres a second: more than three metres a step.
        BodyDesc bullet = undamped({.type = ShapeType::Sphere, .radius = 0.05f}, Vec3(0.0f, 1.0f, 0.0f));
        const BodyId unwatched = unwrap(physics->createBody(bullet));
        bullet.position = Vec3(0.0f, -1.0f, 0.0f);
        bullet.continuous = true;
        const BodyId watched = unwrap(physics->createBody(bullet));
        physics->setLinearVelocity(unwatched, Vec3(200.0f, 0.0f, 0.0f));
        physics->setLinearVelocity(watched, Vec3(200.0f, 0.0f, 0.0f));
        stepFor(*physics, 6);
        CHECK(physics->position(unwatched).x > 10.0f);
        CHECK(physics->position(watched).x < 5.0f);
    }

    TEST_CASE("a ray hits the first thing in its way") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        BodyDesc crateDesc{.motion = BodyMotion::Static,
                           .shape = {.type = ShapeType::Box, .size = Vec3(2.0f, 2.0f, 2.0f)},
                           .position = Vec3(0.0f, 1.0f, 0.0f)};
        crateDesc.userData = 0x1234'5678'9abc'def0ull;
        const BodyId crate = unwrap(physics->createBody(crateDesc));
        const BodyId ball = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                        .shape = {.type = ShapeType::Sphere, .radius = 1.0f},
                                                        .position = Vec3(10.0f, 1.0f, 0.0f)}));
        CHECK(physics->userData(crate) == 0x1234'5678'9abc'def0ull);
        CHECK(physics->userData(ball) == 0);

        // Straight down on the crate: its top is two metres up.
        std::optional<RayHit> hit = physics->raycast(Ray{Vec3(0.2f, 10.0f, -0.3f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == crate);
        CHECK(hit->userData == 0x1234'5678'9abc'def0ull);
        CHECK(hit->distance == doctest::Approx(8.0f).epsilon(1.0e-4));
        CHECK_NEAR(hit->position, Vec3(0.2f, 2.0f, -0.3f), 1.0e-3f);
        CHECK_NEAR(hit->normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);

        // From the side, at the ball: nine metres from the crate's face... but the crate
        // is not in the way of a ray that starts beyond it.
        hit = physics->raycast(Ray{Vec3(4.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == ball);
        CHECK(hit->distance == doctest::Approx(5.0f).epsilon(1.0e-4));
        CHECK_NEAR(hit->normal, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-3f);
        // The direction need not be of unit length; distances are in metres all the same.
        hit = physics->raycast(Ray{Vec3(4.0f, 1.0f, 0.0f), Vec3(25.0f, 0.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(5.0f).epsilon(1.0e-4));

        // Beside everything, the floor; too short to reach it, nothing.
        hit = physics->raycast(Ray{Vec3(5.0f, 3.0f, 5.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(3.0f).epsilon(1.0e-4));
        CHECK_FALSE(physics->raycast(Ray{Vec3(5.0f, 3.0f, 5.0f), Vec3(0.0f, -1.0f, 0.0f)}, 2.9f).has_value());
        // Upwards there is nothing at all.
        CHECK_FALSE(physics->raycast(Ray{Vec3(5.0f, 3.0f, 5.0f), Vec3(0.0f, 1.0f, 0.0f)}, 1000.0f).has_value());

        // Rays that are none hit nothing.
        CHECK_FALSE(physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f)}, 100.0f).has_value());
        CHECK_FALSE(physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 0.0f).has_value());
        CHECK_FALSE(physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, -5.0f).has_value());
        CHECK_FALSE(physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, kNotANumber).has_value());
        CHECK_FALSE(physics->raycast(Ray{Vec3(kNotANumber), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f).has_value());
        CHECK_FALSE(physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(kNotANumber)}, 100.0f).has_value());

        // A body that is gone is not hit any more: the floor behind it is.
        physics->destroyBody(crate);
        hit = physics->raycast(Ray{Vec3(0.2f, 10.0f, -0.3f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(10.0f).epsilon(1.0e-4));
    }

    TEST_CASE("a body that is destroyed is gone, and its id is no body's again") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.maxBodies = 4}));
        const BodyId first = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        CHECK(physics->statistics().bodies == 1);
        physics->destroyBody(first);
        CHECK_FALSE(physics->isValid(first));
        CHECK(physics->statistics().bodies == 0);
        // Destroying it again, or what never was a body, is ignored.
        physics->destroyBody(first);
        physics->destroyBody(BodyId());
        physics->destroyBody(BodyId{12345});

        // The next body takes its place, under an id of its own.
        const BodyId second = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        CHECK(second != first);
        CHECK(physics->isValid(second));
        CHECK_FALSE(physics->isValid(first));
    }

    TEST_CASE("a world holds as many bodies as it was made for") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.maxBodies = 3}));
        std::vector<BodyId> bodies;
        for (int index = 0; index < 3; ++index) {
            bodies.push_back(
                unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere},
                                            .position = Vec3(static_cast<float>(index) * 3.0f, 0.0f, 0.0f)})));
        }
        const Result<BodyId> oneTooMany = physics->createBody({.shape = {.type = ShapeType::Sphere}});
        REQUIRE_FALSE(oneTooMany.has_value());
        CHECK(oneTooMany.error().code == ErrorCode::OutOfRange);
        CHECK(oneTooMany.error().message == "the physics world is full: it holds 3 bodies");
        CHECK(physics->statistics().bodies == 3);
        // Room is given back with a body.
        physics->destroyBody(bodies[1]);
        CHECK(physics->createBody({.shape = {.type = ShapeType::Sphere}}).has_value());
    }

    TEST_CASE("bodies that make no sense are refused, and say why") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const auto refused = [&](const BodyDesc& desc) {
            const Result<BodyId> body = physics->createBody(desc);
            REQUIRE_FALSE(body.has_value());
            CHECK(body.error().code == ErrorCode::InvalidArgument);
            return body.error().message;
        };
        // Shapes without volume, and shapes that are not made of numbers.
        CHECK(refused({.shape = {.type = ShapeType::Box, .size = Vec3(1.0f, 0.0f, 1.0f)}}) ==
              "a box of 1 by 0 by 1 metres has no volume to collide with");
        refused({.shape = {.type = ShapeType::Box, .size = Vec3(1.0f, -2.0f, 1.0f)}});
        refused({.shape = {.type = ShapeType::Box, .size = Vec3(kNotANumber)}});
        CHECK(refused({.shape = {.type = ShapeType::Sphere, .radius = 0.0f}}) ==
              "a sphere of radius 0 has no volume to collide with");
        refused({.shape = {.type = ShapeType::Sphere, .radius = -1.0f}});
        refused({.shape = {.type = ShapeType::Sphere, .radius = kNotANumber}});
        CHECK(refused({.shape = {.type = ShapeType::Capsule, .radius = 0.5f, .height = 0.0f}}) ==
              "a capsule of radius 0.5 and height 0 has no volume to collide with");
        refused({.shape = {.type = ShapeType::Capsule, .radius = 0.0f, .height = 2.0f}});
        refused({.shape = {.type = ShapeType::Capsule, .radius = 0.5f, .height = kNotANumber}});

        // Placements that are none.
        refused({.position = Vec3(0.0f, kNotANumber, 0.0f)});
        refused({.rotation = Quat(0.0f, 0.0f, 0.0f, 0.0f)});
        refused({.rotation = Quat(kNotANumber, 0.0f, 0.0f, 0.0f)});
        // A dynamic body without mass - a static one needs none.
        CHECK(refused({.mass = 0.0f}) == "a dynamic body needs a mass of more than nothing, not 0");
        refused({.mass = -1.0f});
        refused({.mass = kNotANumber});
        CHECK(physics->createBody({.motion = BodyMotion::Static, .mass = 0.0f}).has_value());
        CHECK(physics->createBody({.motion = BodyMotion::Kinematic, .mass = 0.0f}).has_value());
        // The rest.
        BodyDesc desc;
        desc.friction = -0.1f;
        refused(desc);
        desc = {};
        desc.restitution = kNotANumber;
        refused(desc);
        desc = {};
        desc.linearDamping = -1.0f;
        refused(desc);
        desc = {};
        desc.angularDamping = kNotANumber;
        refused(desc);
        desc = {};
        desc.gravityScale = kNotANumber;
        refused(desc);
        // Nothing of all that was added.
        CHECK(physics->statistics().bodies == 2);
    }

    TEST_CASE("a capsule that is no taller than it is wide is a ball, and small shapes are still shapes") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId stubby =
            unwrap(physics->createBody({.shape = {.type = ShapeType::Capsule, .radius = 0.5f, .height = 0.6f},
                                        .position = Vec3(0.0f, 2.0f, 0.0f)}));
        // A box thinner than its edges would be rounded by: a centimetre.
        const BodyId sliver = unwrap(physics->createBody(
            {.shape = {.type = ShapeType::Box, .size = Vec3(1.0f, 0.01f, 1.0f)}, .position = Vec3(5.0f, 0.5f, 0.0f)}));
        stepFor(*physics, 240);
        CHECK(physics->position(stubby).y == doctest::Approx(0.5f).epsilon(0.03));
        // Lying on the floor, give or take what bodies may sink into each other.
        CHECK(physics->position(sliver).y > -0.03f);
        CHECK(physics->position(sliver).y < 0.03f);
    }

    TEST_CASE("a simulation is the same every time it is run") {
        const auto run = [] {
            const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
            addFloor(*physics);
            // A pile that topples: boxes put down off centre, a ball thrown into them.
            std::vector<BodyId> bodies;
            for (int level = 0; level < 6; ++level) {
                bodies.push_back(
                    unwrap(physics->createBody({.shape = {.type = ShapeType::Box, .size = Vec3(1.0f, 0.5f, 1.0f)},
                                                .position = Vec3(0.3f * static_cast<float>(level % 2),
                                                                 0.25f + 0.5f * static_cast<float>(level), 0.0f)})));
            }
            const BodyId ball = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere, .radius = 0.3f},
                                                            .position = Vec3(-6.0f, 1.5f, 0.1f),
                                                            .mass = 5.0f}));
            physics->setLinearVelocity(ball, Vec3(12.0f, 1.0f, 0.0f));
            bodies.push_back(ball);
            stepFor(*physics, 180);
            std::vector<Vec3> places;
            for (const BodyId body : bodies) {
                places.push_back(physics->position(body));
                places.push_back(Vec3(physics->rotation(body).x, physics->rotation(body).y, physics->rotation(body).z));
            }
            return places;
        };
        const std::vector<Vec3> first = run();
        const std::vector<Vec3> second = run();
        // Something happened ...
        CHECK(first[0] != Vec3(0.0f, 0.25f, 0.0f));
        // ... and it happened the same way twice, to the last bit.
        CHECK(first == second);

        // And the same way with every compiler, in every configuration and on every
        // machine: the bits of where everything ended up, as one number. It changes
        // when the library behind the physics does, or how it is built - which is a
        // decision, not an accident.
        const uint64_t fingerprint = hashBytes(std::as_bytes(std::span(first)));
        CHECK(fingerprint == 0xe08c4226f70037adull);
    }

    TEST_CASE("a step that is no time is no step") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId ball = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        physics->step(0.0f);
        physics->step(-1.0f);
        physics->step(kNotANumber);
        physics->step(std::numeric_limits<float>::infinity());
        CHECK(physics->position(ball) == Vec3(0.0f));
        CHECK(physics->statistics().steps == 0);
    }

    TEST_CASE("a step that needs more room than the world has is reported, once") {
        // Room for a single contact, and a row of balls all lying on a floor.
        const std::unique_ptr<PhysicsWorld> physics =
            unwrap(PhysicsWorld::create({.maxBodyPairs = 1, .maxContactConstraints = 1}));
        addFloor(*physics);
        for (int index = 0; index < 8; ++index) {
            REQUIRE(physics
                        ->createBody({.shape = {.type = ShapeType::Sphere, .radius = 0.5f},
                                      .position = Vec3(static_cast<float>(index) * 2.0f, 0.45f, 0.0f)})
                        .has_value());
        }
        const LogCapture log;
        stepFor(*physics, 3);
        CHECK(physics->statistics().overflowedSteps > 0);
        CHECK(physics->statistics().steps == 3);
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("some contacts were left out"));
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("asking about a body that is none is a mistake of the caller") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId gone = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        physics->destroyBody(gone);
        for (const BodyId body : {gone, BodyId()}) {
            CHECK_THROWS_AS((void)physics->position(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->rotation(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->motion(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->userData(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->linearVelocity(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->angularVelocity(body), test::AssertionFailure);
            CHECK_THROWS_AS((void)physics->isActive(body), test::AssertionFailure);
            CHECK_THROWS_AS(physics->activate(body), test::AssertionFailure);
            CHECK_THROWS_AS(physics->setPlacement(body, Vec3(0.0f), kIdentityQuat), test::AssertionFailure);
            CHECK_THROWS_AS(physics->moveKinematic(body, Vec3(0.0f), kIdentityQuat, kStep), test::AssertionFailure);
            CHECK_THROWS_AS(physics->setLinearVelocity(body, Vec3(0.0f)), test::AssertionFailure);
            CHECK_THROWS_AS(physics->setAngularVelocity(body, Vec3(0.0f)), test::AssertionFailure);
            CHECK_THROWS_AS(physics->addForce(body, Vec3(0.0f)), test::AssertionFailure);
            CHECK_THROWS_AS(physics->addImpulse(body, Vec3(0.0f)), test::AssertionFailure);
            CHECK_THROWS_AS(physics->addTorque(body, Vec3(0.0f)), test::AssertionFailure);
        }
        // So are numbers that are none, for a body that is one.
        const BodyId ball = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        CHECK_THROWS_AS(physics->setPlacement(ball, Vec3(kNotANumber), kIdentityQuat), test::AssertionFailure);
        CHECK_THROWS_AS(physics->setPlacement(ball, Vec3(0.0f), Quat(0.0f, 0.0f, 0.0f, 0.0f)), test::AssertionFailure);
        CHECK_THROWS_AS(physics->setLinearVelocity(ball, Vec3(kNotANumber)), test::AssertionFailure);
        CHECK_THROWS_AS(physics->addForce(ball, Vec3(kNotANumber)), test::AssertionFailure);
        CHECK_THROWS_AS(physics->setGravity(Vec3(kNotANumber)), test::AssertionFailure);
        CHECK(physics->position(ball) == Vec3(0.0f));
        CHECK(physics->gravity() == Vec3(0.0f, -9.81f, 0.0f));
    }
#else
    TEST_CASE("without assertions, a body that is none is at the origin and does not move") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId gone = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        physics->destroyBody(gone);
        for (const BodyId body : {gone, BodyId()}) {
            CHECK(physics->position(body) == Vec3(0.0f));
            CHECK(physics->rotation(body) == kIdentityQuat);
            CHECK(physics->motion(body) == BodyMotion::Static);
            CHECK(physics->userData(body) == 0);
            CHECK(physics->linearVelocity(body) == Vec3(0.0f));
            CHECK(physics->angularVelocity(body) == Vec3(0.0f));
            CHECK_FALSE(physics->isActive(body));
            physics->activate(body);
            physics->setPlacement(body, Vec3(1.0f), kIdentityQuat);
            physics->moveKinematic(body, Vec3(1.0f), kIdentityQuat, kStep);
            physics->setLinearVelocity(body, Vec3(1.0f));
            physics->setAngularVelocity(body, Vec3(1.0f));
            physics->addForce(body, Vec3(1.0f));
            physics->addImpulse(body, Vec3(1.0f));
            physics->addTorque(body, Vec3(1.0f));
        }
        // Numbers that are none are ignored for a body that is one.
        const BodyId ball = unwrap(physics->createBody({.shape = {.type = ShapeType::Sphere}}));
        physics->setPlacement(ball, Vec3(kNotANumber), kIdentityQuat);
        physics->setPlacement(ball, Vec3(0.0f), Quat(0.0f, 0.0f, 0.0f, 0.0f));
        physics->setLinearVelocity(ball, Vec3(kNotANumber));
        physics->addForce(ball, Vec3(kNotANumber));
        physics->setGravity(Vec3(kNotANumber));
        CHECK(physics->position(ball) == Vec3(0.0f));
        CHECK(physics->linearVelocity(ball) == Vec3(0.0f));
        CHECK(physics->gravity() == Vec3(0.0f, -9.81f, 0.0f));
        CHECK(physics->statistics().bodies == 1);
    }
#endif
}
