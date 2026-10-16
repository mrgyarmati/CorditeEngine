#include "cordite/renderer/Decals.h"

#include "cordite/math/Transform.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <set>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// Turns an entity's -Z axis straight down, its +Y axis to -Z of the world.
Quat lookingDown() {
    return lookRotation(Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));
}

/// A unit vector `degrees` away from straight up, towards +X.
Vec3 tiltedBy(float degrees) {
    const float angle = glm::radians(degrees);
    return Vec3(std::sin(angle), std::cos(angle), 0.0f);
}

/// A decal entity of `world` with a material, about the origin, projected straight down.
Entity addDecal(World& world, int32_t order = 0) {
    const Entity entity = world.createEntity("Decal");
    world.transform(entity).rotation = lookingDown();
    world.add<DecalComponent>(entity) = {.material = Uuid::generate(), .order = order};
    return entity;
}

} // namespace

TEST_SUITE("Decals") {
    TEST_CASE("a point is placed in a decal's box: across its image, down it, and along the projection") {
        // Two metres across, four up its image and one deep, about (1, 2, 3), projected down.
        const Mat4 decalFromWorld = glm::inverse(
            Transform{.position = Vec3(1.0f, 2.0f, 3.0f), .rotation = lookingDown(), .scale = Vec3(2.0f, 4.0f, 1.0f)}
                .toMatrix());
        const DecalPoint middle = decalPoint(decalFromWorld, Vec3(1.0f, 2.0f, 3.0f));
        CHECK(middle.inside);
        CHECK_NEAR(middle.uv, Vec2(0.5f), 1.0e-6f);
        CHECK(std::abs(middle.depth) < 1.0e-6f);
        // A metre to +X is the right edge of the image; two to -Z of the world, its top.
        CHECK_NEAR(decalPoint(decalFromWorld, Vec3(2.0f, 2.0f, 3.0f)).uv, Vec2(1.0f, 0.5f), 1.0e-5f);
        CHECK_NEAR(decalPoint(decalFromWorld, Vec3(1.0f, 2.0f, 1.0f)).uv, Vec2(0.5f, 0.0f), 1.0e-5f);
        // Half a metre up is the face it is projected from; further up, outside.
        CHECK(decalPoint(decalFromWorld, Vec3(1.0f, 2.5f, 3.0f)).depth ==
              doctest::Approx(-0.5f).scale(0.0).epsilon(1.0e-5));
        CHECK(decalPoint(decalFromWorld, Vec3(1.0f, 2.49f, 3.0f)).inside);
        CHECK_FALSE(decalPoint(decalFromWorld, Vec3(1.0f, 2.51f, 3.0f)).inside);
        CHECK_FALSE(decalPoint(decalFromWorld, Vec3(2.1f, 2.0f, 3.0f)).inside);
        CHECK_FALSE(decalPoint(decalFromWorld, Vec3(kNotANumber)).inside);
    }

    TEST_CASE("a surface takes all of a decal it faces, less as it turns away, and none beyond the fade angle") {
        const Vec3 down(0.0f, -1.0f, 0.0f);
        CHECK(decalFade(Vec3(0.0f, 1.0f, 0.0f), down, 60.0f) == 1.0f);
        CHECK(decalFade(tiltedBy(45.0f), down, 60.0f) == 1.0f);
        CHECK(decalFade(tiltedBy(50.0f), down, 60.0f) == doctest::Approx(1.0f).scale(0.0).epsilon(1.0e-4));
        // Half way through the fade, by a smooth step: a half.
        const float facing = std::cos(glm::radians(55.0f));
        const float t =
            (facing - std::cos(glm::radians(60.0f))) / (std::cos(glm::radians(50.0f)) - std::cos(glm::radians(60.0f)));
        CHECK(decalFade(tiltedBy(55.0f), down, 60.0f) ==
              doctest::Approx(t * t * (3.0f - 2.0f * t)).scale(0.0).epsilon(1.0e-4));
        CHECK(decalFade(tiltedBy(60.0f), down, 60.0f) == 0.0f);
        CHECK(decalFade(tiltedBy(89.0f), down, 90.0f) > 0.0f);
        CHECK(decalFade(tiltedBy(90.0f), down, 90.0f) == 0.0f);
        // Facing away, or no number: nothing.
        CHECK(decalFade(Vec3(0.0f, -1.0f, 0.0f), down, 90.0f) == 0.0f);
        CHECK(decalFade(Vec3(kNotANumber), down, 60.0f) == 0.0f);
        // An angle out of its range is brought into it; one that is no number is 60.
        CHECK(decalFade(tiltedBy(5.0f), down, 1.0f) == decalFade(tiltedBy(5.0f), down, kDecalFadeWidth));
        CHECK(decalFade(tiltedBy(55.0f), down, kNotANumber) == decalFade(tiltedBy(55.0f), down, 60.0f));
    }

    TEST_CASE("the decals of a world are taken by their order, then by id, and only those that can land") {
        World world;
        const Entity late = addDecal(world, 2);
        const Entity early = addDecal(world, -1);
        const Entity first = addDecal(world, 0);
        const Entity second = addDecal(world, 0);
        // What cannot land: no material, nothing of it, switched off, a flat box, no numbers.
        world.tryGet<DecalComponent>(addDecal(world))->material = Uuid();
        world.tryGet<DecalComponent>(addDecal(world))->opacity = 0.0f;
        world.setEnabled(addDecal(world), false);
        world.transform(addDecal(world)).scale = Vec3(1.0f, 1.0f, 0.0f);
        world.transform(addDecal(world)).position = Vec3(kNotANumber);
        world.updateWorldTransforms();

        SceneDecals decals;
        decals.collect(world);
        REQUIRE(decals.decals().size() == 4);
        CHECK(decals.dropped() == 0);
        const bool firstFirst = world.id(first) < world.id(second);
        const std::array<Entity, 4> expected = {early, firstFirst ? first : second, firstFirst ? second : first, late};
        for (size_t index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(decals.decals()[index].material == world.tryGet<DecalComponent>(expected[index])->material);
        }
        // Projected along the entity's -Z axis: straight down.
        CHECK_NEAR(decals.decals()[0].projection, Vec3(0.0f, -1.0f, 0.0f), 1.0e-6f);
        CHECK(decals.decals()[0].decalFromWorld == glm::inverse(decals.decals()[0].worldFromDecal));
    }

    TEST_CASE("a decal is projected the way its depth grows, and its bounds hold its box") {
        World world;
        // Two metres across, four up its image and one deep, about (1, 2, 3), projected down:
        // its image is up along -Z of the world.
        const Entity entity = addDecal(world);
        world.transform(entity).position = Vec3(1.0f, 2.0f, 3.0f);
        world.transform(entity).scale = Vec3(2.0f, 4.0f, 1.0f);
        world.updateWorldTransforms();
        SceneDecals decals;
        decals.collect(world);
        REQUIRE(decals.decals().size() == 1);
        CHECK_NEAR(decals.decals()[0].projection, Vec3(0.0f, -1.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(decals.decals()[0].bounds.min, Vec3(0.0f, 1.5f, 1.0f), 1.0e-5f);
        CHECK_NEAR(decals.decals()[0].bounds.max, Vec3(2.0f, 2.5f, 5.0f), 1.0e-5f);

        // Mirrored along its depth, it is projected the other way: up.
        world.transform(entity).scale = Vec3(2.0f, 4.0f, -1.0f);
        world.updateWorldTransforms();
        decals.collect(world);
        REQUIRE(decals.decals().size() == 1);
        CHECK_NEAR(decals.decals()[0].projection, Vec3(0.0f, 1.0f, 0.0f), 1.0e-6f);
        // Mirrored across, it is not.
        world.transform(entity).scale = Vec3(-2.0f, 4.0f, 1.0f);
        world.updateWorldTransforms();
        decals.collect(world);
        REQUIRE(decals.decals().size() == 1);
        CHECK_NEAR(decals.decals()[0].projection, Vec3(0.0f, -1.0f, 0.0f), 1.0e-6f);

        // Turned under a parent that is stretched, the box is sheared: the way its depth grows
        // is then at right angles to its image, not along its own Z axis.
        const Entity parent = world.createEntity("Parent");
        world.transform(parent).scale = Vec3(1.0f, 3.0f, 1.0f);
        REQUIRE(world.setParent(entity, parent, false).has_value());
        world.transform(entity) =
            Transform{.rotation = glm::angleAxis(glm::radians(45.0f), Vec3(0.0f, 0.0f, 1.0f)) * lookingDown()};
        world.updateWorldTransforms();
        decals.collect(world);
        REQUIRE(decals.decals().size() == 1);
        const SceneDecal& sheared = decals.decals()[0];
        const Vec3 across(sheared.worldFromDecal[0]);
        const Vec3 up(sheared.worldFromDecal[1]);
        CHECK(std::abs(glm::dot(sheared.projection, across)) < 1.0e-5f);
        CHECK(std::abs(glm::dot(sheared.projection, up)) < 1.0e-5f);
        CHECK(std::abs(glm::dot(glm::normalize(-Vec3(sheared.worldFromDecal[2])), sheared.projection)) < 0.99f);
    }

    TEST_CASE("a view takes the first decals in their order, as many as it can hold, and counts the rest") {
        World world;
        for (uint32_t index = 0; index < kMaxDecals + 10; ++index) {
            addDecal(world, static_cast<int32_t>(index));
        }
        world.updateWorldTransforms();
        SceneDecals decals;
        decals.collect(world);
        CHECK(decals.decals().size() == kMaxDecals);
        CHECK(decals.dropped() == 10);
        // Collected again, the same, not more.
        decals.collect(world);
        CHECK(decals.decals().size() == kMaxDecals);
        CHECK(decals.dropped() == 10);
    }
}
