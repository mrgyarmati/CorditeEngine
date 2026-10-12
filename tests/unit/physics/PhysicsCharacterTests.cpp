#include "cordite/physics/PhysicsWorld.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace cordite;
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

/// A ramp six metres long and four wide, whose upper side begins on the floor at
/// x = 2 and rises from there towards +X at `degrees`.
BodyId addRamp(PhysicsWorld& physics, float degrees) {
    const Quat rotation = glm::angleAxis(glm::radians(degrees), Vec3(0.0f, 0.0f, 1.0f));
    return unwrap(physics.createBody({.motion = BodyMotion::Static,
                                      .shapes = {{.type = ShapeType::Box, .size = Vec3(6.0f, 0.2f, 4.0f)}},
                                      .position = Vec3(2.0f, 0.0f, 0.0f) + rotation * Vec3(3.0f, -0.1f, 0.0f),
                                      .rotation = rotation}));
}

/// Ground made of triangles: a strip eight metres long and two wide that begins where
/// x is nothing, a metre up - every second row of its corners `rise` higher, half a
/// metre on, so that it is flat for no rise and ridged otherwise.
BodyId addStrip(PhysicsWorld& physics, float rise) {
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    for (uint32_t column = 0; column <= 16; ++column) {
        const float height = 1.0f + (column % 2 == 0 ? 0.0f : rise);
        positions.emplace_back(0.5f * static_cast<float>(column), height, -1.0f);
        positions.emplace_back(0.5f * static_cast<float>(column), height, 1.0f);
    }
    for (uint32_t column = 0; column < 16; ++column) {
        const uint32_t corner = 2 * column;
        for (const uint32_t index : {corner, corner + 1, corner + 3, corner, corner + 3, corner + 2}) {
            indices.push_back(index);
        }
    }
    const GeometryId strip = unwrap(physics.createTriangleMesh(positions, indices));
    return unwrap(
        physics.createBody({.motion = BodyMotion::Static, .shapes = {{.type = ShapeType::Mesh, .geometry = strip}}}));
}

/// A block on the floor whose near side is at x = 2 and whose top is `height` up.
BodyId addBlock(PhysicsWorld& physics, float height) {
    return unwrap(physics.createBody({.motion = BodyMotion::Static,
                                      .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, height, 4.0f)}},
                                      .position = Vec3(3.0f, 0.5f * height, 0.0f)}));
}

} // namespace

TEST_SUITE("PhysicsWorld characters") {
    TEST_CASE("a character stands on the ground with its soles on it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const BodyId floor = addFloor(*physics);
        const CharacterId guard = unwrap(physics->createCharacter({.position = Vec3(1.0f, 0.0f, 2.0f)}));
        CHECK_FALSE(guard.isNull());
        CHECK(physics->isValid(guard));
        CHECK(physics->statistics().characters == 1);
        // Nothing is known of the ground until a step has looked.
        CHECK(physics->characterState(guard) == CharacterState{.position = Vec3(1.0f, 0.0f, 2.0f)});

        stepFor(*physics, 30);
        const CharacterState state = physics->characterState(guard);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.groundBody == floor);
        CHECK_NEAR(state.groundNormal, Vec3(0.0f, 1.0f, 0.0f), 1.0e-3f);
        CHECK_NEAR(state.position, Vec3(1.0f, 0.0f, 2.0f), 2.0e-3f);
        CHECK_NEAR(state.velocity, Vec3(0.0f), 1.0e-3f);
        // And it goes on standing there.
        stepFor(*physics, 300);
        CHECK_NEAR(physics->characterState(guard).position, Vec3(1.0f, 0.0f, 2.0f), 2.0e-3f);
    }

    TEST_CASE("a character in the air falls as everything falls, and lands") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const CharacterId falling = unwrap(physics->createCharacter({.position = Vec3(0.0f, 2.0f, 0.0f)}));
        const CharacterId light =
            unwrap(physics->createCharacter({.position = Vec3(3.0f, 2.0f, 0.0f), .gravityScale = 0.5f}));
        const CharacterId weightless =
            unwrap(physics->createCharacter({.position = Vec3(6.0f, 2.0f, 0.0f), .gravityScale = 0.0f}));

        // Half a second: thirty steps, each of which first speeds it up and then moves it.
        stepFor(*physics, 30);
        const float fallen = 9.81f * kStep * kStep * (30.0f * 31.0f / 2.0f);
        CharacterState state = physics->characterState(falling);
        CHECK(state.ground == GroundState::InAir);
        CHECK(state.groundBody.isNull());
        CHECK(state.groundNormal == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(state.position.y == doctest::Approx(2.0f - fallen).epsilon(0.001));
        CHECK(state.velocity.y == doctest::Approx(-9.81f * 0.5f).epsilon(0.001));
        CHECK(physics->characterState(light).position.y == doctest::Approx(2.0f - 0.5f * fallen).epsilon(0.001));
        CHECK(physics->characterState(weightless).position.y == doctest::Approx(2.0f));
        CHECK(physics->characterState(weightless).ground == GroundState::InAir);

        stepFor(*physics, 90);
        state = physics->characterState(falling);
        CHECK(state.ground == GroundState::OnGround);
        CHECK_NEAR(state.position, Vec3(0.0f), 2.0e-3f);
        CHECK_NEAR(state.velocity, Vec3(0.0f), 1.0e-3f);
        CHECK(physics->characterState(light).ground == GroundState::OnGround);
    }

    TEST_CASE("a character walks along the ground at the speed it is told to") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const CharacterId walker = unwrap(physics->createCharacter({}));
        stepFor(*physics, 2);

        // What of the speed points up is left out: it walks, it does not rise.
        physics->walk(walker, Vec3(2.0f, 5.0f, -1.0f));
        stepFor(*physics, 60);
        CharacterState state = physics->characterState(walker);
        CHECK_NEAR(state.position, Vec3(2.0f, 0.0f, -1.0f), 5.0e-3f);
        CHECK_NEAR(state.velocity, Vec3(2.0f, 0.0f, -1.0f), 1.0e-3f);
        CHECK(state.ground == GroundState::OnGround);

        // It goes on until it is told otherwise, and then stands at once.
        stepFor(*physics, 60);
        CHECK_NEAR(physics->characterState(walker).position, Vec3(4.0f, 0.0f, -2.0f), 1.0e-2f);
        physics->walk(walker, Vec3(0.0f));
        stepFor(*physics, 60);
        state = physics->characterState(walker);
        CHECK_NEAR(state.position, Vec3(4.0f, 0.0f, -2.0f), 1.0e-2f);
        CHECK_NEAR(state.velocity, Vec3(0.0f), 1.0e-3f);

        // A speed that is no number is not taken.
        physics->walk(walker, Vec3(1.0f, 0.0f, 0.0f));
        physics->walk(walker, Vec3(kNotANumber, 0.0f, 0.0f));
        stepFor(*physics, 60);
        CHECK_NEAR(physics->characterState(walker).position, Vec3(5.0f, 0.0f, -2.0f), 1.0e-2f);
    }

    TEST_CASE("a character walks up a slope that is not too steep for it, and not up one that is") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId ramp = addRamp(*physics, 30.0f);
        const CharacterId walker = unwrap(physics->createCharacter({}));
        physics->walk(walker, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 180);
        CharacterState state = physics->characterState(walker);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.groundBody == ramp);
        CHECK_NEAR(state.groundNormal, Vec3(-0.5f, std::sqrt(0.75f), 0.0f), 1.0e-2f);
        // It is on the ramp: as high as the ramp is where it stands, and well up it.
        CHECK(state.position.x > 4.0f);
        CHECK(state.position.y ==
              doctest::Approx((state.position.x - 2.0f) * std::tan(glm::radians(30.0f))).epsilon(0.03));
        // Told to stand, it stands: it does not creep down ground it can walk on.
        physics->walk(walker, Vec3(0.0f));
        stepFor(*physics, 10);
        const Vec3 stoodAt = physics->characterState(walker).position;
        stepFor(*physics, 300);
        CHECK_NEAR(physics->characterState(walker).position, stoodAt, 1.0e-3f);
        CHECK(physics->characterState(walker).velocity == Vec3(0.0f));
        // Nor does it drift downhill when it walks across the slope: a metre along it,
        // and no lower.
        physics->walk(walker, Vec3(0.0f, 0.0f, 1.0f));
        stepFor(*physics, 60);
        CHECK_NEAR(physics->characterState(walker).position, stoodAt + Vec3(0.0f, 0.0f, 1.0f), 5.0e-3f);
        CHECK(physics->characterState(walker).ground == GroundState::OnGround);
        // Downhill it stays on the ground all the way, and is not thrown off it.
        physics->walk(walker, Vec3(-4.0f, 0.0f, 0.0f));
        for (int step = 0; step < 30; ++step) {
            physics->step(kStep);
            REQUIRE(physics->characterState(walker).ground == GroundState::OnGround);
        }
        const Vec3 lower = physics->characterState(walker).position;
        CHECK(lower.x == doctest::Approx(stoodAt.x - 2.0f).epsilon(0.02));
        CHECK(lower.y == doctest::Approx((lower.x - 2.0f) * std::tan(glm::radians(30.0f))).epsilon(0.03));
        physics->destroyCharacter(walker);
        physics->destroyBody(ramp);

        // Sixty degrees are more than the fifty it walks on: it gets nowhere.
        addRamp(*physics, 60.0f);
        const CharacterId stopped = unwrap(physics->createCharacter({}));
        physics->walk(stopped, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 180);
        state = physics->characterState(stopped);
        CHECK(state.position.x < 2.3f);
        CHECK(state.position.y < 0.3f);
        // Unless it is one that walks on ground as steep as that.
        const CharacterId climber =
            unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 1.0f), .maxSlope = glm::radians(70.0f)}));
        physics->walk(climber, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 180);
        CHECK(physics->characterState(climber).position.y > 1.0f);
    }

    TEST_CASE("a character is put down on steep ground, and slides off it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        addRamp(*physics, 60.0f);
        // On the ramp, two metres up it.
        const Vec3 onRamp = Vec3(2.0f, 0.0f, 0.0f) +
                            glm::angleAxis(glm::radians(60.0f), Vec3(0.0f, 0.0f, 1.0f)) * Vec3(2.0f, 0.0f, 0.0f);
        const CharacterId slider = unwrap(physics->createCharacter({.position = onRamp}));
        stepFor(*physics, 3);
        CHECK(physics->characterState(slider).ground == GroundState::OnSteepGround);
        CHECK_NEAR(physics->characterState(slider).groundNormal, Vec3(-std::sqrt(0.75f), 0.5f, 0.0f), 1.0e-2f);
        stepFor(*physics, 180);
        const CharacterState state = physics->characterState(slider);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.position.y == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(state.position.x < 2.0f);
    }

    TEST_CASE("a character walks up a step that is no higher than it climbs, and is stopped by a higher one") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId low = addBlock(*physics, 0.3f);
        const CharacterId walker = unwrap(physics->createCharacter({}));
        physics->walk(walker, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 120);
        CharacterState state = physics->characterState(walker);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.groundBody == low);
        CHECK(state.position.x > 3.0f);
        CHECK(state.position.y == doctest::Approx(0.3f).epsilon(0.01));
        // And down again at its far side, without leaving the ground for long.
        stepFor(*physics, 90);
        state = physics->characterState(walker);
        CHECK(state.position.x > 4.3f);
        CHECK(state.position.y == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(state.ground == GroundState::OnGround);
        physics->destroyCharacter(walker);
        physics->destroyBody(low);

        // Half a metre is more than the 35 centimetres it climbs: it stands in front of
        // it, as far from it as it is around.
        const BodyId high = addBlock(*physics, 0.5f);
        const CharacterId stopped = unwrap(physics->createCharacter({}));
        physics->walk(stopped, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 120);
        state = physics->characterState(stopped);
        CHECK(state.position.x == doctest::Approx(1.7f).epsilon(0.01));
        CHECK(state.position.y == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(state.ground == GroundState::OnGround);
        // One that climbs higher gets up it, and one that climbs nothing not even up the low one.
        const CharacterId climber =
            unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 1.0f), .stepHeight = 0.6f}));
        physics->walk(climber, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 120);
        CHECK(physics->characterState(climber).position.y == doctest::Approx(0.5f).epsilon(0.01));
        physics->destroyBody(high);
        physics->destroyCharacter(stopped);
        physics->destroyCharacter(climber);
        addBlock(*physics, 0.3f);
        const CharacterId shuffler = unwrap(physics->createCharacter({.stepHeight = 0.0f}));
        physics->walk(shuffler, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 120);
        CHECK(physics->characterState(shuffler).position.x == doctest::Approx(1.7f).epsilon(0.01));
        CHECK(physics->characterState(shuffler).position.y == doctest::Approx(0.0f).epsilon(0.01));
    }

    TEST_CASE("a character that stands jumps, and one in the air does not") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const CharacterId jumper = unwrap(physics->createCharacter({}));
        stepFor(*physics, 5);

        physics->jump(jumper, 4.0f);
        physics->step(kStep);
        CharacterState state = physics->characterState(jumper);
        CHECK(state.ground == GroundState::InAir);
        CHECK(state.velocity.y == doctest::Approx(4.0f - 9.81f * kStep).epsilon(0.001));
        // As high as that speed takes it: v * v / 2 g, less what the steps cut off.
        float highest = state.position.y;
        for (int step = 0; step < 120; ++step) {
            physics->step(kStep);
            highest = std::max(highest, physics->characterState(jumper).position.y);
        }
        CHECK(highest == doctest::Approx(4.0f * 4.0f / (2.0f * 9.81f)).epsilon(0.05));
        state = physics->characterState(jumper);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.position.y == doctest::Approx(0.0f).epsilon(0.01));

        // Asked once, it jumps once.
        stepFor(*physics, 60);
        CHECK(physics->characterState(jumper).position.y == doctest::Approx(0.0f).epsilon(0.01));

        // A jump that is asked for in the air is forgotten: it does not jump when it lands.
        physics->placeCharacter(jumper, Vec3(0.0f, 1.0f, 0.0f));
        physics->step(kStep);
        CHECK(physics->characterState(jumper).ground == GroundState::InAir);
        physics->jump(jumper, 4.0f);
        physics->step(kStep);
        CHECK(physics->characterState(jumper).velocity.y < 0.0f);
        highest = 0.0f;
        for (int step = 0; step < 120; ++step) {
            physics->step(kStep);
            if (physics->characterState(jumper).ground == GroundState::OnGround) {
                highest = std::max(highest, physics->characterState(jumper).position.y);
            }
        }
        CHECK(physics->characterState(jumper).ground == GroundState::OnGround);
        CHECK(highest < 0.01f);

        // A request can be taken back, and one that is no number is none.
        physics->jump(jumper, 4.0f);
        physics->jump(jumper, 0.0f);
        physics->step(kStep);
        CHECK(physics->characterState(jumper).ground == GroundState::OnGround);
        physics->jump(jumper, kNotANumber);
        physics->jump(jumper, -3.0f);
        physics->step(kStep);
        CHECK(physics->characterState(jumper).ground == GroundState::OnGround);
        CHECK(physics->characterState(jumper).position.y == doctest::Approx(0.0f).epsilon(0.01));
    }

    TEST_CASE("a character that jumps against a ceiling comes down from it at once") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A ceiling two metres up: twenty centimetres above its head.
        unwrap(physics->createBody({.motion = BodyMotion::Static,
                                    .shapes = {{.type = ShapeType::Box, .size = Vec3(4.0f, 0.5f, 4.0f)}},
                                    .position = Vec3(0.0f, 2.25f, 0.0f)}));
        const CharacterId jumper = unwrap(physics->createCharacter({}));
        stepFor(*physics, 5);
        // A jump that would take it eighty centimetres up.
        physics->jump(jumper, 4.0f);
        float highest = 0.0f;
        int landedAfter = 0;
        for (int step = 1; step <= 60 && landedAfter == 0; ++step) {
            physics->step(kStep);
            const CharacterState state = physics->characterState(jumper);
            highest = std::max(highest, state.position.y);
            if (state.ground == GroundState::OnGround) {
                landedAfter = step;
            }
        }
        // As high as the ceiling lets it, and no higher.
        CHECK(highest == doctest::Approx(0.2f).epsilon(0.02));
        // It is up there after four steps, and falls the twenty centimetres in a fifth of
        // a second: it does not hang under the ceiling for the four tenths of a second
        // that its speed upwards would have lasted.
        CHECK(landedAfter > 0);
        CHECK(landedAfter <= 20);
    }

    TEST_CASE("what has come to rest on a character does not pin it down, and falls when the character goes") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A character on a block a metre high, and a crate that is dropped on its head.
        const BodyId block =
            unwrap(physics->createBody({.motion = BodyMotion::Static,
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f, 1.0f, 2.0f)}},
                                        .position = Vec3(0.0f, 0.5f, 0.0f)}));
        const CharacterId porter = unwrap(physics->createCharacter({.position = Vec3(0.0f, 1.0f, 0.0f)}));
        const BodyId crate = unwrap(physics->createBody(
            {.shapes = {{.type = ShapeType::Box, .size = Vec3(0.5f)}}, .position = Vec3(0.0f, 3.2f, 0.0f)}));
        // It lands on the body of the character - which is a tenth smaller than the
        // character - and falls asleep there.
        const float onHead = 0.9f + 0.9f * 0.9f + 0.25f;
        int steps = 0;
        while (physics->isActive(crate) && steps < 600) {
            physics->step(kStep);
            ++steps;
        }
        REQUIRE_FALSE(physics->isActive(crate));
        CHECK(physics->position(crate).y == doctest::Approx(1.0f + onHead).epsilon(0.01));
        CHECK(physics->characterState(porter).ground == GroundState::OnGround);
        CHECK_NEAR(physics->characterState(porter).position, Vec3(0.0f, 1.0f, 0.0f), 5.0e-3f);

        // The block goes, and the character falls to the floor - straight down, from
        // under the crate, without touching it. The crate notices all the same, and
        // comes down on its head again.
        physics->destroyBody(block);
        stepFor(*physics, 180);
        CHECK(physics->characterState(porter).ground == GroundState::OnGround);
        CHECK_NEAR(physics->characterState(porter).position, Vec3(0.0f), 5.0e-3f);
        CHECK(physics->position(crate).y == doctest::Approx(onHead).epsilon(0.02));

        // The character is not pinned to the spot by what lies on it. It darts off - a
        // metre in one step, so that the crate, asleep again, is not touched by anything
        // as it goes - and the crate falls to the floor.
        steps = 0;
        while (physics->isActive(crate) && steps < 600) {
            physics->step(kStep);
            ++steps;
        }
        REQUIRE_FALSE(physics->isActive(crate));
        physics->walk(porter, Vec3(60.0f, 0.0f, 0.0f));
        physics->step(kStep);
        CHECK(physics->characterState(porter).position.x == doctest::Approx(1.0f).epsilon(0.01));
        physics->walk(porter, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 90);
        CHECK(physics->characterState(porter).position.x == doctest::Approx(4.0f).epsilon(0.01));
        CHECK(physics->position(crate).y == doctest::Approx(0.25f).epsilon(0.05));

        // What comes at it from the side is another matter: that it runs into, and pushes.
        const BodyId ball = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.25f}},
                                                        .position = Vec3(6.0f, 1.2f, 0.0f),
                                                        .gravityScale = 0.0f}));
        stepFor(*physics, 120);
        CHECK(physics->position(ball).x > 6.5f);
        CHECK(physics->position(ball).x - physics->characterState(porter).position.x > 0.5f);
    }

    TEST_CASE("a character falls when what it stood on is gone") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId block = unwrap(physics->createBody({.motion = BodyMotion::Static,
                                                         .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f)}},
                                                         .position = Vec3(0.0f, 1.0f, 0.0f)}));
        const CharacterId guard = unwrap(physics->createCharacter({.position = Vec3(0.0f, 2.0f, 0.0f)}));
        stepFor(*physics, 30);
        CHECK(physics->characterState(guard).groundBody == block);
        CHECK_NEAR(physics->characterState(guard).position, Vec3(0.0f, 2.0f, 0.0f), 2.0e-3f);

        physics->destroyBody(block);
        physics->step(kStep);
        CHECK(physics->characterState(guard).ground == GroundState::InAir);
        CHECK(physics->characterState(guard).groundBody.isNull());
        stepFor(*physics, 90);
        CHECK(physics->characterState(guard).ground == GroundState::OnGround);
        CHECK_NEAR(physics->characterState(guard).position, Vec3(0.0f), 2.0e-3f);
    }

    TEST_CASE("a character walks over ground of triangles without stumbling over where they meet") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        // A strip a metre up, eight metres long: sixteen triangles, an edge between two
        // every half metre.
        const BodyId ground = addStrip(*physics, 0.0f);
        const CharacterId walker = unwrap(physics->createCharacter({.position = Vec3(1.0f, 1.0f, 0.3f)}));
        physics->step(kStep);
        // Not along the strip alone: across the diagonals as well.
        physics->walk(walker, Vec3(3.0f, 0.0f, -0.25f));
        for (int step = 1; step <= 100; ++step) {
            physics->step(kStep);
            const CharacterState state = physics->characterState(walker);
            REQUIRE(state.ground == GroundState::OnGround);
            REQUIRE(state.groundBody == ground);
            // Every step takes it as far as every other, and none up or down.
            REQUIRE(state.velocity.x == doctest::Approx(3.0f).epsilon(0.001));
            REQUIRE(state.position.y == doctest::Approx(1.0f).epsilon(0.001));
        }
        CHECK_NEAR(physics->characterState(walker).position, Vec3(6.0f, 1.0f, 0.3f - 100.0f * kStep * 0.25f), 5.0e-3f);
        physics->destroyCharacter(walker);
        physics->destroyBody(ground);

        // The same strip in ridges of five centimetres: up and down, half a metre each.
        // It goes up and down them, and is slower by what a slope like that takes - the
        // speed along the ground is the speed along the slope, seen from above - but by
        // no more: the edge in a furrow is nothing that it runs into.
        addStrip(*physics, 0.05f);
        const CharacterId hiker = unwrap(physics->createCharacter({.position = Vec3(1.0f, 1.05f, 0.3f)}));
        stepFor(*physics, 10);
        physics->walk(hiker, Vec3(3.0f, 0.0f, 0.0f));
        const float slope = std::atan(0.05f / 0.5f);
        const float onSlope = 3.0f * std::cos(slope) * std::cos(slope);
        for (int step = 1; step <= 120; ++step) {
            physics->step(kStep);
            const CharacterState state = physics->characterState(hiker);
            REQUIRE(state.ground == GroundState::OnGround);
            REQUIRE(state.velocity.x > onSlope - 0.005f);
            REQUIRE(state.velocity.x < 3.001f);
        }
    }

    TEST_CASE("in the air a character goes sideways as it left the ground, as far as it has no say there") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // Three that run and jump, and are told to stand while they are in the air.
        const CharacterId steered = unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 0.0f)}));
        const CharacterId half =
            unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 2.0f), .airControl = 0.5f}));
        const CharacterId thrown =
            unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 4.0f), .airControl = 0.0f}));
        const CharacterId all[] = {steered, half, thrown};
        for (const CharacterId character : all) {
            physics->walk(character, Vec3(3.0f, 0.0f, 0.0f));
        }
        stepFor(*physics, 30);
        for (const CharacterId character : all) {
            physics->jump(character, 5.0f);
        }
        physics->step(kStep);
        for (const CharacterId character : all) {
            physics->walk(character, Vec3(0.0f));
        }
        physics->step(kStep);
        // All of the say: it stops going sideways at once. None: it goes on as it left.
        CHECK(physics->characterState(steered).velocity.x == doctest::Approx(0.0f));
        CHECK(physics->characterState(half).velocity.x == doctest::Approx(1.5f));
        CHECK(physics->characterState(thrown).velocity.x == doctest::Approx(3.0f));
        // And so on, however many steps that is: a share of the say is no brake.
        stepFor(*physics, 20);
        CHECK(physics->characterState(half).ground == GroundState::InAir);
        CHECK(physics->characterState(half).velocity.x == doctest::Approx(1.5f));
        CHECK(physics->characterState(thrown).velocity.x == doctest::Approx(3.0f));
        // Back on the ground they stand, as they were told.
        stepFor(*physics, 120);
        for (const CharacterId character : all) {
            CHECK(physics->characterState(character).ground == GroundState::OnGround);
            CHECK_NEAR(physics->characterState(character).velocity, Vec3(0.0f), 1.0e-3f);
        }
    }

    TEST_CASE("a character pushes what it can move, and is stopped by what it cannot") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A hundred newtons move a crate of five kilograms over this floor, and not one of a ton.
        const BodyId light = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(0.6f)}},
                                                         .position = Vec3(1.5f, 0.3f, 0.0f),
                                                         .mass = 5.0f}));
        const BodyId heavy = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(0.6f)}},
                                                         .position = Vec3(1.5f, 0.3f, 4.0f),
                                                         .mass = 1000.0f}));
        const CharacterId pusher = unwrap(physics->createCharacter({}));
        const CharacterId stopped = unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 4.0f)}));
        physics->walk(pusher, Vec3(1.0f, 0.0f, 0.0f));
        physics->walk(stopped, Vec3(1.0f, 0.0f, 0.0f));
        stepFor(*physics, 240);

        CHECK(physics->position(light).x > 2.5f);
        CHECK(physics->characterState(pusher).position.x > 1.5f);
        // The crate is ahead of what pushes it: nothing has gone through anything.
        CHECK(physics->position(light).x - physics->characterState(pusher).position.x > 0.55f);

        CHECK(physics->position(heavy).x == doctest::Approx(1.5f).epsilon(0.01));
        // It stands in front of the crate: as far from its near side as it is around.
        CHECK(physics->characterState(stopped).position.x == doctest::Approx(1.2f - 0.3f).epsilon(0.02));
        CHECK(physics->characterState(stopped).ground == GroundState::OnGround);

        // With more force, the heavy one moves as well.
        const CharacterId strong =
            unwrap(physics->createCharacter({.position = Vec3(-2.0f, 0.0f, 4.0f), .pushForce = 20000.0f}));
        physics->destroyCharacter(stopped);
        physics->walk(strong, Vec3(1.0f, 0.0f, 0.0f));
        stepFor(*physics, 420);
        CHECK(physics->position(heavy).x > 2.0f);
    }

    TEST_CASE("a character is carried by what it stands on") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A platform whose top is a metre up, moved by hand.
        const BodyId platform =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(4.0f, 0.2f, 4.0f)}},
                                        .position = Vec3(0.0f, 0.9f, 0.0f)}));
        const CharacterId rider = unwrap(physics->createCharacter({.position = Vec3(0.0f, 1.0f, 0.0f)}));
        stepFor(*physics, 5);
        CHECK(physics->characterState(rider).groundBody == platform);

        // A metre a second to the side, and half a metre a second up, for two seconds.
        for (int step = 1; step <= 120; ++step) {
            const float seconds = static_cast<float>(step) * kStep;
            physics->moveKinematic(platform, Vec3(seconds, 0.9f + 0.5f * seconds, 0.0f), kIdentityQuat, kStep);
            physics->step(kStep);
        }
        CharacterState state = physics->characterState(rider);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.groundBody == platform);
        CHECK_NEAR(state.position, Vec3(2.0f, 2.0f, 0.0f), 0.05f);
        CHECK_NEAR(state.velocity, Vec3(1.0f, 0.5f, 0.0f), 0.02f);

        // It walks on the platform as it walks on the ground: its own speed on top.
        physics->walk(rider, Vec3(0.0f, 0.0f, 1.0f));
        for (int step = 121; step <= 180; ++step) {
            const float seconds = static_cast<float>(step) * kStep;
            physics->moveKinematic(platform, Vec3(seconds, 0.9f + 0.5f * seconds, 0.0f), kIdentityQuat, kStep);
            physics->step(kStep);
        }
        state = physics->characterState(rider);
        CHECK_NEAR(state.position, Vec3(3.0f, 2.5f, 1.0f), 0.05f);

        // On a body that the simulation moves it stands as well, and presses on it.
        physics->destroyCharacter(rider);
        const BodyId crate = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f)}},
                                                         .position = Vec3(-10.0f, 0.5f, 0.0f),
                                                         .mass = 200.0f}));
        const CharacterId stander = unwrap(physics->createCharacter({.position = Vec3(-10.0f, 1.0f, 0.0f)}));
        stepFor(*physics, 120);
        state = physics->characterState(stander);
        CHECK(state.ground == GroundState::OnGround);
        CHECK(state.groundBody == crate);
        CHECK(state.position.y == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(physics->position(crate).y == doctest::Approx(0.5f).epsilon(0.02));
    }

    TEST_CASE("a body stands for a character: rays find it, and sensors notice it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId gate =
            unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 2.0f, 4.0f)}},
                                        .position = Vec3(3.0f, 1.0f, 0.0f),
                                        .sensor = true,
                                        .userData = 100}));
        const CharacterId guard = unwrap(physics->createCharacter({.userData = 77}));
        const BodyId body = physics->characterBody(guard);
        REQUIRE(physics->isValid(body));
        CHECK(physics->characterOf(body) == guard);
        CHECK(physics->characterOf(gate).isNull());
        CHECK(physics->userData(body) == 77);
        CHECK(physics->motion(body) == BodyMotion::Kinematic);
        CHECK_FALSE(physics->isSensor(body));
        // The floor, the gate and the body of the character.
        CHECK(physics->statistics().bodies == 3);

        // A ray from above meets its head: a tenth below where the head of the character
        // is, because its body is that much smaller than it.
        std::optional<RayHit> hit = physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == body);
        CHECK(hit->userData == 77);
        CHECK(hit->distance == doctest::Approx(10.0f - (0.9f + 0.9f * 0.9f)).epsilon(0.001));
        CHECK(physics->overlapSphere(Vec3(0.0f, 0.9f, 0.0f), 0.1f) == std::vector<BodyId>{body});

        // It walks through the gate, which stops nothing: the gate says that it came, and that it went.
        physics->walk(guard, Vec3(2.0f, 0.0f, 0.0f));
        std::vector<ContactEvent> events;
        for (int step = 0; step < 180; ++step) {
            physics->step(kStep);
            const std::span<const ContactEvent> said = physics->contactEvents();
            events.insert(events.end(), said.begin(), said.end());
        }
        CHECK(physics->characterState(guard).position.x > 5.0f);
        REQUIRE(events.size() == 2);
        CHECK(events[0].phase == ContactPhase::Began);
        CHECK(events[0].first == gate);
        CHECK(events[0].second == body);
        CHECK(events[0].secondUserData == 77);
        CHECK(events[0].sensor);
        CHECK(events[1].phase == ContactPhase::Ended);
        CHECK(events[1].first == gate);
        CHECK(events[1].second == body);
        // Where the character is, its body is: around the same middle.
        CHECK(physics->overlapSphere(physics->characterState(guard).position + Vec3(0.0f, 0.9f, 0.0f), 0.1f) ==
              std::vector<BodyId>{body});

        // The body is the character's: it is where the character is, and nobody else
        // puts it anywhere or gives it a speed.
        const Vec3 stands = physics->characterState(guard).position;
        physics->walk(guard, Vec3(0.0f));
        physics->setPlacement(body, Vec3(0.0f, 20.0f, 0.0f), kIdentityQuat);
        physics->moveKinematic(body, Vec3(0.0f, 20.0f, 0.0f), kIdentityQuat, kStep);
        physics->setLinearVelocity(body, Vec3(0.0f, 9.0f, 0.0f));
        physics->setAngularVelocity(body, Vec3(0.0f, 9.0f, 0.0f));
        CHECK(physics->linearVelocity(body) == Vec3(0.0f));
        CHECK(physics->angularVelocity(body) == Vec3(0.0f));
        stepFor(*physics, 3);
        CHECK_NEAR(physics->characterState(guard).position, stands, 2.0e-3f);
        CHECK(physics->overlapSphere(stands + Vec3(0.0f, 0.9f, 0.0f), 0.1f) == std::vector<BodyId>{body});
        // And it does not go by itself.
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS(physics->destroyBody(body), test::AssertionFailure);
#else
        physics->destroyBody(body);
#endif
        CHECK(physics->isValid(body));
        CHECK(physics->isValid(guard));

        // With the character it does. An id that is no character's is ignored.
        physics->destroyCharacter(guard);
        CHECK_FALSE(physics->isValid(guard));
        CHECK_FALSE(physics->isValid(body));
        CHECK(physics->characterOf(body).isNull());
        CHECK(physics->statistics().characters == 0);
        CHECK(physics->statistics().bodies == 2);
        physics->destroyCharacter(guard);
        physics->destroyCharacter(CharacterId());
        CHECK(physics->statistics().characters == 0);
    }

    TEST_CASE("a character that goes while it is in a sensor is said to have left it") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const BodyId gate = unwrap(physics->createBody({.motion = BodyMotion::Kinematic,
                                                        .shapes = {{.type = ShapeType::Box, .size = Vec3(2.0f)}},
                                                        .position = Vec3(0.0f, 1.0f, 0.0f),
                                                        .sensor = true}));
        const CharacterId guard = unwrap(physics->createCharacter({.userData = 5}));
        const BodyId body = physics->characterBody(guard);
        physics->step(kStep);
        REQUIRE(physics->contactEvents().size() == 1);
        CHECK(physics->contactEvents()[0].phase == ContactPhase::Began);
        physics->step(kStep);
        CHECK(physics->contactEvents().empty());

        physics->destroyCharacter(guard);
        physics->step(kStep);
        REQUIRE(physics->contactEvents().size() == 1);
        CHECK(physics->contactEvents()[0].phase == ContactPhase::Ended);
        CHECK(physics->contactEvents()[0].first == gate);
        CHECK(physics->contactEvents()[0].second == body);
        CHECK(physics->contactEvents()[0].secondUserData == 5);
    }

    TEST_CASE("a character is put somewhere else at once, and given a speed") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const CharacterId guard = unwrap(physics->createCharacter({.airControl = 0.0f}));
        stepFor(*physics, 5);
        physics->placeCharacter(guard, Vec3(10.0f, 0.0f, -3.0f));
        CHECK(physics->characterState(guard).position == Vec3(10.0f, 0.0f, -3.0f));
        // Its body has come along.
        CHECK(physics->overlapSphere(Vec3(10.0f, 0.9f, -3.0f), 0.1f) ==
              std::vector<BodyId>{physics->characterBody(guard)});
        CHECK(physics->overlapSphere(Vec3(0.0f, 0.9f, 0.0f), 0.1f).empty());
        stepFor(*physics, 5);
        CHECK(physics->characterState(guard).ground == GroundState::OnGround);
        CHECK_NEAR(physics->characterState(guard).position, Vec3(10.0f, 0.0f, -3.0f), 2.0e-3f);
        // A place that is none is no place.
        physics->placeCharacter(guard, Vec3(0.0f, kNotANumber, 0.0f));
        CHECK_NEAR(physics->characterState(guard).position, Vec3(10.0f, 0.0f, -3.0f), 2.0e-3f);

        // A blow: up and to the side. It leaves the ground, and goes sideways as it was
        // thrown, having no say in the air.
        physics->setCharacterVelocity(guard, Vec3(2.0f, 5.0f, 0.0f));
        physics->setCharacterVelocity(guard, Vec3(kNotANumber));
        physics->step(kStep);
        CharacterState state = physics->characterState(guard);
        CHECK(state.ground == GroundState::InAir);
        CHECK(state.velocity.x == doctest::Approx(2.0f));
        CHECK(state.velocity.y == doctest::Approx(5.0f - 9.81f * kStep).epsilon(0.001));
        stepFor(*physics, 30);
        CHECK(physics->characterState(guard).velocity.x == doctest::Approx(2.0f));
        // When it has landed it stands, as it was never told to walk.
        stepFor(*physics, 120);
        state = physics->characterState(guard);
        CHECK(state.ground == GroundState::OnGround);
        CHECK_NEAR(state.velocity, Vec3(0.0f), 1.0e-3f);
        CHECK(state.position.x == doctest::Approx(10.0f + 2.0f * (2.0f * 5.0f / 9.81f)).epsilon(0.03));
    }

    TEST_CASE("characters do not walk through each other, or through what stands still") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        const CharacterId standing = unwrap(physics->createCharacter({.position = Vec3(2.0f, 0.0f, 0.0f)}));
        const CharacterId walking = unwrap(physics->createCharacter({}));
        physics->walk(walking, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 180);
        // The one that stands is not pushed away, and the other stands in front of it.
        CHECK(physics->characterState(standing).position.x == doctest::Approx(2.0f).epsilon(0.01));
        const float between =
            physics->characterState(standing).position.x - physics->characterState(walking).position.x;
        CHECK(between > 0.5f);
        CHECK(between < 0.65f);

        // A wall of triangles stops it from either side, as it stops everything.
        const std::vector<Vec3> corners = {Vec3(0.0f, 0.0f, -4.0f), Vec3(0.0f, 0.0f, 4.0f), Vec3(0.0f, 4.0f, 4.0f),
                                           Vec3(0.0f, 4.0f, -4.0f)};
        const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
        const GeometryId wall = unwrap(physics->createTriangleMesh(corners, indices));
        unwrap(physics->createBody({.motion = BodyMotion::Static,
                                    .shapes = {{.type = ShapeType::Mesh, .geometry = wall}},
                                    .position = Vec3(-3.0f, 0.0f, 0.0f)}));
        physics->walk(walking, Vec3(-2.0f, 0.0f, 0.0f));
        stepFor(*physics, 240);
        CHECK(physics->characterState(walking).position.x == doctest::Approx(-2.7f).epsilon(0.01));
        const CharacterId behind = unwrap(physics->createCharacter({.position = Vec3(-5.0f, 0.0f, 0.0f)}));
        physics->walk(behind, Vec3(2.0f, 0.0f, 0.0f));
        stepFor(*physics, 240);
        CHECK(physics->characterState(behind).position.x == doctest::Approx(-3.3f).epsilon(0.01));
    }

    TEST_CASE("a character is a capsule as large as it was asked for") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        addFloor(*physics);
        // A ceiling two metres up, and a beam under it that leaves a metre and a half.
        unwrap(physics->createBody({.motion = BodyMotion::Static,
                                    .shapes = {{.type = ShapeType::Box, .size = Vec3(1.0f, 0.5f, 8.0f)}},
                                    .position = Vec3(3.0f, 1.75f, 0.0f)}));
        // One of 1.8 metres does not get under the beam; one of 1.4 does.
        const CharacterId tall = unwrap(physics->createCharacter({}));
        const CharacterId small =
            unwrap(physics->createCharacter({.radius = 0.2f, .height = 1.4f, .position = Vec3(0.0f, 0.0f, 2.0f)}));
        // And one that is no taller than it is wide is a ball: it is its radius that counts.
        const CharacterId ball =
            unwrap(physics->createCharacter({.radius = 0.5f, .height = 0.2f, .position = Vec3(0.0f, 0.0f, -2.0f)}));
        for (const CharacterId character : {tall, small, ball}) {
            physics->walk(character, Vec3(2.0f, 0.0f, 0.0f));
        }
        stepFor(*physics, 240);
        CHECK(physics->characterState(tall).position.x == doctest::Approx(2.5f - 0.3f).epsilon(0.02));
        CHECK(physics->characterState(small).position.x > 6.0f);
        CHECK(physics->characterState(ball).position.x > 6.0f);
        CHECK(physics->characterState(ball).position.y == doctest::Approx(0.0f).epsilon(0.01));
        // The ball is a metre tall: a ray from above meets its body nine tenths of that up.
        const Vec3 at = physics->characterState(ball).position;
        const std::optional<RayHit> hit =
            physics->raycast(Ray{Vec3(at.x, 10.0f, at.z), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(hit->body == physics->characterBody(ball));
        CHECK(hit->position.y == doctest::Approx(0.5f + 0.9f * 0.5f).epsilon(0.01));
    }

    TEST_CASE("what no character can be made of is refused") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const std::vector<CharacterDesc> refused = {
            {.radius = 0.01f},
            {.radius = -1.0f},
            {.radius = kNotANumber},
            {.height = 0.0f},
            {.height = kNotANumber},
            {.position = Vec3(0.0f, kNotANumber, 0.0f)},
            {.mass = 0.0f},
            {.mass = kNotANumber},
            {.maxSlope = -0.1f},
            {.maxSlope = kHalfPi},
            {.maxSlope = kNotANumber},
            {.stepHeight = -0.1f},
            {.stepHeight = kNotANumber},
            {.pushForce = -1.0f},
            {.pushForce = kNotANumber},
            {.gravityScale = kNotANumber},
            {.airControl = -0.1f},
            {.airControl = 1.1f},
            {.airControl = kNotANumber},
        };
        for (size_t index = 0; index < refused.size(); ++index) {
            CAPTURE(index);
            const Result<CharacterId> made = physics->createCharacter(refused[index]);
            REQUIRE_FALSE(made.has_value());
            CHECK(made.error().code == ErrorCode::InvalidArgument);
        }
        CHECK(physics->createCharacter({.radius = 0.01f}).error().message ==
              "a character is at least 0.05 m around and has a height, not 0.01 m around and 1.8 m tall");
        CHECK(physics->statistics().characters == 0);
        CHECK(physics->statistics().bodies == 0);
        // The smallest there is, the steepest that is not upright, and nothing at all to push with.
        CHECK(physics->createCharacter({.radius = kMinCharacterRadius, .height = 0.01f}).has_value());
        CHECK(physics->createCharacter({.maxSlope = 0.0f, .stepHeight = 0.0f, .pushForce = 0.0f}).has_value());

        // A world that is full has no room for the body of one more.
        const std::unique_ptr<PhysicsWorld> small = unwrap(PhysicsWorld::create({.maxBodies = 2}));
        addFloor(*small);
        const CharacterId fits = unwrap(small->createCharacter({}));
        const Result<CharacterId> oneTooMany = small->createCharacter({.position = Vec3(3.0f, 0.0f, 0.0f)});
        REQUIRE_FALSE(oneTooMany.has_value());
        CHECK(oneTooMany.error().code == ErrorCode::OutOfRange);
        CHECK(oneTooMany.error().message == "the physics world is full: it holds 2 bodies, and a character needs one");
        CHECK(small->statistics().characters == 1);
        // When one goes, there is room again - and its id is not given to the next.
        small->destroyCharacter(fits);
        const CharacterId next = unwrap(small->createCharacter({}));
        CHECK(next != fits);
        CHECK_FALSE(small->isValid(fits));
    }

    TEST_CASE("the ground states have the names tools use") {
        CHECK(nameOf(GroundState::OnGround) == "onGround");
        CHECK(nameOf(GroundState::OnSteepGround) == "onSteepGround");
        CHECK(nameOf(GroundState::InAir) == "inAir");
        CHECK(kGroundStates.size() == 3);
    }

    TEST_CASE("questions about a character that is none are mistakes") {
        const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
        const CharacterId none{12345};
        CHECK_FALSE(physics->isValid(none));
        CHECK_FALSE(physics->isValid(CharacterId()));
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)physics->characterState(none), test::AssertionFailure);
        CHECK_THROWS_AS((void)physics->characterBody(none), test::AssertionFailure);
        CHECK_THROWS_AS(physics->walk(none, Vec3(1.0f, 0.0f, 0.0f)), test::AssertionFailure);
        CHECK_THROWS_AS(physics->jump(none, 1.0f), test::AssertionFailure);
        CHECK_THROWS_AS(physics->placeCharacter(none, Vec3(0.0f)), test::AssertionFailure);
        CHECK_THROWS_AS(physics->setCharacterVelocity(none, Vec3(0.0f)), test::AssertionFailure);
#else
        CHECK(physics->characterState(none) == CharacterState{});
        CHECK(physics->characterBody(none).isNull());
        physics->walk(none, Vec3(1.0f, 0.0f, 0.0f));
        physics->jump(none, 1.0f);
        physics->placeCharacter(none, Vec3(0.0f));
        physics->setCharacterVelocity(none, Vec3(0.0f));
        physics->step(kStep);
        CHECK(physics->statistics().characters == 0);
#endif
    }

    TEST_CASE("two worlds with the same characters, told the same, end up the same") {
        const auto run = [] {
            const std::unique_ptr<PhysicsWorld> physics = unwrap(PhysicsWorld::create());
            addFloor(*physics);
            addRamp(*physics, 25.0f);
            const BodyId crate = unwrap(physics->createBody({.shapes = {{.type = ShapeType::Box, .size = Vec3(0.6f)}},
                                                             .position = Vec3(1.0f, 0.3f, 1.0f),
                                                             .mass = 5.0f}));
            const CharacterId first = unwrap(physics->createCharacter({}));
            const CharacterId second = unwrap(physics->createCharacter({.position = Vec3(0.0f, 0.0f, 1.0f)}));
            physics->walk(first, Vec3(1.5f, 0.0f, 0.2f));
            physics->walk(second, Vec3(1.0f, 0.0f, 0.0f));
            std::vector<Vec3> seen;
            for (int step = 0; step < 240; ++step) {
                if (step == 60) {
                    physics->jump(first, 3.0f);
                }
                physics->step(kStep);
            }
            seen.push_back(physics->characterState(first).position);
            seen.push_back(physics->characterState(second).position);
            seen.push_back(physics->position(crate));
            return seen;
        };
        const std::vector<Vec3> once = run();
        const std::vector<Vec3> again = run();
        // To the bit.
        CHECK(once == again);
        // And something happened: the crate was pushed.
        CHECK(once[2].x > 1.2f);
    }
}
