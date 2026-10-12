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
#include <string>
#include <tuple>
#include <utility>
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
                                      .shapes = {{.type = ShapeType::Box, .size = Vec3(50.0f, 1.0f, 50.0f)}},
                                      .position = Vec3(0.0f, -0.5f, 0.0f)}));
}

/// The corners of a box of `size` around the origin.
std::vector<Vec3> boxCorners(const Vec3& size) {
    std::vector<Vec3> corners;
    for (const float x : {-0.5f, 0.5f}) {
        for (const float y : {-0.5f, 0.5f}) {
            for (const float z : {-0.5f, 0.5f}) {
                corners.push_back(Vec3(x, y, z) * size);
            }
        }
    }
    return corners;
}

/// A square of two metres at the height of one that faces up: two triangles, counter-
/// clockwise seen from above.
const std::vector<Vec3> kSquarePositions = {Vec3(-1.0f, 1.0f, -1.0f), Vec3(-1.0f, 1.0f, 1.0f), Vec3(1.0f, 1.0f, 1.0f),
                                            Vec3(1.0f, 1.0f, -1.0f)};
const std::vector<uint32_t> kSquareIndices = {0, 1, 2, 0, 2, 3};

/// How high a ray straight down from high above (x, z) hits; far below for nothing.
constexpr float kNothingBelow = -1000.0f;

float heightAt(const PhysicsWorld& physics, float x, float z) {
    const std::optional<RayHit> hit = physics.raycast(Ray{Vec3(x, 100.0f, z), Vec3(0.0f, -1.0f, 0.0f)}, 500.0f);
    return hit ? hit->position.y : kNothingBelow;
}

/// The message a request was refused with, which must have been refused for what it
/// was given.
template <typename T>
std::string refusalOf(const Result<T>& result) {
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    return result.error().message;
}

/// Takes steps, and gathers what the bodies say of touching in each of them.
std::vector<ContactEvent> stepAndListen(PhysicsWorld& physics, int steps) {
    std::vector<ContactEvent> heard;
    for (int step = 0; step < steps; ++step) {
        physics.step(kStep);
        const std::span<const ContactEvent> events = physics.contactEvents();
        heard.insert(heard.end(), events.begin(), events.end());
    }
    return heard;
}

/// A body that nothing slows down but what it hits.
BodyDesc undamped(const ShapeDesc& shape, const Vec3& position) {
    return {.shapes = {shape}, .position = position, .linearDamping = 0.0f, .angularDamping = 0.0f};
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
        CHECK(nameOf(ShapeType::ConvexHull) == "convexHull");
        CHECK(nameOf(ShapeType::Mesh) == "mesh");
        CHECK(nameOf(JointType::Fixed) == "fixed");
        CHECK(nameOf(JointType::Ball) == "ball");
        CHECK(nameOf(JointType::Hinge) == "hinge");
        CHECK(nameOf(JointType::Slider) == "slider");
        CHECK(nameOf(JointType::Distance) == "distance");
        CHECK(kBodyMotions.size() == 3);
        CHECK(kShapeTypes.size() == 5);
        CHECK(kJointTypes.size() == 5);
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
        CHECK_FALSE(PhysicsWorld::create({.maxGeometryPoints = 0}).has_value());
        CHECK_FALSE(PhysicsWorld::create({.maxGeometryTriangles = 0}).has_value());
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

        const BodyId box =
            unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 2.0f, 1.0f)}},
                                        .position = Vec3(0.0f, 3.0f, 0.0f)}));
        const BodyId ball = unwrap(physics->createBody(
            {.shapes = {{.type = ShapeType::Sphere, .radius = 0.25f}}, .position = Vec3(3.0f, 2.0f, 0.0f)}));
        const BodyId capsule =
            unwrap(physics->createBody({.shapes = {{.type = ShapeType::Capsule, .radius = 0.3f, .height = 1.8f}},
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
            {.shapes = {{.type = ShapeType::Box}}, .position = Vec3(0.0f, 0.6f, 0.0f), .allowSleep = false}));
        stepFor(*physics, 240);
        CHECK(physics->position(box).y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(physics->isActive(box));
    }

    TEST_CASE("a body bounces as much as it is made to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        BodyDesc floor{.motion = BodyMotion::Static,
                       .shapes = {{.type = ShapeType::Box, .size = Vec3(50.0f, 1.0f, 50.0f)}},
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
                       .shapes = {{.type = ShapeType::Box, .size = Vec3(40.0f, 1.0f, 40.0f)}},
                       .rotation = tilt};
        slope.friction = 1.0f;
        REQUIRE(physics->createBody(slope).has_value());
        const auto boxOnSlope = [&](float z, float friction) {
            // Lying on the slope: half a metre of slab and half a metre of box above its middle.
            BodyDesc box{
                .shapes = {{.type = ShapeType::Box}}, .position = tilt * Vec3(0.0f, 1.01f, z), .rotation = tilt};
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
            {.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::Box}}, .position = Vec3(20.0f, 0.0f, 0.0f)}));
        const BodyId lift = unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                                        .shapes = {{.type = ShapeType::Box}},
                                                        .position = Vec3(40.0f, 0.0f, 0.0f)}));
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
        BodyDesc desc{.shapes = {{.type = ShapeType::Sphere}}};
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
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 4.0f, 4.0f)}},
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
                                  .shapes = {{.type = ShapeType::Box, .size = Vec3(0.1f, 10.0f, 10.0f)}},
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
                           .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, 2.0f, 2.0f)}},
                           .position = Vec3(0.0f, 1.0f, 0.0f)};
        crateDesc.userData = 0x1234'5678'9abc'def0ull;
        const BodyId crate = unwrap(physics->createBody(crateDesc));
        const BodyId ball = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                        .shapes = {{.type = ShapeType::Sphere, .radius = 1.0f}},
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

    TEST_CASE("a ball that is moved along a ray touches the first thing in its way") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        BodyDesc crateDesc{.motion = BodyMotion::Static,
                           .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, 2.0f, 2.0f)}},
                           .position = Vec3(0.0f, 1.0f, 0.0f)};
        crateDesc.userData = 77;
        const BodyId crate = unwrap(physics->createBody(crateDesc));
        const BodyId ball = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                        .shapes = {{.type = ShapeType::Sphere, .radius = 1.0f}},
                                                        .position = Vec3(10.0f, 1.0f, 0.0f)}));
        const Vec3 down(0.0f, -1.0f, 0.0f);

        // Straight down on the crate, whose top is two metres up: a ball of half a metre
        // touches it when its middle is half a metre above that.
        std::optional<RayHit> hit = physics->sphereCast(Ray{Vec3(0.2f, 10.0f, -0.3f), down}, 0.5f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == crate);
        CHECK(hit->userData == 77);
        CHECK(hit->distance == doctest::Approx(7.5f).epsilon(1.0e-3));
        CHECK_NEAR(hit->position, Vec3(0.2f, 2.0f, -0.3f), 1.0e-2f);
        CHECK_NEAR(hit->normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-3f);

        // Beside the crate a ray goes past it to the floor. The ball is wider: it
        // touches the edge of the crate, and is pushed off it sideways as much as up.
        const Ray beside{Vec3(1.3f, 10.0f, -0.3f), down};
        hit = physics->raycast(beside, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(10.0f).epsilon(1.0e-4));
        hit = physics->sphereCast(beside, 0.5f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == crate);
        // Three, four, five: three tenths beside the edge, the middle is four above it.
        // (The edges of a box are a little round, which is what the tolerance is for.)
        CHECK(hit->distance == doctest::Approx(7.6f).epsilon(5.0e-3));
        CHECK_NEAR(hit->position, Vec3(1.0f, 2.0f, -0.3f), 0.05f);
        CHECK_NEAR(hit->normal, Vec3(0.6f, 0.8f, 0.0f), 0.05f);
        // A thinner one goes past as the ray does.
        hit = physics->sphereCast(beside, 0.2f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(9.8f).epsilon(1.0e-3));

        // From the side at the ball: two balls touch when their middles are as far
        // apart as their radii together.
        hit = physics->sphereCast(Ray{Vec3(4.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)}, 0.5f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == ball);
        CHECK(hit->distance == doctest::Approx(4.5f).epsilon(1.0e-3));
        CHECK_NEAR(hit->position, Vec3(9.0f, 1.0f, 0.0f), 1.0e-2f);
        CHECK_NEAR(hit->normal, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-3f);
        // The direction need not be of unit length, and what is too far is not reached.
        hit = physics->sphereCast(Ray{Vec3(4.0f, 1.0f, 0.0f), Vec3(25.0f, 0.0f, 0.0f)}, 0.5f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(4.5f).epsilon(1.0e-3));
        CHECK_FALSE(physics->sphereCast(Ray{Vec3(4.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)}, 0.5f, 4.4f).has_value());
        CHECK_FALSE(
            physics->sphereCast(Ray{Vec3(4.0f, 5.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)}, 0.5f, 1000.0f).has_value());

        // A ball that starts in something touches it at once.
        hit = physics->sphereCast(Ray{Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)}, 0.25f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == crate);
        CHECK(hit->distance == 0.0f);

        // The triangles of a surface are touched from behind too, as a ray finds them.
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        const BodyId ceiling = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                           .shapes = {{.type = ShapeType::Mesh, .geometry = square}},
                                                           .position = Vec3(40.0f, 5.0f, 0.0f)}));
        hit = physics->sphereCast(Ray{Vec3(40.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)}, 0.5f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == ceiling);
        CHECK(hit->distance == doctest::Approx(4.5f).epsilon(1.0e-3));
        CHECK_NEAR(hit->normal, Vec3(0.0f, -1.0f, 0.0f), 1.0e-3f);

        // A ball too small to be one is the smallest there is: next to a ray.
        const Vec3 above(0.2f, 10.0f, -0.3f);
        hit = physics->sphereCast(Ray{above, down}, 1.0e-7f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->distance == doctest::Approx(8.0f).epsilon(1.0e-3));

        // Balls and ways that are none touch nothing.
        CHECK_FALSE(physics->sphereCast(Ray{above, Vec3(0.0f)}, 0.5f, 100.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, down}, 0.0f, 100.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, down}, -0.5f, 100.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, down}, kNotANumber, 100.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, down}, 0.5f, 0.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, down}, 0.5f, kNotANumber).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{Vec3(kNotANumber), down}, 0.5f, 100.0f).has_value());
        CHECK_FALSE(physics->sphereCast(Ray{above, Vec3(kNotANumber)}, 0.5f, 100.0f).has_value());
    }

    TEST_CASE("the bodies that reach into a ball or a box are found, each once") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        const BodyId crate = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                         .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f)}},
                                                         .position = Vec3(0.0f, 1.0f, 0.0f)}));
        const BodyId ball = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                        .shapes = {{.type = ShapeType::Sphere, .radius = 1.0f}},
                                                        .position = Vec3(10.0f, 1.0f, 0.0f)}));
        // One body of two boxes, three metres apart.
        const BodyId pair =
            unwrap(physics->createBody({.motion = BodyMotion::Static,
                                        .shapes = {{.type = ShapeType::Box, .offset = Vec3(-1.5f, 0.0f, 0.0f)},
                                                   {.type = ShapeType::Box, .offset = Vec3(1.5f, 0.0f, 0.0f)}},
                                        .position = Vec3(20.0f, 5.0f, 0.0f)}));
        using Bodies = std::vector<BodyId>;

        // A small ball in the middle of the crate: the crate alone. A larger one reaches
        // the floor as well - in the order in which the bodies were made.
        CHECK(physics->overlapSphere(Vec3(0.0f, 1.0f, 0.0f), 0.5f) == Bodies{crate});
        CHECK(physics->overlapSphere(Vec3(0.0f, 1.0f, 0.0f), 2.0f) == Bodies{floor, crate});
        // Between everything, nothing; large enough, everything near.
        CHECK(physics->overlapSphere(Vec3(5.0f, 3.0f, 0.0f), 1.0f).empty());
        CHECK(physics->overlapSphere(Vec3(5.0f, 1.0f, 0.0f), 4.5f) == Bodies{floor, crate, ball});
        // Only just not, and only just: a ball beside the crate, whose face is at one.
        CHECK(physics->overlapSphere(Vec3(1.6f, 1.0f, 0.0f), 0.55f).empty());
        CHECK(physics->overlapSphere(Vec3(1.6f, 1.0f, 0.0f), 0.65f) == Bodies{crate});
        // A body of two shapes that are both reached is one body.
        CHECK(physics->overlapSphere(Vec3(20.0f, 5.0f, 0.0f), 2.5f) == Bodies{pair});
        CHECK(physics->overlapSphere(Vec3(20.0f, 5.0f, 0.0f), 0.9f).empty());

        // A box beside the crate that does not reach it - until it is turned an eighth
        // about Y, and its corner does.
        const Vec3 beside(2.4f, 1.0f, 0.0f);
        const Vec3 size(2.0f, 1.0f, 2.0f);
        CHECK(physics->overlapBox(beside, size).empty());
        CHECK(physics->overlapBox(beside, size, glm::angleAxis(0.25f * kPi, Vec3(0.0f, 1.0f, 0.0f))) == Bodies{crate});
        // A rotation of any length is one.
        CHECK(physics->overlapBox(beside, size, 3.0f * glm::angleAxis(0.25f * kPi, Vec3(0.0f, 1.0f, 0.0f))) ==
              Bodies{crate});
        // A slab that lies on everything.
        CHECK(physics->overlapBox(Vec3(10.0f, 0.0f, 0.0f), Vec3(30.0f, 0.5f, 1.0f)) == Bodies{floor, crate, ball});

        // Of a surface it is the triangles that count, from either side.
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        const BodyId sheet = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                         .shapes = {{.type = ShapeType::Mesh, .geometry = square}},
                                                         .position = Vec3(40.0f, 4.0f, 0.0f)}));
        CHECK(physics->overlapSphere(Vec3(40.0f, 4.8f, 0.0f), 0.3f) == Bodies{sheet});
        CHECK(physics->overlapSphere(Vec3(40.0f, 5.2f, 0.0f), 0.3f) == Bodies{sheet});
        CHECK(physics->overlapSphere(Vec3(40.0f, 4.5f, 0.0f), 0.3f).empty());

        // Balls and boxes that are none have nothing in them.
        const Vec3 middle(0.0f, 1.0f, 0.0f);
        CHECK(physics->overlapSphere(middle, 0.0f).empty());
        CHECK(physics->overlapSphere(middle, -1.0f).empty());
        CHECK(physics->overlapSphere(middle, kNotANumber).empty());
        CHECK(physics->overlapSphere(Vec3(kNotANumber), 1.0f).empty());
        CHECK(physics->overlapBox(middle, Vec3(1.0f, 0.0f, 1.0f)).empty());
        CHECK(physics->overlapBox(middle, Vec3(1.0f, kNotANumber, 1.0f)).empty());
        CHECK(physics->overlapBox(Vec3(kNotANumber), Vec3(1.0f)).empty());
        CHECK(physics->overlapBox(middle, Vec3(1.0f), Quat(0.0f, 0.0f, 0.0f, 0.0f)).empty());
        CHECK(physics->overlapBox(middle, Vec3(1.0f)) == Bodies{crate});
        // What is too small to be a shape is the smallest there is.
        CHECK(physics->overlapSphere(middle, 1.0e-7f) == Bodies{crate});
        CHECK(physics->overlapBox(middle, Vec3(1.0f, 1.0e-7f, 1.0f)) == Bodies{crate});
        CHECK(physics->overlapBox(Vec3(0.0f, 5.0f, 0.0f), Vec3(1.0e-7f)).empty());

        // A body that is gone is not found any more.
        physics->destroyBody(crate);
        CHECK(physics->overlapSphere(middle, 2.0f) == Bodies{floor});
    }

    TEST_CASE("bodies say when they begin to touch, and when they stop") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.5f}},
                                                        .position = Vec3(0.0f, 3.0f, 0.0f),
                                                        .linearDamping = 0.0f,
                                                        .userData = 7}));
        CHECK(physics->contactEvents().empty());

        // While the ball falls there is nothing to say. Then it lands, and that is said
        // once: who, where, which way, and how hard - after two and a half metres it
        // comes down at seven metres a second.
        CHECK(stepAndListen(*physics, 30).empty());
        std::vector<ContactEvent> heard = stepAndListen(*physics, 30);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].phase == ContactPhase::Began);
        CHECK(heard[0].first == floor);
        CHECK(heard[0].second == ball);
        CHECK(heard[0].firstUserData == 0);
        CHECK(heard[0].secondUserData == 7);
        CHECK_FALSE(heard[0].sensor);
        CHECK_NEAR(heard[0].position, Vec3(0.0f), 0.05f);
        CHECK_NEAR(heard[0].normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-3f);
        CHECK(heard[0].speed == doctest::Approx(7.0f).epsilon(0.03));

        // While it lies there and falls asleep, and when it is woken where it lies,
        // the two touch on: nothing is said.
        CHECK(stepAndListen(*physics, 300).empty());
        CHECK_FALSE(physics->isActive(ball));
        physics->activate(ball);
        CHECK(stepAndListen(*physics, 10).empty());

        // Lifted off, they have stopped touching.
        physics->setPlacement(ball, Vec3(0.0f, 2.0f, 0.0f), kIdentityQuat);
        heard = stepAndListen(*physics, 1);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0] ==
              ContactEvent{.phase = ContactPhase::Ended, .first = floor, .second = ball, .secondUserData = 7});
        // It falls again, and they touch again.
        heard = stepAndListen(*physics, 60);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].phase == ContactPhase::Began);
        CHECK(heard[0].speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 1.5f)).epsilon(0.03));

        // What is destroyed stops touching - also asleep - and is still named.
        CHECK(stepAndListen(*physics, 300).empty());
        physics->destroyBody(ball);
        heard = stepAndListen(*physics, 1);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].phase == ContactPhase::Ended);
        CHECK(heard[0].second == ball);
        CHECK(heard[0].secondUserData == 7);
        CHECK_FALSE(physics->isValid(heard[0].second));
        // What is said is of the last step, and no longer.
        physics->step(kStep);
        CHECK(physics->contactEvents().empty());
    }

    TEST_CASE("two bodies touch once however many of their shapes meet, and are named in the order of the bodies") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        // A table that comes down on its four legs at once, and beside it a crate with
        // another above it.
        std::vector<ShapeDesc> legs;
        for (const float x : {-1.0f, 1.0f}) {
            for (const float z : {-0.5f, 0.5f}) {
                legs.push_back({.type = ShapeType::Box, .size = Vec3(0.2f, 1.0f, 0.2f), .offset = Vec3(x, 0.0f, z)});
            }
        }
        const BodyId table = unwrap(physics->createBody({.shapes = legs, .position = Vec3(0.0f, 0.6f, 0.0f)}));
        const BodyId lower = unwrap(physics->createBody({.position = Vec3(10.0f, 0.6f, 0.0f)}));
        const BodyId upper = unwrap(physics->createBody({.position = Vec3(10.0f, 2.0f, 0.0f)}));

        // The table and the lower crate land in the same step: first the pair whose
        // bodies come first. The upper crate lands on the lower one later.
        const std::vector<ContactEvent> heard = stepAndListen(*physics, 240);
        REQUIRE(heard.size() == 3);
        CHECK(heard[0].first == floor);
        CHECK(heard[0].second == table);
        CHECK(heard[1].first == floor);
        CHECK(heard[1].second == lower);
        CHECK(heard[2].first == lower);
        CHECK(heard[2].second == upper);
        // The lower crate is under the upper one: the normal points from the first to the second.
        CHECK_NEAR(heard[2].normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-2f);
        CHECK(heard[2].position.y == doctest::Approx(1.0f).epsilon(0.03));
        for (const ContactEvent& event : heard) {
            CHECK(event.phase == ContactPhase::Began);
        }
    }

    TEST_CASE("a sensor notices what reaches into it, and stops nothing") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        // The ball is made before the sensor. Of the two, the sensor is named first all the same.
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.25f}},
                                                        .position = Vec3(0.0f, 6.0f, 0.0f),
                                                        .linearDamping = 0.0f,
                                                        .userData = 7}));
        const BodyId gate =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, 1.0f, 2.0f)}},
                                        .position = Vec3(0.0f, 3.0f, 0.0f),
                                        .sensor = true,
                                        .userData = 100}));
        CHECK(physics->isSensor(gate));
        CHECK_FALSE(physics->isSensor(ball));
        CHECK(physics->motion(gate) == BodyMotion::Kinematic);

        // Nothing that looks for bodies finds a sensor: a ray and a ball go through it
        // to the floor, and nothing reaches into a ball in the middle of it.
        const Ray down{Vec3(0.5f, 10.0f, 0.5f), Vec3(0.0f, -1.0f, 0.0f)};
        std::optional<RayHit> hit = physics->raycast(down, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == floor);
        hit = physics->sphereCast(down, 0.1f, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == floor);
        CHECK(physics->overlapSphere(Vec3(0.5f, 3.0f, 0.5f), 0.3f).empty());
        CHECK(physics->overlapBox(Vec3(0.5f, 3.0f, 0.5f), Vec3(0.5f)).empty());

        // The ball falls through it to the floor: into the sensor, out of it, and down
        // on the floor - as fast as a fall of five and three quarter metres makes it.
        std::vector<ContactEvent> heard = stepAndListen(*physics, 120);
        REQUIRE(heard.size() == 3);
        CHECK(heard[0].phase == ContactPhase::Began);
        CHECK(heard[0].first == gate);
        CHECK(heard[0].second == ball);
        CHECK(heard[0].firstUserData == 100);
        CHECK(heard[0].secondUserData == 7);
        CHECK(heard[0].sensor);
        CHECK(heard[1].phase == ContactPhase::Ended);
        CHECK(heard[1].first == gate);
        CHECK(heard[1].second == ball);
        CHECK(heard[1].sensor);
        CHECK(heard[2].phase == ContactPhase::Began);
        CHECK(heard[2].first == floor);
        CHECK(heard[2].second == ball);
        CHECK_FALSE(heard[2].sensor);
        CHECK(heard[2].speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 5.75f)).epsilon(0.03));

        // What comes to rest in a sensor and falls asleep is still in it. The floor,
        // which the sensor reaches into, was never noticed: it does not move.
        const BodyId zone =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(4.0f, 2.0f, 4.0f)}},
                                        .position = Vec3(20.0f, 0.5f, 0.0f),
                                        .sensor = true}));
        const BodyId crate = unwrap(physics->createBody({.position = Vec3(20.0f, 0.5f, 0.0f)}));
        heard = stepAndListen(*physics, 400);
        REQUIRE(heard.size() == 2);
        CHECK(heard[0].first == floor);
        CHECK(heard[0].second == crate);
        CHECK(heard[1].first == zone);
        CHECK(heard[1].second == crate);
        CHECK(heard[1].phase == ContactPhase::Began);
        CHECK_FALSE(physics->isActive(crate));

        // The sensor is taken away from around it: the crate has left it, and sleeps on.
        physics->setPlacement(zone, Vec3(40.0f, 0.5f, 0.0f), kIdentityQuat);
        heard = stepAndListen(*physics, 3);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].phase == ContactPhase::Ended);
        CHECK(heard[0].first == zone);
        CHECK(heard[0].second == crate);
        CHECK_FALSE(physics->isActive(crate));

        // A body that is moved by hand is noticed like one that the simulation moves.
        const BodyId lift =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic, .position = Vec3(40.0f, 10.0f, 0.0f)}));
        CHECK(stepAndListen(*physics, 3).empty());
        physics->setPlacement(lift, Vec3(40.0f, 0.5f, 0.0f), kIdentityQuat);
        heard = stepAndListen(*physics, 3);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].phase == ContactPhase::Began);
        CHECK(heard[0].first == zone);
        CHECK(heard[0].second == lift);

        // Two sensors say nothing of each other.
        unwrap(physics->createBody(
            {.motion = BodyMotion::Kinematic, .position = Vec3(60.0f, 5.0f, 0.0f), .sensor = true}));
        unwrap(physics->createBody(
            {.motion = BodyMotion::Kinematic, .position = Vec3(60.2f, 5.0f, 0.0f), .sensor = true}));
        CHECK(stepAndListen(*physics, 5).empty());

        // A sensor that stands still would not notice what sleeps in it, and is refused.
        CHECK(refusalOf(physics->createBody({.motion = BodyMotion::Static, .sensor = true})) ==
              "a sensor has to notice what has fallen asleep in it, and a static body does not: make it kinematic");
    }

    TEST_CASE("what sleeps on a body wakes when that body is destroyed or put somewhere else") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // Three shelves a metre and a half up, each with a crate asleep on it, and a
        // crate on the floor far away from all of them.
        std::vector<BodyId> shelves;
        std::vector<BodyId> crates;
        for (const float x : {0.0f, 10.0f, 20.0f}) {
            shelves.push_back(
                unwrap(physics->createBody({.motion = BodyMotion::Static,
                                            .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, 0.2f, 2.0f)}},
                                            .position = Vec3(x, 1.4f, 0.0f)})));
            crates.push_back(unwrap(physics->createBody({.position = Vec3(x, 2.0f, 0.0f)})));
        }
        const BodyId apart = unwrap(physics->createBody({.position = Vec3(-10.0f, 0.5f, 0.0f)}));
        stepFor(*physics, 300);
        for (const BodyId crate : crates) {
            REQUIRE_FALSE(physics->isActive(crate));
            REQUIRE(physics->position(crate).y == doctest::Approx(2.0f).epsilon(0.02));
        }
        REQUIRE_FALSE(physics->isActive(apart));

        // The first shelf is destroyed, the second put somewhere else, and the third
        // turned on its side under its crate. None of the crates stays where nothing
        // holds it: each wakes, and falls to the floor.
        physics->destroyBody(shelves[0]);
        physics->setPlacement(shelves[1], Vec3(10.0f, 1.4f, 30.0f), kIdentityQuat);
        physics->setPlacement(shelves[2], Vec3(23.0f, 1.4f, 0.0f), glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f)));
        physics->step(kStep);
        for (const BodyId crate : crates) {
            CHECK(physics->isActive(crate));
        }
        // What was near none of it sleeps on.
        CHECK_FALSE(physics->isActive(apart));
        stepFor(*physics, 300);
        for (const BodyId crate : crates) {
            CHECK(physics->position(crate).y == doctest::Approx(0.5f).epsilon(0.02));
            CHECK_FALSE(physics->isActive(crate));
        }

        // A body that is put where another sleeps wakes it: a shelf brought down onto
        // the crate that lies apart pushes it out from under itself.
        physics->setPlacement(shelves[1], Vec3(-10.0f, 0.6f, 0.0f), kIdentityQuat);
        physics->step(kStep);
        CHECK(physics->isActive(apart));

        // So does a body that is made where another sleeps and that stands still - what
        // moves finds what sleeps by itself.
        stepFor(*physics, 300);
        REQUIRE_FALSE(physics->isActive(crates[0]));
        unwrap(physics->createBody({.motion = BodyMotion::Static, .position = Vec3(0.4f, 0.5f, 0.0f)}));
        CHECK(physics->isActive(crates[0]));
        CHECK_FALSE(physics->isActive(crates[2]));

        // A crate that sleeps on a crate falls when the one below is taken away.
        const BodyId lower = unwrap(physics->createBody({.position = Vec3(-20.0f, 0.5f, 0.0f)}));
        const BodyId upper = unwrap(physics->createBody({.position = Vec3(-20.0f, 1.5f, 0.0f)}));
        stepFor(*physics, 300);
        REQUIRE_FALSE(physics->isActive(upper));
        physics->setPlacement(lower, Vec3(-20.0f, 0.5f, 10.0f), kIdentityQuat);
        stepFor(*physics, 300);
        CHECK(physics->position(upper).y == doctest::Approx(0.5f).epsilon(0.02));
        // A sensor holds nothing up: taking one away wakes nobody.
        const BodyId zone = unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(3.0f)}},
                                                        .position = Vec3(-20.0f, 0.5f, 0.0f),
                                                        .sensor = true}));
        stepFor(*physics, 5);
        REQUIRE_FALSE(physics->isActive(upper));
        physics->destroyBody(zone);
        physics->step(kStep);
        CHECK_FALSE(physics->isActive(upper));
    }

    TEST_CASE("a body that is destroyed is gone, and its id is no body's again") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.maxBodies = 4}));
        const BodyId first = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
        CHECK(physics->statistics().bodies == 1);
        physics->destroyBody(first);
        CHECK_FALSE(physics->isValid(first));
        CHECK(physics->statistics().bodies == 0);
        // Destroying it again, or what never was a body, is ignored.
        physics->destroyBody(first);
        physics->destroyBody(BodyId());
        physics->destroyBody(BodyId{12345});

        // The next body takes its place, under an id of its own.
        const BodyId second = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
        CHECK(second != first);
        CHECK(physics->isValid(second));
        CHECK_FALSE(physics->isValid(first));
    }

    TEST_CASE("a world holds as many bodies as it was made for") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.maxBodies = 3}));
        std::vector<BodyId> bodies;
        for (int index = 0; index < 3; ++index) {
            bodies.push_back(
                unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}},
                                            .position = Vec3(static_cast<float>(index) * 3.0f, 0.0f, 0.0f)})));
        }
        const Result<BodyId> oneTooMany = physics->createBody({.shapes = {{.type = ShapeType::Sphere}}});
        REQUIRE_FALSE(oneTooMany.has_value());
        CHECK(oneTooMany.error().code == ErrorCode::OutOfRange);
        CHECK(oneTooMany.error().message == "the physics world is full: it holds 3 bodies");
        CHECK(physics->statistics().bodies == 3);
        // Room is given back with a body.
        physics->destroyBody(bodies[1]);
        CHECK(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}).has_value());
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
        CHECK(refused({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 0.0f, 1.0f)}}}) ==
              "a box of 1 by 0 by 1 metres has no volume to collide with");
        refused({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, -2.0f, 1.0f)}}});
        refused({.shapes = {{.type = ShapeType::Box, .size = Vec3(kNotANumber)}}});
        CHECK(refused({.shapes = {{.type = ShapeType::Sphere, .radius = 0.0f}}}) ==
              "a sphere of radius 0 has no volume to collide with");
        refused({.shapes = {{.type = ShapeType::Sphere, .radius = -1.0f}}});
        refused({.shapes = {{.type = ShapeType::Sphere, .radius = kNotANumber}}});
        CHECK(refused({.shapes = {{.type = ShapeType::Capsule, .radius = 0.5f, .height = 0.0f}}}) ==
              "a capsule of radius 0.5 and height 0 has no volume to collide with");
        refused({.shapes = {{.type = ShapeType::Capsule, .radius = 0.0f, .height = 2.0f}}});
        refused({.shapes = {{.type = ShapeType::Capsule, .radius = 0.5f, .height = kNotANumber}}});

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
            unwrap(physics->createBody({.shapes = {{.type = ShapeType::Capsule, .radius = 0.5f, .height = 0.6f}},
                                        .position = Vec3(0.0f, 2.0f, 0.0f)}));
        // A box thinner than its edges would be rounded by: a centimetre.
        const BodyId sliver =
            unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 0.01f, 1.0f)}},
                                        .position = Vec3(5.0f, 0.5f, 0.0f)}));
        stepFor(*physics, 240);
        CHECK(physics->position(stubby).y == doctest::Approx(0.5f).epsilon(0.03));
        // Lying on the floor, give or take what bodies may sink into each other.
        CHECK(physics->position(sliver).y > -0.03f);
        CHECK(physics->position(sliver).y < 0.03f);
    }

    TEST_CASE("a body is put together of shapes that are placed in it, and move as one") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // Two balls a metre to either side of where the body is, and a capsule laid
        // across between them, a metre up: turned a quarter turn about Z, it lies along X.
        const Quat lyingDown = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f));
        const BodyId dumbbell = unwrap(physics->createBody(
            {.motion = BodyMotion::Static,
             .shapes = {{.type = ShapeType::Sphere, .radius = 0.25f, .offset = Vec3(-1.0f, 0.0f, 0.0f)},
                        {.type = ShapeType::Sphere, .radius = 0.25f, .offset = Vec3(1.0f, 0.0f, 0.0f)},
                        {.type = ShapeType::Capsule,
                         .radius = 0.1f,
                         .height = 3.0f,
                         .offset = Vec3(0.0f, 1.0f, 0.0f),
                         .rotation = lyingDown * 2.0f}},
             .position = Vec3(10.0f, 5.0f, 0.0f)}));
        CHECK(physics->position(dumbbell) == Vec3(10.0f, 5.0f, 0.0f));
        CHECK(physics->statistics().bodies == 1);

        const auto hitFromAbove = [&](float x) {
            return physics->raycast(Ray{Vec3(x, 20.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        };
        // Over the middle: the capsule, whose top is 1.1 above the body.
        std::optional<RayHit> hit = hitFromAbove(10.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == dumbbell);
        CHECK(hit->position.y == doctest::Approx(6.1f).epsilon(1.0e-4));
        // It reaches a metre and a half to either side, and no further.
        CHECK(hitFromAbove(11.4f).has_value());
        CHECK(hitFromAbove(8.6f).has_value());
        CHECK_FALSE(hitFromAbove(11.6f).has_value());
        // From the side, below the capsule: a ball, a quarter of a metre round.
        hit = physics->raycast(Ray{Vec3(20.0f, 5.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->position.x == doctest::Approx(11.25f).epsilon(1.0e-4));
        // Between the balls there is nothing.
        CHECK_FALSE(physics->raycast(Ray{Vec3(10.0f, 5.0f, 20.0f), Vec3(0.0f, 0.0f, -1.0f)}, 100.0f).has_value());

        // Turned and moved, the shapes go along: a quarter turn about Y puts the balls
        // on the Z axis.
        physics->setPlacement(dumbbell, Vec3(0.0f), glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f)));
        hit = physics->raycast(Ray{Vec3(0.0f, 0.0f, 20.0f), Vec3(0.0f, 0.0f, -1.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->position.z == doctest::Approx(1.25f).epsilon(1.0e-4));

        // A table: a top on four legs. It stands on its legs, and its body - placed
        // where the middle of the top is - rests that high above the floor.
        addFloor(*physics);
        std::vector<ShapeDesc> table = {{.type = ShapeType::Box, .size = Vec3(2.0f, 0.1f, 1.0f)}};
        for (const float x : {-0.9f, 0.9f}) {
            for (const float z : {-0.4f, 0.4f}) {
                table.push_back({.type = ShapeType::Box, .size = Vec3(0.1f, 0.8f, 0.1f), .offset = Vec3(x, -0.45f, z)});
            }
        }
        const BodyId standing = unwrap(physics->createBody({.shapes = table, .position = Vec3(-20.0f, 1.2f, 0.0f)}));
        stepFor(*physics, 240);
        CHECK(physics->position(standing).y == doctest::Approx(0.85f).epsilon(0.02));
        CHECK(std::abs(physics->position(standing).x + 20.0f) < 0.01f);
        CHECK_FALSE(physics->isActive(standing));
        // The top is hit where it is, and between the legs under it there is the floor.
        hit = physics->raycast(Ray{Vec3(-20.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == standing);
        CHECK(hit->position.y == doctest::Approx(0.9f).epsilon(0.02));
        hit = physics->raycast(Ray{Vec3(-20.0f, 0.4f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}, 4.0f);
        CHECK_FALSE(hit.has_value());
    }

    TEST_CASE("a body of no shapes, of too many, or of shapes that cannot be placed is refused") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const auto refused = [&](const BodyDesc& desc) {
            const Result<BodyId> body = physics->createBody(desc);
            REQUIRE_FALSE(body.has_value());
            CHECK(body.error().code == ErrorCode::InvalidArgument);
            return body.error().message;
        };
        CHECK(refused({.shapes = {}}) == "a body is put together of 1 to 1024 shapes, not 0");
        CHECK(refused({.shapes = std::vector<ShapeDesc>(kMaxBodyShapes + 1)}) ==
              "a body is put together of 1 to 1024 shapes, not 1025");
        CHECK(refused({.shapes = {{.type = ShapeType::Sphere, .offset = Vec3(kNotANumber, 0.0f, 0.0f)}}}) ==
              "a sphere cannot be placed in its body: its offset or rotation is not made of numbers");
        refused({.shapes = {{.rotation = Quat(0.0f, 0.0f, 0.0f, 0.0f)}}});
        refused({.shapes = {{.rotation = Quat(kNotANumber, 0.0f, 0.0f, 0.0f)}}});
        // One shape that is none among good ones: the body is not made of the rest.
        CHECK(refused({.shapes = {{.type = ShapeType::Box},
                                  {.type = ShapeType::Sphere, .radius = 0.0f, .offset = Vec3(2.0f, 0.0f, 0.0f)},
                                  {.type = ShapeType::Box, .offset = Vec3(4.0f, 0.0f, 0.0f)}}}) ==
              "a sphere of radius 0 has no volume to collide with");
        CHECK(physics->statistics().bodies == 0);
        // As many as a body may have are no trouble.
        std::vector<ShapeDesc> many;
        for (uint32_t index = 0; index < kMaxBodyShapes; ++index) {
            many.push_back(
                {.type = ShapeType::Sphere, .radius = 0.1f, .offset = Vec3(static_cast<float>(index), 0.0f, 0.0f)});
        }
        CHECK(physics->createBody({.motion = BodyMotion::Static, .shapes = many}).has_value());
    }

    TEST_CASE("a convex hull is the shape around its points, at any size and either way round") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // The corners of the wedge under the plane x + y + z = 1 - and points inside it,
        // and one that is given twice, which change nothing.
        const std::vector<Vec3> points = {Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                          Vec3(0.0f, 0.0f, 1.0f), Vec3(0.1f, 0.1f, 0.1f), Vec3(0.2f, 0.3f, 0.1f),
                                          Vec3(1.0f, 0.0f, 0.0f)};
        const GeometryId wedge = unwrap(physics->createConvexHull(points));
        CHECK_FALSE(wedge.isNull());
        CHECK(physics->isValid(wedge));
        CHECK(physics->statistics().geometries == 1);
        const auto wedgeAt = [&](float x, const ShapeDesc& shape) {
            return unwrap(physics->createBody(
                {.motion = BodyMotion::Static, .shapes = {shape}, .position = Vec3(x, 0.0f, 0.0f)}));
        };

        // From above, the slanted face: as high as x and z leave of one.
        const BodyId body = wedgeAt(10.0f, {.type = ShapeType::ConvexHull, .geometry = wedge});
        const std::optional<RayHit> hit =
            physics->raycast(Ray{Vec3(10.25f, 5.0f, 0.25f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == body);
        CHECK(hit->position.y == doctest::Approx(0.5f).epsilon(1.0e-3));
        CHECK_NEAR(hit->normal, glm::normalize(Vec3(1.0f)), 1.0e-3f);
        CHECK(heightAt(*physics, 10.6f, 0.6f) == kNothingBelow);

        // The same geometry twice as wide, three times as tall and four times as deep.
        wedgeAt(20.0f, {.type = ShapeType::ConvexHull, .geometry = wedge, .scale = Vec3(2.0f, 3.0f, 4.0f)});
        CHECK(heightAt(*physics, 20.5f, 1.0f) == doctest::Approx(1.5f).epsilon(1.0e-3));
        // Scaled by less than nothing along X, it lies on the other side.
        wedgeAt(30.0f, {.type = ShapeType::ConvexHull, .geometry = wedge, .scale = Vec3(-1.0f, 1.0f, 1.0f)});
        CHECK(heightAt(*physics, 29.75f, 0.25f) == doctest::Approx(0.5f).epsilon(1.0e-3));
        CHECK(heightAt(*physics, 30.25f, 0.25f) == kNothingBelow);
        // Placed and turned in its body like any other shape: a quarter turn about Z
        // puts what was along X upright, and what was upright along -X.
        wedgeAt(40.0f, {.type = ShapeType::ConvexHull,
                        .geometry = wedge,
                        .offset = Vec3(0.0f, 2.0f, 0.0f),
                        .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f))});
        CHECK(heightAt(*physics, 39.75f, 0.25f) == doctest::Approx(2.5f).epsilon(1.0e-3));
        CHECK(heightAt(*physics, 40.25f, 0.25f) == kNothingBelow);
        CHECK(physics->statistics().geometries == 1);

        // A hull has an inside, and so the simulation can move a body of it: a box
        // given by its corners falls on the floor and lies there.
        addFloor(*physics);
        const GeometryId box = unwrap(physics->createConvexHull(boxCorners(Vec3(1.0f, 0.5f, 2.0f))));
        const BodyId crate = unwrap(physics->createBody({.shapes = {{.type = ShapeType::ConvexHull, .geometry = box}},
                                                         .position = Vec3(0.0f, 3.0f, 0.0f),
                                                         .mass = 5.0f}));
        stepFor(*physics, 240);
        CHECK(physics->position(crate).y == doctest::Approx(0.25f).epsilon(0.02));
        CHECK_FALSE(physics->isActive(crate));
    }

    TEST_CASE("a hull around many points is made of the corners that matter most") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A ball of a metre as seven hundred and sixty-two points: three times the
        // corners a hull is made of.
        constexpr int kRings = 20;
        constexpr int kSegments = 40;
        std::vector<Vec3> points = {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)};
        for (int ring = 1; ring < kRings; ++ring) {
            const float polar = kPi * static_cast<float>(ring) / static_cast<float>(kRings);
            for (int segment = 0; segment < kSegments; ++segment) {
                const float around = kTwoPi * static_cast<float>(segment) / static_cast<float>(kSegments);
                points.emplace_back(std::sin(polar) * std::cos(around), std::cos(polar),
                                    std::sin(polar) * std::sin(around));
            }
        }
        const GeometryId ball = unwrap(physics->createConvexHull(points));
        unwrap(physics->createBody(
            {.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::ConvexHull, .geometry = ball}}}));
        // It is the ball all the same, but for what the corners that were left out
        // stood out by.
        CHECK(heightAt(*physics, 0.0f, 0.0f) == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(heightAt(*physics, 0.5f, 0.0f) == doctest::Approx(std::sqrt(0.75f)).epsilon(0.05));
        CHECK(heightAt(*physics, 1.1f, 0.0f) == kNothingBelow);
        // A shape of 256 corners at the most has fewer than three times as many edges.
        const std::vector<Vec3> edges = physics->geometryEdges(ball, 10000);
        CHECK(edges.size() > 2 * 200);
        CHECK(edges.size() < 2 * 3 * 256);
    }

    TEST_CASE("points that have no volume between them are no hull, and it is said why") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.maxGeometryPoints = 8}));
        const std::vector<Vec3> corners = boxCorners(Vec3(1.0f));
        // Too few, and more than the world was made for.
        CHECK(refusalOf(physics->createConvexHull(std::span(corners).first(3))) ==
              "a convex hull is found around 4 to 8 points, not 3");
        std::vector<Vec3> nine = corners;
        nine.emplace_back(0.0f);
        CHECK(refusalOf(physics->createConvexHull(nine)) == "a convex hull is found around 4 to 8 points, not 9");
        CHECK(refusalOf(physics->createConvexHull({})) == "a convex hull is found around 4 to 8 points, not 0");
        // Eight are as many as may be.
        CHECK(physics->createConvexHull(corners).has_value());

        // A point that is no number.
        std::vector<Vec3> broken = corners;
        broken[5].y = kNotANumber;
        CHECK(refusalOf(physics->createConvexHull(broken)) == "point 5 of 8 for a convex hull is not made of numbers");
        broken[5].y = std::numeric_limits<float>::infinity();
        CHECK_FALSE(physics->createConvexHull(broken).has_value());

        // Points in one plane, on one line, and in one place.
        const std::vector<Vec3> flat = {Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 1.0f),
                                        Vec3(0.0f, 0.0f, 1.0f), Vec3(0.5f, 0.0f, 0.5f)};
        CHECK(refusalOf(physics->createConvexHull(flat)) ==
              "the 5 points for a convex hull lie in one plane: there is no volume between them");
        const std::vector<Vec3> line = {Vec3(0.0f), Vec3(1.0f), Vec3(2.0f), Vec3(3.0f)};
        CHECK_FALSE(refusalOf(physics->createConvexHull(line)).empty());
        const std::vector<Vec3> place(4, Vec3(1.0f, 2.0f, 3.0f));
        CHECK_FALSE(refusalOf(physics->createConvexHull(place)).empty());
        CHECK(physics->statistics().geometries == 1);
    }

    TEST_CASE("a surface of triangles collides where its triangles are") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        CHECK(physics->isValid(square));
        CHECK(physics->statistics().geometries == 1);
        const BodyId ground = unwrap(physics->createBody(
            {.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::Mesh, .geometry = square}}}));

        const std::optional<RayHit> hit =
            physics->raycast(Ray{Vec3(0.5f, 5.0f, 0.5f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == ground);
        CHECK(hit->position.y == doctest::Approx(1.0f).epsilon(1.0e-4));
        CHECK_NEAR(hit->normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);
        CHECK(heightAt(*physics, 1.1f, 0.0f) == kNothingBelow);

        // A ball that is dropped on it lies on it.
        const BodyId ball = unwrap(physics->createBody(
            {.shapes = {{.type = ShapeType::Sphere, .radius = 0.25f}}, .position = Vec3(0.2f, 3.0f, 0.1f)}));
        stepFor(*physics, 240);
        CHECK(physics->position(ball).y == doctest::Approx(1.25f).epsilon(0.02));

        // The same surface three times as wide and deep, twice as high up, and placed
        // in its body half a metre higher still.
        unwrap(physics->createBody({.motion = BodyMotion::Static,
                                    .shapes = {{.type = ShapeType::Mesh,
                                                .geometry = square,
                                                .scale = Vec3(3.0f, 2.0f, 3.0f),
                                                .offset = Vec3(0.0f, 0.5f, 0.0f)}},
                                    .position = Vec3(20.0f, 0.0f, 0.0f)}));
        CHECK(heightAt(*physics, 22.9f, 0.0f) == doctest::Approx(2.5f).epsilon(1.0e-4));
        CHECK(heightAt(*physics, 23.1f, 0.0f) == kNothingBelow);

        // Scaled by less than nothing along Y it is a metre below, and faces down.
        const BodyId ceiling = unwrap(physics->createBody(
            {.motion = BodyMotion::Static,
             .shapes = {{.type = ShapeType::Mesh, .geometry = square, .scale = Vec3(1.0f, -1.0f, 1.0f)}},
             .position = Vec3(40.0f, 0.0f, 0.0f)}));
        const std::optional<RayHit> fromBelow =
            physics->raycast(Ray{Vec3(40.5f, -5.0f, 0.5f), Vec3(0.0f, 1.0f, 0.0f)}, 100.0f);
        REQUIRE(fromBelow.has_value());
        CHECK(fromBelow->body == ceiling);
        CHECK(fromBelow->position.y == doctest::Approx(-1.0f).epsilon(1.0e-4));
        CHECK_NEAR(fromBelow->normal, Vec3(0.0f, -1.0f, 0.0f), 1.0e-4f);
    }

    TEST_CASE("a surface has no inside: it is hit from behind too, and no body of it is moved by the simulation") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        const BodyId ground = unwrap(physics->createBody(
            {.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::Mesh, .geometry = square}}}));

        // A ray finds the surface from the side it does not face as well - what is
        // looked for with a ray is there, whichever way round it is. The normal is
        // that of the surface: the way it faces.
        const std::optional<RayHit> fromBehind =
            physics->raycast(Ray{Vec3(0.5f, -5.0f, 0.5f), Vec3(0.0f, 1.0f, 0.0f)}, 100.0f);
        REQUIRE(fromBehind.has_value());
        CHECK(fromBehind->body == ground);
        CHECK(fromBehind->distance == doctest::Approx(6.0f).epsilon(1.0e-4));
        CHECK_NEAR(fromBehind->normal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);

        // Moved by hand it can be: kinematic, it lifts what lies on it.
        const BodyId lift = unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                                        .shapes = {{.type = ShapeType::Mesh, .geometry = square}},
                                                        .position = Vec3(20.0f, 0.0f, 0.0f)}));
        const BodyId ball = unwrap(physics->createBody(
            {.shapes = {{.type = ShapeType::Sphere, .radius = 0.25f}}, .position = Vec3(20.0f, 1.25f, 0.0f)}));
        stepFor(*physics, 30);
        for (int step = 1; step <= 60; ++step) {
            physics->moveKinematic(lift, Vec3(20.0f, static_cast<float>(step) / 60.0f, 0.0f), kIdentityQuat, kStep);
            physics->step(kStep);
        }
        // Told to be where it is, it comes to a halt there.
        physics->moveKinematic(lift, Vec3(20.0f, 1.0f, 0.0f), kIdentityQuat, kStep);
        stepFor(*physics, 30);
        CHECK(physics->position(lift).y == doctest::Approx(1.0f).epsilon(1.0e-3));
        CHECK(physics->position(ball).y == doctest::Approx(2.25f).epsilon(0.02));

        // By the simulation it cannot: there is no mass to spread over what has no inside.
        CHECK(refusalOf(physics->createBody({.shapes = {{.type = ShapeType::Mesh, .geometry = square}}})) ==
              "a dynamic body cannot be made of the triangles of a mesh, which have no inside: a convex hull has one");
        // Not with a shape beside it that has one, either.
        CHECK_FALSE(
            physics->createBody({.shapes = {{.type = ShapeType::Box}, {.type = ShapeType::Mesh, .geometry = square}}})
                .has_value());
        CHECK(physics->statistics().bodies == 3);
    }

    TEST_CASE("triangles that are none are left out, and a surface of none is refused") {
        const std::unique_ptr<PhysicsWorld> physics =
            unwrap(PhysicsWorld::create({.maxGeometryPoints = 4, .maxGeometryTriangles = 3}));
        // Of three triangles one has no area and one is there twice: one is left.
        const std::vector<uint32_t> withoutArea = {0, 1, 2, 0, 0, 1, 0, 1, 2};
        const GeometryId triangle = unwrap(physics->createTriangleMesh(kSquarePositions, withoutArea));
        CHECK(physics->geometryEdges(triangle, 100).size() == 2 * 3);
        // None is left of these.
        const std::vector<uint32_t> noArea = {0, 0, 1, 2, 2, 2};
        CHECK(refusalOf(physics->createTriangleMesh(kSquarePositions, noArea)) ==
              "none of the 2 triangles of a surface has an area");

        // Indices that are not three to a triangle, or name no position.
        const std::vector<uint32_t> four = {0, 1, 2, 3};
        CHECK(refusalOf(physics->createTriangleMesh(kSquarePositions, four)) ==
              "a surface of triangles is made of 1 to 4 positions and three indices to each of 1 to 3 triangles, "
              "not of 4 positions and 4 indices");
        const std::vector<uint32_t> beyond = {0, 1, 2, 0, 2, 4};
        CHECK(refusalOf(physics->createTriangleMesh(kSquarePositions, beyond)) ==
              "triangle 1 of a surface names position 4, and there are 4");
        // Nothing at all, and more than the world was made for.
        CHECK_FALSE(physics->createTriangleMesh({}, kSquareIndices).has_value());
        CHECK_FALSE(physics->createTriangleMesh(kSquarePositions, {}).has_value());
        std::vector<Vec3> five = kSquarePositions;
        five.emplace_back(0.0f);
        CHECK_FALSE(physics->createTriangleMesh(five, kSquareIndices).has_value());
        const std::vector<uint32_t> fourTriangles = {0, 1, 2, 0, 2, 3, 1, 2, 3, 0, 1, 3};
        CHECK_FALSE(physics->createTriangleMesh(kSquarePositions, fourTriangles).has_value());
        // A position that is no number.
        std::vector<Vec3> broken = kSquarePositions;
        broken[2].z = kNotANumber;
        CHECK(refusalOf(physics->createTriangleMesh(broken, kSquareIndices)) ==
              "position 2 of 4 for a surface of triangles is not made of numbers");
        CHECK(physics->statistics().geometries == 1);
    }

    TEST_CASE("geometry that is forgotten leaves the bodies made of it, and is no geometry again") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const GeometryId box = unwrap(physics->createConvexHull(boxCorners(Vec3(1.0f))));
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        CHECK(box != square);
        unwrap(physics->createBody(
            {.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::ConvexHull, .geometry = box}}}));
        CHECK(physics->statistics().geometries == 2);

        physics->destroyGeometry(box);
        CHECK_FALSE(physics->isValid(box));
        CHECK(physics->statistics().geometries == 1);
        // The body that was made of it is as it was.
        CHECK(heightAt(*physics, 0.0f, 0.0f) == doctest::Approx(0.5f).epsilon(1.0e-3));
        // No body is made of it any more - nor of geometry of the other kind, or of none.
        const auto bodyOf = [&](ShapeType type, GeometryId geometry) {
            return physics->createBody(
                {.motion = BodyMotion::Static, .shapes = {{.type = type, .geometry = geometry}}});
        };
        CHECK(refusalOf(bodyOf(ShapeType::ConvexHull, box)) ==
              "a shape of the type convexHull needs geometry that was prepared for one, and has none");
        CHECK(refusalOf(bodyOf(ShapeType::ConvexHull, square)) ==
              "a shape of the type convexHull needs geometry that was prepared for one, and has none");
        CHECK(refusalOf(bodyOf(ShapeType::Mesh, GeometryId())) ==
              "a shape of the type mesh needs geometry that was prepared for one, and has none");
        // Forgetting it again, or what never was, is nothing.
        physics->destroyGeometry(box);
        physics->destroyGeometry(GeometryId{12345});
        physics->destroyGeometry(GeometryId());
        CHECK(physics->statistics().geometries == 1);
        // What is prepared next is not taken for it.
        const GeometryId next = unwrap(physics->createConvexHull(boxCorners(Vec3(2.0f))));
        CHECK(next != box);
        CHECK_FALSE(physics->isValid(box));

        // A scale that leaves a hull no volume, or a surface no size, or is no number.
        const auto scaled = [&](ShapeType type, GeometryId geometry, const Vec3& scale) {
            return physics->createBody(
                {.motion = BodyMotion::Static, .shapes = {{.type = type, .geometry = geometry, .scale = scale}}});
        };
        CHECK(refusalOf(scaled(ShapeType::ConvexHull, next, Vec3(1.0f, 0.0f, 1.0f))) ==
              "a convexHull scaled by 1 by 0 by 1 is 2 by 0 by 2 metres, and has no volume to collide with");
        CHECK_FALSE(scaled(ShapeType::ConvexHull, next, Vec3(1.0f, 1.0e-4f, 1.0f)).has_value());
        CHECK_FALSE(scaled(ShapeType::ConvexHull, next, Vec3(1.0f, kNotANumber, 1.0f)).has_value());
        CHECK_FALSE(scaled(ShapeType::Mesh, square, Vec3(0.0f, 1.0f, 1.0f)).has_value());
        CHECK_FALSE(scaled(ShapeType::Mesh, square, Vec3(1.0e-5f)).has_value());
        // A surface may be flat, and thin things are things: a millimetre is enough.
        CHECK(scaled(ShapeType::Mesh, square, Vec3(1.0f, 1.0e-3f, 1.0f)).has_value());
        CHECK(scaled(ShapeType::ConvexHull, next, Vec3(1.0f, 1.0e-3f, 1.0f)).has_value());
    }

    TEST_CASE("the edges of geometry are those of its faces, each once") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A box that is not around the origin: twelve edges between its eight corners,
        // where they were given.
        std::vector<Vec3> corners = boxCorners(Vec3(1.0f));
        for (Vec3& corner : corners) {
            corner += Vec3(5.0f, 0.0f, -2.0f);
        }
        const GeometryId box = unwrap(physics->createConvexHull(corners));
        const std::vector<Vec3> boxEdges = physics->geometryEdges(box, 100);
        REQUIRE(boxEdges.size() == 2 * 12);
        // The box around it is where it was given, too.
        CHECK_NEAR(physics->geometryBounds(box).min, Vec3(4.5f, -0.5f, -2.5f), 1.0e-4f);
        CHECK_NEAR(physics->geometryBounds(box).max, Vec3(5.5f, 0.5f, -1.5f), 1.0e-4f);
        for (size_t edge = 0; edge < boxEdges.size(); edge += 2) {
            CAPTURE(edge);
            // A metre long, along one axis, from a corner to a corner.
            const Vec3 along = glm::abs(boxEdges[edge + 1] - boxEdges[edge]);
            CHECK(along.x + along.y + along.z == doctest::Approx(1.0f).epsilon(1.0e-4));
            CHECK(std::max({along.x, along.y, along.z}) == doctest::Approx(1.0f).epsilon(1.0e-4));
            CHECK_NEAR(glm::abs(boxEdges[edge] - Vec3(5.0f, 0.0f, -2.0f)), Vec3(0.5f), 1.0e-4f);
        }

        // A square of two triangles: its four sides, and the one the triangles share.
        const GeometryId square = unwrap(physics->createTriangleMesh(kSquarePositions, kSquareIndices));
        const std::vector<Vec3> squareEdges = physics->geometryEdges(square, 100);
        REQUIRE(squareEdges.size() == 2 * 5);
        float length = 0.0f;
        for (size_t edge = 0; edge < squareEdges.size(); edge += 2) {
            CHECK(squareEdges[edge].y == doctest::Approx(1.0f).epsilon(1.0e-4));
            length += glm::distance(squareEdges[edge], squareEdges[edge + 1]);
        }
        CHECK(length == doctest::Approx(4.0f * 2.0f + 2.0f * std::sqrt(2.0f)).epsilon(1.0e-4));

        // Given as triangles that share no position - as a mesh has them along a seam -
        // it is the same surface: corners in one place are one corner.
        const std::vector<Vec3> apart = {kSquarePositions[0], kSquarePositions[1], kSquarePositions[2],
                                         kSquarePositions[0], kSquarePositions[2], kSquarePositions[3]};
        const std::vector<uint32_t> inOrder = {0, 1, 2, 3, 4, 5};
        const GeometryId seamed = unwrap(physics->createTriangleMesh(apart, inOrder));
        CHECK(physics->geometryEdges(seamed, 100).size() == 2 * 5);

        // More edges than were asked for are none at all: half a picture says less than none.
        CHECK(physics->geometryEdges(box, 12).size() == 2 * 12);
        CHECK(physics->geometryEdges(box, 11).empty());
        CHECK(physics->geometryEdges(square, 4).empty());
        CHECK(physics->geometryEdges(square, 0).empty());
        CHECK_NEAR(physics->geometryBounds(square).min, Vec3(-1.0f, 1.0f, -1.0f), 1.0e-4f);
        CHECK_NEAR(physics->geometryBounds(square).max, Vec3(1.0f, 1.0f, 1.0f), 1.0e-4f);
        // And geometry that is none has none, and no box around it.
        CHECK(physics->geometryEdges(GeometryId(), 100).empty());
        CHECK(physics->geometryBounds(GeometryId()).isEmpty());
        physics->destroyGeometry(box);
        CHECK(physics->geometryEdges(box, 100).empty());
    }

    TEST_CASE("a fixed joint holds a body to the world, and two bodies to each other") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A crate that is joined to the world where it is hangs in the air.
        const BodyId crate = unwrap(physics->createBody({.position = Vec3(0.0f, 5.0f, 0.0f)}));
        const JointId toWorld = unwrap(
            physics->createJoint({.type = JointType::Fixed, .first = crate, .secondPoint = Vec3(0.0f, 5.0f, 0.0f)}));
        CHECK_FALSE(toWorld.isNull());
        CHECK(physics->isValid(toWorld));
        CHECK(physics->statistics().joints == 1);
        stepFor(*physics, 120);
        CHECK_NEAR(physics->position(crate), Vec3(0.0f, 5.0f, 0.0f), 5.0e-3f);

        // A second one joined to its side - a metre and a half along the first one's X -
        // is held out there, though nothing is under it.
        const BodyId beside = unwrap(physics->createBody({.position = Vec3(1.5f, 5.0f, 0.0f)}));
        const JointId toCrate = unwrap(physics->createJoint(
            {.type = JointType::Fixed, .first = beside, .second = crate, .secondPoint = Vec3(1.5f, 0.0f, 0.0f)}));
        CHECK(toCrate != toWorld);
        CHECK(physics->statistics().joints == 2);
        stepFor(*physics, 120);
        CHECK_NEAR(physics->position(crate), Vec3(0.0f, 5.0f, 0.0f), 0.02f);
        CHECK_NEAR(physics->position(beside), Vec3(1.5f, 5.0f, 0.0f), 0.03f);

        // Let go of by the world, the two fall as one: side by side, as far apart as they were.
        physics->destroyJoint(toWorld);
        CHECK_FALSE(physics->isValid(toWorld));
        CHECK(physics->isValid(toCrate));
        stepFor(*physics, 30);
        CHECK(physics->position(crate).y < 4.0f);
        CHECK(physics->position(beside).y < 4.1f);
        CHECK(glm::distance(physics->position(beside), physics->position(crate)) ==
              doctest::Approx(1.5f).epsilon(0.01));
    }

    TEST_CASE("a ball joint holds a point, and lets a body swing about it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A pendulum of two metres, let go level with what it hangs from.
        const Vec3 pivot(0.0f, 5.0f, 0.0f);
        const BodyId bob =
            unwrap(physics->createBody(undamped({.type = ShapeType::Sphere, .radius = 0.25f}, Vec3(2.0f, 5.0f, 0.0f))));
        unwrap(physics->createJoint(
            {.type = JointType::Ball, .first = bob, .firstPoint = Vec3(-2.0f, 0.0f, 0.0f), .secondPoint = pivot}));

        float lowest = 5.0f;
        float farthest = 2.0f;
        for (int step = 0; step < 240; ++step) {
            physics->step(kStep);
            // Wherever it is, it is two metres from the pivot.
            REQUIRE(glm::distance(physics->position(bob), pivot) == doctest::Approx(2.0f).epsilon(0.01));
            lowest = std::min(lowest, physics->position(bob).y);
            farthest = std::min(farthest, physics->position(bob).x);
        }
        // It went through the bottom, and well up the other side.
        CHECK(lowest == doctest::Approx(3.0f).epsilon(0.01));
        CHECK(farthest < -1.5f);
    }

    TEST_CASE("a hinge lets a body turn about its axis, and no further than it is limited to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        // Two doors of a metre, hinged to the world at an edge about Y and pushed open
        // at two radians a second. The second may open an eighth of a turn that way, and
        // half as far the other.
        const auto doorAt = [&](float x, bool limited) {
            const BodyId door = unwrap(physics->createBody(
                undamped({.type = ShapeType::Box, .size = Vec3(1.0f, 2.0f, 0.1f)}, Vec3(x + 0.5f, 1.0f, 0.0f))));
            unwrap(physics->createJoint({.type = JointType::Hinge,
                                         .first = door,
                                         .firstPoint = Vec3(-0.5f, 0.0f, 0.0f),
                                         .secondPoint = Vec3(x, 1.0f, 0.0f),
                                         .limited = limited,
                                         .minimum = -0.125f * kPi,
                                         .maximum = 0.25f * kPi}));
            // Turning about the hinge, its middle goes round at half a metre.
            physics->setAngularVelocity(door, Vec3(0.0f, 2.0f, 0.0f));
            physics->setLinearVelocity(door, Vec3(0.0f, 0.0f, -1.0f));
            return door;
        };
        const BodyId open = doorAt(0.0f, false);
        const BodyId held = doorAt(10.0f, true);

        float widest = 0.0f;
        for (int step = 0; step < 60; ++step) {
            physics->step(kStep);
            // Each stays on its hinge: half a metre from the axis, at the height it was.
            for (const auto& [door, x] : {std::pair{open, 0.0f}, std::pair{held, 10.0f}}) {
                const Vec3 middle = physics->position(door) - Vec3(x, 0.0f, 0.0f);
                REQUIRE(glm::length(Vec2(middle.x, middle.z)) == doctest::Approx(0.5f).epsilon(0.02));
                REQUIRE(middle.y == doctest::Approx(1.0f).epsilon(0.01));
            }
            widest = std::max(widest, glm::angle(physics->rotation(held)));
            REQUIRE(glm::angle(physics->rotation(held)) < 0.25f * kPi + 0.03f);
        }
        // After a second the one that is free has turned two radians about Y.
        CHECK_NEAR(physics->position(open), Vec3(0.5f * std::cos(2.0f), 1.0f, -0.5f * std::sin(2.0f)), 0.03f);
        // The other went as far as it may: an eighth of a turn about +Y, the way it was
        // pushed, which takes its middle towards -Z.
        CHECK(widest == doctest::Approx(0.25f * kPi).epsilon(0.03));
        // Pushed back the other way - turning about the hinge again, its middle going
        // round with it - it goes half as far past where it started.
        const Vec3 spin(0.0f, -2.0f, 0.0f);
        physics->setAngularVelocity(held, spin);
        physics->setLinearVelocity(held, glm::cross(spin, physics->position(held) - Vec3(10.0f, 1.0f, 0.0f)));
        float back = 0.0f;
        for (int step = 0; step < 90; ++step) {
            physics->step(kStep);
            back = std::max(back, physics->position(held).z);
        }
        CHECK(back == doctest::Approx(0.5f * std::sin(0.125f * kPi)).epsilon(0.03));
    }

    TEST_CASE("a slider lets a body slide along its axis, and no further than it is limited to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A drawer that slides along X: from half a metre back to a metre out.
        const BodyId drawer = unwrap(physics->createBody(undamped({.type = ShapeType::Box}, Vec3(0.0f, 1.0f, 0.0f))));
        unwrap(physics->createJoint({.type = JointType::Slider,
                                     .first = drawer,
                                     .secondPoint = Vec3(0.0f, 1.0f, 0.0f),
                                     .firstAxis = Vec3(1.0f, 0.0f, 0.0f),
                                     .secondAxis = Vec3(1.0f, 0.0f, 0.0f),
                                     .firstNormal = Vec3(0.0f, 1.0f, 0.0f),
                                     .secondNormal = Vec3(0.0f, 1.0f, 0.0f),
                                     .limited = true,
                                     .minimum = -0.5f,
                                     .maximum = 1.0f}));
        // It does not fall, and does not turn: it has only the one way to go.
        physics->setAngularVelocity(drawer, Vec3(1.0f, 2.0f, 3.0f));
        stepFor(*physics, 60);
        CHECK_NEAR(physics->position(drawer), Vec3(0.0f, 1.0f, 0.0f), 0.02f);
        CHECK(glm::angle(physics->rotation(drawer)) < 0.05f);

        // Pulled out, it stops at a metre; pushed in, at half a metre the other way. It
        // arrives at speed, and may be past the end by what it goes in a step.
        physics->setLinearVelocity(drawer, Vec3(3.0f, 0.0f, 0.0f));
        for (int step = 0; step < 120; ++step) {
            physics->step(kStep);
            REQUIRE(physics->position(drawer).x < 1.0f + 3.0f * kStep + 0.01f);
        }
        CHECK(physics->position(drawer).x == doctest::Approx(1.0f).epsilon(0.02));
        physics->setLinearVelocity(drawer, Vec3(-5.0f, 0.0f, 0.0f));
        for (int step = 0; step < 120; ++step) {
            physics->step(kStep);
            REQUIRE(physics->position(drawer).x > -0.5f - 5.0f * kStep - 0.01f);
        }
        CHECK(physics->position(drawer).x == doctest::Approx(-0.5f).epsilon(0.02));
        CHECK(physics->position(drawer).y == doctest::Approx(1.0f).epsilon(0.01));
    }

    TEST_CASE("a distance joint keeps two points as far apart as it is told: a rod, a rope, a spring") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const auto weightAt = [&](const Vec3& position) {
            return unwrap(physics->createBody(undamped({.type = ShapeType::Sphere, .radius = 0.25f}, position)));
        };
        // A rod: not limited, it keeps the weight as far from what it hangs on as it is.
        const BodyId onRod = weightAt(Vec3(0.0f, 6.0f, 0.0f));
        unwrap(
            physics->createJoint({.type = JointType::Distance, .first = onRod, .secondPoint = Vec3(0.0f, 8.0f, 0.0f)}));
        // A rope of two metres: the weight, let go a metre below, falls another metre.
        const BodyId onRope = weightAt(Vec3(10.0f, 7.0f, 0.0f));
        unwrap(physics->createJoint({.type = JointType::Distance,
                                     .first = onRope,
                                     .secondPoint = Vec3(10.0f, 8.0f, 0.0f),
                                     .limited = true,
                                     .maximum = 2.0f}));
        // A spring of two metres that swings twice a second: the weight pulls it a
        // little longer - by what gravity is to the square of that swing - and stops.
        const BodyId onSpring = weightAt(Vec3(20.0f, 6.0f, 0.0f));
        unwrap(physics->createJoint({.type = JointType::Distance,
                                     .first = onSpring,
                                     .secondPoint = Vec3(20.0f, 8.0f, 0.0f),
                                     .limited = true,
                                     .minimum = 2.0f,
                                     .maximum = 2.0f,
                                     .frequency = 2.0f,
                                     .damping = 0.5f}));

        // Pushed sideways the rod swings, and is two metres long all the while.
        physics->setLinearVelocity(onRod, Vec3(3.0f, 0.0f, 0.0f));
        for (int step = 0; step < 300; ++step) {
            physics->step(kStep);
            REQUIRE(glm::distance(physics->position(onRod), Vec3(0.0f, 8.0f, 0.0f)) ==
                    doctest::Approx(2.0f).epsilon(0.01));
            // It reaches the end of the rope at speed, and is a little past it for a step.
            REQUIRE(glm::distance(physics->position(onRope), Vec3(10.0f, 8.0f, 0.0f)) < 2.0f + 0.08f);
        }
        CHECK(physics->position(onRope).y == doctest::Approx(6.0f).epsilon(5.0e-3));
        const float stretch = 9.81f / ((kTwoPi * 2.0f) * (kTwoPi * 2.0f));
        CHECK(physics->position(onSpring).y == doctest::Approx(6.0f - stretch).epsilon(2.0e-3));
    }

    TEST_CASE("bodies that are joined do not collide with each other unless they are to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create({.gravity = Vec3(0.0f)}));
        // Two crates that reach into each other, joined as they are. And two more
        // beside them that are joined and to collide.
        const auto pairAt = [&](float x, bool collide) {
            const BodyId one = unwrap(physics->createBody({.position = Vec3(x, 5.0f, 0.0f)}));
            const BodyId other = unwrap(physics->createBody({.position = Vec3(x + 0.6f, 5.0f, 0.0f)}));
            const JointId joint = unwrap(physics->createJoint({.type = JointType::Ball,
                                                               .first = other,
                                                               .second = one,
                                                               .firstPoint = Vec3(-0.3f, 0.0f, 0.0f),
                                                               .secondPoint = Vec3(0.3f, 0.0f, 0.0f),
                                                               .collide = collide}));
            return std::tuple{one, other, joint};
        };
        const auto [one, other, joint] = pairAt(0.0f, false);
        const auto [third, fourth, colliding] = pairAt(10.0f, true);
        // A second joint between the first two.
        const JointId second = unwrap(physics->createJoint({.type = JointType::Ball,
                                                            .first = other,
                                                            .second = one,
                                                            .firstPoint = Vec3(-0.3f, 0.0f, 0.0f),
                                                            .secondPoint = Vec3(0.3f, 0.0f, 0.0f)}));

        // Only of the two that are to collide is anything said.
        std::vector<ContactEvent> heard = stepAndListen(*physics, 20);
        REQUIRE(heard.size() == 1);
        CHECK(heard[0].first == third);
        CHECK(heard[0].second == fourth);
        CHECK(glm::distance(physics->position(one), physics->position(other)) == doctest::Approx(0.6f).epsilon(0.01));

        // With one of two joints gone they are still joined, and still do not collide.
        physics->destroyJoint(second);
        CHECK(stepAndListen(*physics, 20).empty());
        // With the other gone too, they are two crates in each other's way.
        physics->destroyJoint(joint);
        heard = stepAndListen(*physics, 60);
        REQUIRE_FALSE(heard.empty());
        CHECK(heard[0].phase == ContactPhase::Began);
        CHECK(heard[0].first == one);
        CHECK(heard[0].second == other);
        CHECK(glm::distance(physics->position(one), physics->position(other)) > 0.95f);
        CHECK(physics->isValid(colliding));
        CHECK(physics->statistics().joints == 1);
    }

    TEST_CASE("a joint goes with either of its bodies, and what it held wakes") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A crate that hangs from the world, and one that hangs from a post.
        const BodyId hanging = unwrap(physics->createBody({.position = Vec3(0.0f, 5.0f, 0.0f)}));
        const JointId toWorld = unwrap(
            physics->createJoint({.type = JointType::Fixed, .first = hanging, .secondPoint = Vec3(0.0f, 5.0f, 0.0f)}));
        const BodyId post =
            unwrap(physics->createBody({.motion = BodyMotion::Static, .position = Vec3(10.0f, 8.0f, 0.0f)}));
        const BodyId lamp = unwrap(physics->createBody({.position = Vec3(10.0f, 5.0f, 0.0f)}));
        const JointId toPost = unwrap(physics->createJoint(
            {.type = JointType::Fixed, .first = lamp, .second = post, .secondPoint = Vec3(0.0f, -3.0f, 0.0f)}));
        stepFor(*physics, 300);
        REQUIRE_FALSE(physics->isActive(hanging));
        REQUIRE_FALSE(physics->isActive(lamp));
        CHECK_NEAR(physics->position(lamp), Vec3(10.0f, 5.0f, 0.0f), 0.02f);

        // The joint is taken away: the crate wakes, and falls.
        physics->destroyJoint(toWorld);
        CHECK(physics->isActive(hanging));
        // The post is destroyed: its joint is none any more, and the lamp falls too.
        physics->destroyBody(post);
        CHECK_FALSE(physics->isValid(toPost));
        CHECK(physics->statistics().joints == 0);
        CHECK(physics->isActive(lamp));
        stepFor(*physics, 300);
        CHECK(physics->position(hanging).y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(physics->position(lamp).y == doctest::Approx(0.5f).epsilon(0.02));

        // Taking away what is none is nothing, and a joint that is made later is another.
        physics->destroyJoint(toWorld);
        physics->destroyJoint(JointId());
        physics->destroyJoint(JointId{999});
        const JointId again = unwrap(
            physics->createJoint({.type = JointType::Ball, .first = hanging, .secondPoint = Vec3(0.0f, 0.5f, 0.0f)}));
        CHECK(again != toWorld);
        CHECK(again != toPost);
        // The body that is joined going, the joint goes as well.
        physics->destroyBody(hanging);
        CHECK_FALSE(physics->isValid(again));
    }

    TEST_CASE("joints that make no sense are refused, and it is said why") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId crate = unwrap(physics->createBody({.position = Vec3(0.0f, 5.0f, 0.0f)}));
        const BodyId wall =
            unwrap(physics->createBody({.motion = BodyMotion::Static, .position = Vec3(5.0f, 5.0f, 0.0f)}));
        const BodyId lift =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic, .position = Vec3(9.0f, 5.0f, 0.0f)}));
        const BodyId gone = unwrap(physics->createBody({.position = Vec3(20.0f, 5.0f, 0.0f)}));
        physics->destroyBody(gone);

        // Bodies that are none, that are gone, or one and the same.
        const std::string noBodies =
            "a hinge joint joins a body to another body of this world, or to none for the world";
        CHECK(refusalOf(physics->createJoint({.type = JointType::Hinge})) == noBodies);
        CHECK(refusalOf(physics->createJoint({.type = JointType::Hinge, .first = gone})) == noBodies);
        CHECK(refusalOf(physics->createJoint({.type = JointType::Hinge, .first = crate, .second = gone})) == noBodies);
        CHECK(refusalOf(physics->createJoint({.type = JointType::Hinge, .first = crate, .second = crate})) == noBodies);
        // Bodies of which none is moved by the simulation.
        const std::string nothingHeld = "a fixed joint holds nothing: neither of its bodies is dynamic, and only what "
                                        "the simulation moves can be held";
        CHECK(refusalOf(physics->createJoint({.first = wall})) == nothingHeld);
        CHECK(refusalOf(physics->createJoint({.first = lift})) == nothingHeld);
        CHECK(refusalOf(physics->createJoint({.first = lift, .second = wall})) == nothingHeld);
        // One that is, joined to one that is not, is held: a door on a wall, a crate on a lift.
        CHECK(physics->createJoint({.first = wall, .second = crate}).has_value());
        CHECK(physics->createJoint({.first = crate, .second = lift}).has_value());

        // Points and directions that are no numbers, and an axis that has no length.
        CHECK_FALSE(physics->createJoint({.first = crate, .firstPoint = Vec3(kNotANumber)}).has_value());
        CHECK_FALSE(physics->createJoint({.first = crate, .secondPoint = Vec3(0.0f, kNotANumber, 0.0f)}).has_value());
        CHECK_FALSE(physics->createJoint({.first = crate, .firstAxis = Vec3(kNotANumber)}).has_value());
        CHECK_FALSE(physics->createJoint({.first = crate, .secondNormal = Vec3(kNotANumber)}).has_value());
        CHECK(refusalOf(physics->createJoint({.type = JointType::Slider, .first = crate, .firstAxis = Vec3(0.0f)})) ==
              "the axis of a slider joint has no length, and so no direction");
        CHECK_FALSE(
            physics->createJoint({.type = JointType::Hinge, .first = crate, .secondAxis = Vec3(0.0f)}).has_value());
        // A ball has no axis to be without, and a normal that runs along the axis is made one that does not.
        CHECK(physics->createJoint({.type = JointType::Ball, .first = crate, .firstAxis = Vec3(0.0f)}).has_value());
        CHECK(physics->createJoint({.type = JointType::Hinge, .first = crate, .firstNormal = Vec3(0.0f, 3.0f, 0.0f)})
                  .has_value());

        // Limits that are not what the kind of joint has.
        const auto limited = [&](JointType type, float minimum, float maximum) {
            return physics->createJoint(
                {.type = type, .first = crate, .limited = true, .minimum = minimum, .maximum = maximum});
        };
        CHECK(refusalOf(limited(JointType::Hinge, 0.5f, 1.0f)) ==
              "a hinge turns from between -pi and 0 to between 0 and pi, not from 0.5 to 1");
        CHECK_FALSE(limited(JointType::Hinge, -1.0f, -0.5f).has_value());
        CHECK_FALSE(limited(JointType::Hinge, -4.0f, 1.0f).has_value());
        CHECK_FALSE(limited(JointType::Hinge, -1.0f, 4.0f).has_value());
        CHECK_FALSE(limited(JointType::Hinge, kNotANumber, 1.0f).has_value());
        CHECK(limited(JointType::Hinge, -kPi, kPi).has_value());
        // A hinge or a slider that may not move at all is no hinge and no slider -
        // unless it gives, and is a spring about where it is.
        CHECK(refusalOf(limited(JointType::Hinge, 0.0f, 0.0f)) ==
              "a hinge that is limited to where it is, and does not give, is a fixed joint: use one");
        CHECK_FALSE(limited(JointType::Slider, 0.0f, 0.0f).has_value());
        CHECK(physics->createJoint({.type = JointType::Hinge, .first = crate, .limited = true, .frequency = 2.0f})
                  .has_value());
        CHECK(refusalOf(limited(JointType::Slider, 0.5f, 1.0f)) ==
              "a slider slides from at most 0 to at least 0, not from 0.5 to 1");
        CHECK_FALSE(limited(JointType::Slider, -1.0f, -0.5f).has_value());
        CHECK(limited(JointType::Slider, -100.0f, 100.0f).has_value());
        CHECK(refusalOf(limited(JointType::Distance, -1.0f, 1.0f)) ==
              "the ends of a distance are from 0 or more to as much or more apart, not from -1 to 1");
        CHECK_FALSE(limited(JointType::Distance, 2.0f, 1.0f).has_value());
        CHECK(limited(JointType::Distance, 0.0f, 0.0f).has_value());
        // Limits that are not asked for are not looked at, and a joint without any has none.
        CHECK(physics->createJoint({.type = JointType::Hinge, .first = crate, .minimum = 9.0f, .maximum = -9.0f})
                  .has_value());
        CHECK(limited(JointType::Fixed, 9.0f, -9.0f).has_value());
        CHECK(limited(JointType::Ball, 9.0f, -9.0f).has_value());

        // A spring that swings less than never, or dies away faster than at once.
        CHECK_FALSE(
            physics->createJoint({.type = JointType::Distance, .first = crate, .frequency = -1.0f}).has_value());
        CHECK_FALSE(physics->createJoint({.type = JointType::Distance, .first = crate, .damping = -0.1f}).has_value());
        CHECK_FALSE(
            physics->createJoint({.type = JointType::Distance, .first = crate, .frequency = kNotANumber}).has_value());
    }

    TEST_CASE("a simulation is the same every time it is run") {
        const auto run = [] {
            const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
            addFloor(*physics);
            // A pile that topples: boxes put down off centre, a ball thrown into them.
            std::vector<BodyId> bodies;
            for (int level = 0; level < 6; ++level) {
                bodies.push_back(
                    unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 0.5f, 1.0f)}},
                                                .position = Vec3(0.3f * static_cast<float>(level % 2),
                                                                 0.25f + 0.5f * static_cast<float>(level), 0.0f)})));
            }
            const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.3f}},
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
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
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
                        ->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.5f}},
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
        const BodyId gone = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
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
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
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
        const BodyId gone = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
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
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere}}}));
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
