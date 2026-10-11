#include "cordite/renderer/ScenePostProcess.h"

#include "cordite/core/Uuid.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::unwrap;

namespace {

Uuid idOf(std::string_view text) {
    const std::optional<Uuid> parsed = Uuid::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// Adds an entity with a PostProcess component that sets the exposure, which is how
/// these tests tell which components were applied, and how much.
Entity addLook(World& world, std::string_view name, float exposure, const Transform& transform = {}) {
    const Entity entity = world.createEntity(name);
    world.transform(entity) = transform;
    world.add<PostProcessComponent>(entity).exposure = exposure;
    return entity;
}

PostProcessComponent& lookOf(World& world, Entity entity) {
    PostProcessComponent* component = world.tryGet<PostProcessComponent>(entity);
    REQUIRE(component != nullptr);
    return *component;
}

PostProcessSettings resolveAt(World& world, const Vec3& position, const Uuid& camera = {}) {
    world.updateWorldTransforms();
    return resolvePostProcess(world, {.position = position, .camera = camera});
}

Mat4 placedAt(const Vec3& position, const Quat& rotation = kIdentityQuat, const Vec3& scale = Vec3(1.0f)) {
    return Transform{position, rotation, scale}.toMatrix();
}

} // namespace

TEST_SUITE("ScenePostProcess") {
    TEST_CASE("a world that says nothing gets the settings as they are when nothing is said") {
        World world;
        const PostProcessSettings look = resolveAt(world, Vec3(0.0f));
        const PostProcessSettings defaults;
        CHECK(look.exposure == defaults.exposure);
        CHECK(look.exposureMode == defaults.exposureMode);
        CHECK(look.toneMapper == defaults.toneMapper);
        CHECK(look.bloomIntensity == defaults.bloomIntensity);
        CHECK(look.saturation == defaults.saturation);
        CHECK(look.vignette == defaults.vignette);
        CHECK(look.colorLut.isNil());
    }

    TEST_CASE("a global component applies wherever the view is seen from, by its weight") {
        World world;
        const Entity entity = addLook(world, "Look", 2.0f);
        PostProcessComponent& component = lookOf(world, entity);
        component.toneMapper = ToneMapper::Aces;
        component.saturation = 0.5f;

        for (const Vec3& position : {Vec3(0.0f), Vec3(500.0f, -20.0f, 3.0f)}) {
            const PostProcessSettings look = resolveAt(world, position);
            CHECK(look.exposure == 2.0f);
            CHECK(look.toneMapper == ToneMapper::Aces);
            CHECK(look.saturation == 0.5f);
        }

        // Three quarters of it: numbers go that far, the rest is over half and changes.
        component.weight = 0.75f;
        PostProcessSettings look = resolveAt(world, Vec3(0.0f));
        CHECK(look.exposure == doctest::Approx(1.5f));
        CHECK(look.saturation == doctest::Approx(0.625f));
        CHECK(look.toneMapper == ToneMapper::Aces);
        // A quarter: not yet.
        component.weight = 0.25f;
        look = resolveAt(world, Vec3(0.0f));
        CHECK(look.exposure == doctest::Approx(0.5f));
        CHECK(look.toneMapper == ToneMapper::Filmic);
        // None at all, and weights that are no weights.
        component.weight = 0.0f;
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
        component.weight = -3.0f;
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
        component.weight = 7.0f;
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 2.0f);
        component.weight = std::numeric_limits<float>::quiet_NaN();
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
    }

    TEST_CASE("a box applies inside it, fades out over its blend distance and is gone beyond") {
        // A cube of one metre, where its matrix puts it.
        const Mat4 unit = placedAt(Vec3(0.0f));
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.0f)) == 1.0f);
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.5f, -0.5f, 0.49f)) == 1.0f);
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.75f, 0.0f, 0.0f)) == doctest::Approx(0.75f));
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.0f, -1.0f, 0.0f)) == doctest::Approx(0.5f));
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.0f, 0.0f, 1.5f)) == doctest::Approx(0.0f));
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(9.0f, 0.0f, 0.0f)) == 0.0f);
        // Off a corner, the distance is to the corner.
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(0.8f, 0.9f, 0.0f)) == doctest::Approx(0.5f));
        // A longer fade reaches further.
        CHECK(postProcessBoxWeight(unit, 4.0f, Vec3(1.5f, 0.0f, 0.0f)) == doctest::Approx(0.75f));
        // No fade: the box ends where it ends.
        CHECK(postProcessBoxWeight(unit, 0.0f, Vec3(0.5f, 0.0f, 0.0f)) == 1.0f);
        CHECK(postProcessBoxWeight(unit, 0.0f, Vec3(0.51f, 0.0f, 0.0f)) == 0.0f);
        CHECK(postProcessBoxWeight(unit, -2.0f, Vec3(0.51f, 0.0f, 0.0f)) == 0.0f);

        // Moved, scaled and turned: ten metres along X, two up, four deep, a quarter turn
        // about Y - so that its ten metres lie along the world's Z.
        const Mat4 room =
            placedAt(Vec3(20.0f, 1.0f, 0.0f), glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f)), Vec3(10.0f, 2.0f, 4.0f));
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(20.0f, 1.0f, 0.0f)) == 1.0f);
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(20.0f, 1.0f, 4.9f)) == 1.0f);
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(21.9f, 1.9f, -4.9f)) == 1.0f);
        // A metre past its end along Z, in metres of the world whatever the scale.
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(20.0f, 1.0f, 6.0f)) == doctest::Approx(0.5f).epsilon(0.001));
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(23.0f, 1.0f, 0.0f)) == doctest::Approx(0.5f).epsilon(0.001));
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(20.0f, 3.0f, 0.0f)) == doctest::Approx(0.5f).epsilon(0.001));
        CHECK(postProcessBoxWeight(room, 2.0f, Vec3(20.0f, 1.0f, 7.5f)) == 0.0f);

        // A box without a size, a position that is nowhere: nothing applies.
        CHECK(postProcessBoxWeight(placedAt(Vec3(0.0f), kIdentityQuat, Vec3(0.0f, 1.0f, 1.0f)), 1.0f, Vec3(0.0f)) ==
              0.0f);
        CHECK(postProcessBoxWeight(Mat4(0.0f), 1.0f, Vec3(0.0f)) == 0.0f);
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        CHECK(postProcessBoxWeight(unit, 1.0f, Vec3(notANumber, 0.0f, 0.0f)) == 0.0f);
        CHECK(postProcessBoxWeight(unit, notANumber, Vec3(0.6f, 0.0f, 0.0f)) == 0.0f);
        CHECK(postProcessBoxWeight(unit, notANumber, Vec3(0.2f, 0.0f, 0.0f)) == 1.0f);
        CHECK(postProcessBoxWeight(Mat4(notANumber), 1.0f, Vec3(0.0f)) == 0.0f);
    }

    TEST_CASE("a component with a box applies to views seen from inside and around it") {
        World world;
        const Entity entity = addLook(world, "Cave", -2.0f, {.position = Vec3(10.0f, 0.0f, 0.0f), .scale = Vec3(4.0f)});
        PostProcessComponent& component = lookOf(world, entity);
        component.global = false;
        component.blendDistance = 2.0f;

        CHECK(resolveAt(world, Vec3(10.0f, 0.0f, 0.0f)).exposure == -2.0f);
        CHECK(resolveAt(world, Vec3(11.9f, 1.0f, -1.5f)).exposure == -2.0f);
        // A metre outside: half of it.
        CHECK(resolveAt(world, Vec3(13.0f, 0.0f, 0.0f)).exposure == doctest::Approx(-1.0f));
        CHECK(resolveAt(world, Vec3(14.5f, 0.0f, 0.0f)).exposure == 0.0f);
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
        // By its weight on top of that.
        component.weight = 0.5f;
        CHECK(resolveAt(world, Vec3(13.0f, 0.0f, 0.0f)).exposure == doctest::Approx(-0.5f));
    }

    TEST_CASE("components are laid over one another by priority, then by the ids of their entities") {
        World world;
        const Entity low = addLook(world, "Low", 1.0f);
        const Entity high = addLook(world, "High", 3.0f);
        lookOf(world, low).priority = -5;
        lookOf(world, high).priority = 5;
        // The higher priority is applied last, and with its full weight nothing of the
        // other is left.
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 3.0f);
        // With half its weight, it goes half the way from what the lower one set.
        lookOf(world, high).weight = 0.5f;
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == doctest::Approx(2.0f));
        // The other way round, the first one is on top.
        lookOf(world, low).priority = 9;
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 1.0f);

        // The same priority: the order of the ids decides, not the order of creation.
        World tied;
        const Entity later = unwrap(tied.createEntityWithId(idOf("00000000-0000-4000-8000-0000000000b0"), "B"));
        const Entity earlier = unwrap(tied.createEntityWithId(idOf("00000000-0000-4000-8000-0000000000a0"), "A"));
        tied.add<PostProcessComponent>(later).exposure = 4.0f;
        tied.add<PostProcessComponent>(earlier).exposure = -4.0f;
        CHECK(resolveAt(tied, Vec3(0.0f)).exposure == 4.0f);
        World tiedOtherWay;
        const Entity first = unwrap(tiedOtherWay.createEntityWithId(idOf("00000000-0000-4000-8000-0000000000a0"), "A"));
        const Entity second =
            unwrap(tiedOtherWay.createEntityWithId(idOf("00000000-0000-4000-8000-0000000000b0"), "B"));
        tiedOtherWay.add<PostProcessComponent>(first).exposure = -4.0f;
        tiedOtherWay.add<PostProcessComponent>(second).exposure = 4.0f;
        CHECK(resolveAt(tiedOtherWay, Vec3(0.0f)).exposure == 4.0f);
    }

    TEST_CASE("disabled entities say nothing") {
        World world;
        const Entity group = world.createEntity("Group");
        const Entity entity = addLook(world, "Look", 2.0f);
        REQUIRE(world.setParent(entity, group).has_value());
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 2.0f);
        world.setEnabled(entity, false);
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
        world.setEnabled(entity, true);
        world.setEnabled(group, false);
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == 0.0f);
    }

    TEST_CASE("a component on a camera is for the views through that camera, and has the last word") {
        World world;
        addLook(world, "Scene", 1.0f);
        const Entity camera = addLook(world, "Camera", -3.0f, {.position = Vec3(100.0f, 0.0f, 0.0f)});
        world.add<CameraComponent>(camera);
        PostProcessComponent& own = lookOf(world, camera);
        // Neither a box nor a low priority make a difference to it.
        own.global = false;
        own.priority = -100;
        const Entity otherCamera = world.createEntity("Other camera");
        world.add<CameraComponent>(otherCamera);

        // Seen through it: its settings, over the scene's.
        CHECK(resolveAt(world, Vec3(0.0f), world.id(camera)).exposure == -3.0f);
        own.weight = 0.5f;
        CHECK(resolveAt(world, Vec3(0.0f), world.id(camera)).exposure == doctest::Approx(-1.0f));
        own.weight = 1.0f;
        // Seen through another camera, or through none that is an entity: the scene's
        // alone - even from where the camera stands.
        CHECK(resolveAt(world, Vec3(0.0f), world.id(otherCamera)).exposure == 1.0f);
        CHECK(resolveAt(world, Vec3(100.0f, 0.0f, 0.0f)).exposure == 1.0f);
        // A camera that is switched off sets nothing.
        world.setEnabled(camera, false);
        CHECK(resolveAt(world, Vec3(0.0f), world.id(camera)).exposure == 1.0f);
    }

    TEST_CASE("settings out of their ranges are brought into them before they are applied") {
        World world;
        const Entity entity = addLook(world, "Wild", 1000.0f);
        PostProcessComponent& component = lookOf(world, entity);
        component.vignette = 5.0f;
        component.gamma = Vec3(0.0f);
        component.bloomIntensity = std::numeric_limits<float>::quiet_NaN();
        const PostProcessSettings look = resolveAt(world, Vec3(0.0f));
        CHECK(look.exposure == 16.0f);
        CHECK(look.vignette == 1.0f);
        CHECK(look.gamma == Vec3(0.2f));
        CHECK(look.bloomIntensity == PostProcessSettings().bloomIntensity);
    }

    TEST_CASE("more components than a view can take in: those of the highest priority count") {
        World world;
        // A hundred that each go a tenth of the way to their own exposure; only the
        // sixty-four of the highest priority are applied, in their order.
        const auto exposureOf = [](int index) { return static_cast<float>(index) * 0.125f; };
        for (int index = 0; index < 100; ++index) {
            const Entity entity = addLook(world, "Look " + std::to_string(index), exposureOf(index));
            PostProcessComponent& component = lookOf(world, entity);
            component.priority = index;
            component.weight = 0.1f;
        }
        float expected = 0.0f;
        float withAll = 0.0f;
        for (int index = 0; index < 100; ++index) {
            withAll += (exposureOf(index) - withAll) * 0.1f;
            if (index >= 36) {
                expected += (exposureOf(index) - expected) * 0.1f;
            }
        }
        CHECK(resolveAt(world, Vec3(0.0f)).exposure == doctest::Approx(expected).epsilon(1.0e-5));
        // Which is not what all hundred would have come to.
        CHECK(std::abs(expected - withAll) > 0.001f);
    }
}
