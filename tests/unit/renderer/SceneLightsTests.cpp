#include "cordite/renderer/SceneLights.h"

#include "cordite/core/Uuid.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"
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
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

Mat4 placedAt(const Vec3& position, const Quat& rotation = kIdentityQuat, const Vec3& scale = Vec3(1.0f)) {
    return Transform{position, rotation, scale}.toMatrix();
}

/// Adds an entity with a light to a world.
Entity addLight(World& world, std::string_view name, const LightComponent& light, const Transform& transform = {}) {
    const Entity entity = world.createEntity(name);
    world.transform(entity) = transform;
    world.add<LightComponent>(entity) = light;
    return entity;
}

Uuid id(std::string_view text) {
    const std::optional<Uuid> parsed = Uuid::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_SUITE("SceneLights") {
    TEST_CASE("a light is where its entity is and shines along the entity's -Z axis") {
        const LightComponent component{.type = LightType::Spot};

        const SceneLight forward = makeSceneLight(component, placedAt(Vec3(1.0f, 2.0f, 3.0f)));
        CHECK(forward.type == GpuLightType::Spot);
        CHECK_NEAR(forward.position, Vec3(1.0f, 2.0f, 3.0f), 1.0e-6f);
        CHECK_NEAR(forward.direction, Vec3(0.0f, 0.0f, -1.0f), 1.0e-6f);

        // Pitched a quarter turn down, it shines at the ground.
        const SceneLight down =
            makeSceneLight(component, placedAt(Vec3(0.0f), glm::angleAxis(-kHalfPi, Vec3(1.0f, 0.0f, 0.0f))));
        CHECK_NEAR(down.direction, Vec3(0.0f, -1.0f, 0.0f), 1.0e-6f);

        // Turned a quarter turn to the left, along -X.
        const SceneLight left =
            makeSceneLight(component, placedAt(Vec3(0.0f), glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f))));
        CHECK_NEAR(left.direction, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-6f);

        // A scale changes neither where the light is nor that its direction has length one.
        const SceneLight scaled = makeSceneLight(
            component, placedAt(Vec3(4.0f, 0.0f, 0.0f), lookRotation(Vec3(1.0f, -1.0f, 0.0f)), Vec3(3.0f, 0.5f, 7.0f)));
        CHECK_NEAR(scaled.position, Vec3(4.0f, 0.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(scaled.direction, glm::normalize(Vec3(1.0f, -1.0f, 0.0f)), 1.0e-5f);

        // An entity flattened to nothing has no direction: the light shines down.
        const SceneLight flattened = makeSceneLight(component, placedAt(Vec3(0.0f), kIdentityQuat, Vec3(0.0f)));
        CHECK_NEAR(flattened.direction, Vec3(0.0f, -1.0f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("an intensity of one becomes a radiance of pi, colored by the light") {
        const SceneLight plain = makeSceneLight(LightComponent(), Mat4(1.0f));
        CHECK_NEAR(plain.color, Vec3(kPi), 1.0e-5f);

        const SceneLight colored =
            makeSceneLight(LightComponent{.color = Vec3(1.0f, 0.5f, 0.25f), .intensity = 2.0f}, Mat4(1.0f));
        CHECK_NEAR(colored.color, Vec3(2.0f * kPi, kPi, 0.5f * kPi), 1.0e-5f);

        // Color and intensity are never negative: there is no such thing as taking light away.
        const SceneLight negativeColor = makeSceneLight(LightComponent{.color = Vec3(-1.0f, 0.5f, -0.1f)}, Mat4(1.0f));
        CHECK_NEAR(negativeColor.color, Vec3(0.0f, 0.5f * kPi, 0.0f), 1.0e-5f);
        const SceneLight negativeIntensity = makeSceneLight(LightComponent{.intensity = -4.0f}, Mat4(1.0f));
        CHECK(negativeIntensity.color == Vec3(0.0f));
    }

    TEST_CASE("cone angles become radians, the inner cone never wider than the outer one") {
        const SceneLight spot = makeSceneLight(
            LightComponent{.type = LightType::Spot, .innerConeDegrees = 30.0f, .outerConeDegrees = 60.0f}, Mat4(1.0f));
        CHECK(spot.innerConeAngle == doctest::Approx(kPi / 6.0f));
        CHECK(spot.outerConeAngle == doctest::Approx(kPi / 3.0f));

        const SceneLight swapped = makeSceneLight(
            LightComponent{.type = LightType::Spot, .innerConeDegrees = 50.0f, .outerConeDegrees = 20.0f}, Mat4(1.0f));
        CHECK(swapped.outerConeAngle == doctest::Approx(toRadians(20.0f)));
        CHECK(swapped.innerConeAngle == doctest::Approx(toRadians(20.0f)));

        // Set from code, past what a file may hold: brought back into what a cone can be.
        const SceneLight wide = makeSceneLight(
            LightComponent{.type = LightType::Spot, .innerConeDegrees = -10.0f, .outerConeDegrees = 170.0f},
            Mat4(1.0f));
        CHECK(wide.innerConeAngle == 0.0f);
        CHECK(wide.outerConeAngle == doctest::Approx(toRadians(89.0f)));
    }

    TEST_CASE("the kind, the range and the channels of a light are handed on as they are") {
        CHECK(makeSceneLight(LightComponent{.type = LightType::Directional}, Mat4(1.0f)).type ==
              GpuLightType::Directional);
        CHECK(makeSceneLight(LightComponent{.type = LightType::Point}, Mat4(1.0f)).type == GpuLightType::Point);
        CHECK(makeSceneLight(LightComponent{.type = LightType::Spot}, Mat4(1.0f)).type == GpuLightType::Spot);

        CHECK(makeSceneLight(LightComponent{.range = 42.5f}, Mat4(1.0f)).range == 42.5f);
        CHECK(makeSceneLight(LightComponent{.channels = 0x24}, Mat4(1.0f)).channels == 0x24);
        // There are eight channels; bits beyond them mean nothing.
        CHECK(makeSceneLight(LightComponent{.channels = 0xFFFFFF01u}, Mat4(1.0f)).channels == 0x01);
    }

    TEST_CASE("how a light casts shadows is handed on, with its resolution as a factor") {
        const SceneLight plain = makeSceneLight(LightComponent(), Mat4(1.0f));
        CHECK_FALSE(plain.shadow.enabled);
        CHECK(plain.shadow.bias == 1.0f);
        CHECK(plain.shadow.normalOffset == 1.0f);
        CHECK(plain.shadow.resolutionScale == 1.0f);
        CHECK(plain.shadow.distance == 60.0f);
        CHECK(plain.shadow.cascades == 4);
        CHECK(plain.shadow.cascadeDistribution == 0.75f);

        const SceneLight set = makeSceneLight(LightComponent{.castsShadows = true,
                                                             .shadowBias = 0.5f,
                                                             .shadowNormalOffset = 3.0f,
                                                             .shadowResolution = ShadowResolution::High,
                                                             .shadowDistance = 35.0f,
                                                             .shadowCascades = 2,
                                                             .shadowCascadeDistribution = 0.25f},
                                              Mat4(1.0f));
        CHECK(set.shadow.enabled);
        CHECK(set.shadow.bias == 0.5f);
        CHECK(set.shadow.normalOffset == 3.0f);
        CHECK(set.shadow.resolutionScale == 2.0f);
        CHECK(set.shadow.distance == 35.0f);
        CHECK(set.shadow.cascades == 2);
        CHECK(set.shadow.cascadeDistribution == 0.25f);
        CHECK(makeSceneLight(LightComponent{.shadowResolution = ShadowResolution::Low}, Mat4(1.0f))
                  .shadow.resolutionScale == 0.5f);

        // Offsets are never negative: they would push a surface into its own shadow.
        const SceneLight negative =
            makeSceneLight(LightComponent{.shadowBias = -1.0f, .shadowNormalOffset = -2.0f}, Mat4(1.0f));
        CHECK(negative.shadow.bias == 0.0f);
        CHECK(negative.shadow.normalOffset == 0.0f);
    }

    TEST_CASE("the lights of a world are collected with their place in the world") {
        World world;
        const Entity room = world.createEntity("Room");
        world.transform(room) = {.position = Vec3(10.0f, 0.0f, 0.0f),
                                 .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f))};
        const Entity lamp = addLight(world, "Lamp", {.type = LightType::Spot, .intensity = 2.0f, .range = 5.0f},
                                     {.position = Vec3(0.0f, 3.0f, -2.0f)});
        REQUIRE(world.setParent(lamp, room, false).has_value());
        world.updateWorldTransforms();

        SceneLights lights;
        lights.collect(world);
        REQUIRE(lights.lights().size() == 1);
        const SceneLight& light = lights.lights()[0];
        CHECK(light.type == GpuLightType::Spot);
        // The room turns its -Z axis to -X: the lamp is two metres along that, and looks on.
        CHECK_NEAR(light.position, Vec3(8.0f, 3.0f, 0.0f), 1.0e-5f);
        CHECK_NEAR(light.direction, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-5f);
        CHECK(light.range == 5.0f);
        CHECK_NEAR(light.color, Vec3(2.0f * kPi), 1.0e-5f);
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK(lights.groundColor() == Vec3(0.0f));
        CHECK_FALSE(lights.isDefault());
    }

    TEST_CASE("a light that is switched off, or below something that is, is not collected") {
        World world;
        const Entity group = world.createEntity("Group");
        const Entity inside = addLight(world, "Inside", {.range = 1.0f});
        REQUIRE(world.setParent(inside, group).has_value());
        const Entity alone = addLight(world, "Alone", {.range = 2.0f});
        addLight(world, "Always", {.range = 3.0f});
        world.updateWorldTransforms();

        SceneLights lights;
        lights.collect(world);
        CHECK(lights.lights().size() == 3);

        world.setEnabled(alone, false);
        lights.collect(world);
        CHECK(lights.lights().size() == 2);

        world.setEnabled(group, false);
        lights.collect(world);
        REQUIRE(lights.lights().size() == 1);
        CHECK(lights.lights()[0].range == 3.0f);

        world.setEnabled(group, true);
        world.setEnabled(alone, true);
        lights.collect(world);
        CHECK(lights.lights().size() == 3);
    }

    TEST_CASE("lights come in the order of their entities' ids, whatever order they were made in") {
        const std::vector<Uuid> ids = {
            id("30000000-0000-4000-8000-000000000000"), id("10000000-0000-4000-8000-000000000000"),
            id("20000000-0000-4000-8000-000000000000"), id("10000000-0000-4000-8000-000000000001")};
        const auto collect = [&ids](const std::vector<size_t>& creationOrder, bool removeAndAddAgain) {
            World world;
            std::vector<Entity> entities;
            for (const size_t index : creationOrder) {
                const Entity entity = unwrap(world.createEntityWithId(ids[index], "Light"));
                // The range says which light it is.
                world.add<LightComponent>(entity).range = static_cast<float>(index + 1);
                entities.push_back(entity);
            }
            if (removeAndAddAgain) {
                // Shuffles how the components are stored.
                world.remove<LightComponent>(entities[0]);
                world.add<LightComponent>(entities[0]).range = static_cast<float>(creationOrder[0] + 1);
            }
            world.updateWorldTransforms();
            SceneLights lights;
            lights.collect(world);
            std::vector<float> ranges;
            for (const SceneLight& light : lights.lights()) {
                ranges.push_back(light.range);
            }
            return ranges;
        };

        // By id: the second, the fourth, the third, the first.
        const std::vector<float> expected = {2.0f, 4.0f, 3.0f, 1.0f};
        CHECK(collect({0, 1, 2, 3}, false) == expected);
        CHECK(collect({3, 2, 1, 0}, false) == expected);
        CHECK(collect({2, 0, 3, 1}, false) == expected);
        CHECK(collect({1, 3, 0, 2}, true) == expected);
    }

    TEST_CASE("ambient lights add up, each times its intensity") {
        World world;
        SceneLights lights;
        lights.collect(world);
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK(lights.groundColor() == Vec3(0.0f));

        const Entity sky = world.createEntity("Sky");
        world.add<AmbientLightComponent>(sky) = {
            .skyColor = Vec3(0.2f, 0.4f, 0.8f), .groundColor = Vec3(0.4f, 0.2f, 0.1f), .intensity = 0.5f};
        lights.collect(world);
        CHECK_NEAR(lights.skyColor(), Vec3(0.1f, 0.2f, 0.4f), 1.0e-6f);
        CHECK_NEAR(lights.groundColor(), Vec3(0.2f, 0.1f, 0.05f), 1.0e-6f);
        CHECK(lights.lights().empty());

        const Entity glow = world.createEntity("Glow");
        world.add<AmbientLightComponent>(glow) = {
            .skyColor = Vec3(1.0f, 0.0f, 0.0f), .groundColor = Vec3(0.0f, 0.0f, 1.0f), .intensity = 2.0f};
        lights.collect(world);
        CHECK_NEAR(lights.skyColor(), Vec3(2.1f, 0.2f, 0.4f), 1.0e-6f);
        CHECK_NEAR(lights.groundColor(), Vec3(0.2f, 0.1f, 2.05f), 1.0e-6f);

        // One that is switched off adds nothing; negative values count as none.
        world.setEnabled(sky, false);
        lights.collect(world);
        CHECK_NEAR(lights.skyColor(), Vec3(2.0f, 0.0f, 0.0f), 1.0e-6f);
        world.tryGet<AmbientLightComponent>(glow)->intensity = -1.0f;
        lights.collect(world);
        CHECK(lights.skyColor() == Vec3(0.0f));
        world.tryGet<AmbientLightComponent>(glow)->intensity = 1.0f;
        world.tryGet<AmbientLightComponent>(glow)->skyColor = Vec3(-1.0f, 0.5f, 0.0f);
        lights.collect(world);
        CHECK_NEAR(lights.skyColor(), Vec3(0.0f, 0.5f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("a world has the environment of the enabled entity with the lowest id, or none") {
        World world;
        SceneLights lights;
        lights.collect(world);
        CHECK(lights.environment().texture.isNil());
        CHECK(lights.environment().intensity == 0.0f);
        CHECK_FALSE(lights.environment().visible);

        const Uuid dusk = id("11111111-1111-4111-8111-111111111111");
        const Uuid noon = id("22222222-2222-4222-8222-222222222222");
        // Made in the order of their ids turned round.
        const Entity second = unwrap(world.createEntityWithId(id("00000000-0000-4000-8000-0000000000b0"), "Noon"));
        world.add<EnvironmentComponent>(second) = {
            .texture = noon, .intensity = 3.0f, .rotationDegrees = 180.0f, .visible = false};
        lights.collect(world);
        CHECK(lights.environment().texture == noon);
        CHECK(lights.environment().intensity == 3.0f);
        CHECK(lights.environment().rotation == doctest::Approx(kPi));
        CHECK_FALSE(lights.environment().visible);
        // An environment is no light among the lights, and no ambient light.
        CHECK(lights.lights().empty());
        CHECK(lights.skyColor() == Vec3(0.0f));

        const Entity first = unwrap(world.createEntityWithId(id("00000000-0000-4000-8000-0000000000a0"), "Dusk"));
        world.add<EnvironmentComponent>(first) = {.texture = dusk, .intensity = 0.5f, .rotationDegrees = -90.0f};
        lights.collect(world);
        CHECK(lights.environment().texture == dusk);
        CHECK(lights.environment().intensity == 0.5f);
        CHECK(lights.environment().rotation == doctest::Approx(-kHalfPi));
        CHECK(lights.environment().visible);

        // Switched off - itself or something above it - it gives way to the next.
        world.setEnabled(first, false);
        lights.collect(world);
        CHECK(lights.environment().texture == noon);
        world.setEnabled(first, true);
        const Entity group = world.createEntity("Group");
        REQUIRE(world.setParent(first, group).has_value());
        lights.collect(world);
        CHECK(lights.environment().texture == dusk);
        world.setEnabled(group, false);
        lights.collect(world);
        CHECK(lights.environment().texture == noon);

        // One without an image is one all the same: the world has no environment then.
        world.setEnabled(group, true);
        world.tryGet<EnvironmentComponent>(first)->texture = Uuid();
        lights.collect(world);
        CHECK(lights.environment().texture.isNil());

        // With all of them off there is none, and what was collected before is gone.
        world.setEnabled(group, false);
        world.setEnabled(second, false);
        lights.collect(world);
        CHECK(lights.environment().texture.isNil());
        CHECK(lights.environment().intensity == 0.0f);
        CHECK(lights.environment().rotation == 0.0f);
    }

    TEST_CASE("an environment's numbers reach the renderer as numbers") {
        World world;
        const Entity sky = world.createEntity("Sky");
        EnvironmentComponent& environment = world.add<EnvironmentComponent>(sky);
        environment.texture = id("11111111-1111-4111-8111-111111111111");
        SceneLights lights;

        // Less than no light is none; what is no number is none, and no turn.
        environment.intensity = -2.0f;
        lights.collect(world);
        CHECK(lights.environment().intensity == 0.0f);
        environment.intensity = std::numeric_limits<float>::quiet_NaN();
        environment.rotationDegrees = std::numeric_limits<float>::infinity();
        lights.collect(world);
        CHECK(lights.environment().intensity == 0.0f);
        CHECK(lights.environment().rotation == 0.0f);
        environment.intensity = 4.0f;
        environment.rotationDegrees = 45.0f;
        lights.collect(world);
        CHECK(lights.environment().intensity == 4.0f);
        CHECK(lights.environment().rotation == doctest::Approx(kPi / 4.0f));
    }

    TEST_CASE("a world without light components gets the default lighting, and only when asked") {
        World world;
        world.createEntity("Crate");
        SceneLights lights;

        // A game shows what its scene says: nothing.
        lights.collect(world);
        CHECK(lights.lights().empty());
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK_FALSE(lights.isDefault());

        // An editor asks for stand-ins: a key light from above and light from all around.
        lights.collect(world, {.defaultLighting = true});
        CHECK(lights.isDefault());
        REQUIRE(lights.lights().size() == 1);
        const SceneLight& key = lights.lights()[0];
        CHECK(key.type == GpuLightType::Directional);
        CHECK(glm::length(key.direction) == doctest::Approx(1.0f));
        CHECK_NEAR(key.direction, glm::normalize(Vec3(-0.45f, -0.80f, -0.40f)), 1.0e-6f);
        CHECK_NEAR(key.color, Vec3(2.75f, 2.60f, 2.40f), 1.0e-6f);
        // It lights everything, whatever channel it is in.
        CHECK(key.channels == kAllLightChannels);
        CHECK_NEAR(lights.skyColor(), Vec3(0.26f, 0.31f, 0.40f), 1.0e-6f);
        CHECK_NEAR(lights.groundColor(), Vec3(0.10f, 0.09f, 0.08f), 1.0e-6f);

        // Asking again without replaces them.
        lights.collect(world);
        CHECK(lights.lights().empty());
        CHECK_FALSE(lights.isDefault());
    }

    TEST_CASE("a world that has lights is lit by them alone, even while they are switched off") {
        World world;
        const Entity lamp = addLight(world, "Lamp", {.type = LightType::Point, .range = 4.0f});
        world.updateWorldTransforms();
        SceneLights lights;

        lights.collect(world, {.defaultLighting = true});
        REQUIRE(lights.lights().size() == 1);
        CHECK(lights.lights()[0].type == GpuLightType::Point);
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK_FALSE(lights.isDefault());

        // Switched off: a scene that was made dark stays dark.
        world.setEnabled(lamp, false);
        lights.collect(world, {.defaultLighting = true});
        CHECK(lights.lights().empty());
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK_FALSE(lights.isDefault());

        // Ambient light alone counts as lighting too.
        world.destroyEntity(lamp);
        const Entity sky = world.createEntity("Sky");
        world.add<AmbientLightComponent>(sky) = {.skyColor = Vec3(0.5f), .groundColor = Vec3(0.25f)};
        lights.collect(world, {.defaultLighting = true});
        CHECK(lights.lights().empty());
        CHECK(lights.skyColor() == Vec3(0.5f));
        CHECK_FALSE(lights.isDefault());

        // So does an environment, with an image or without, switched on or off.
        world.destroyEntity(sky);
        const Entity surroundings = world.createEntity("Surroundings");
        world.add<EnvironmentComponent>(surroundings);
        lights.collect(world, {.defaultLighting = true});
        CHECK(lights.lights().empty());
        CHECK(lights.skyColor() == Vec3(0.0f));
        CHECK_FALSE(lights.isDefault());
        world.setEnabled(surroundings, false);
        lights.collect(world, {.defaultLighting = true});
        CHECK_FALSE(lights.isDefault());

        // With the last light component gone, the stand-ins are back, and they come
        // without an environment.
        world.destroyEntity(surroundings);
        lights.collect(world, {.defaultLighting = true});
        CHECK(lights.isDefault());
        CHECK(lights.lights().size() == 1);
        CHECK(lights.environment().texture.isNil());
    }

    TEST_CASE("values that are not numbers are handed on for the clusters to leave out") {
        // Collecting makes no judgment; buildLightClusters drops what cannot be drawn.
        World world;
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        addLight(world, "Broken", {.type = LightType::Point, .intensity = notANumber, .range = notANumber});
        addLight(world, "Whole", {.type = LightType::Point, .range = 2.0f});
        world.updateWorldTransforms();

        SceneLights lights;
        lights.collect(world);
        CHECK(lights.lights().size() == 2);

        CameraComponent camera;
        const CameraView view = makeCameraView(placedAt(Vec3(0.0f, 0.0f, 5.0f)), camera, 16.0f / 9.0f);
        LightClusters clusters;
        buildLightClusters(view, lights.lights(), clusters);
        REQUIRE(clusters.lights.size() == 1);
        CHECK(clusters.lights[0].range == 2.0f);
    }
}
