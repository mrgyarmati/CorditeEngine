/// @file
/// Keeps the showcase scene complete.
///
/// The showcase is the scene that exercises everything the engine has. These tests fail
/// when a component, or a named option of a component, exists in the engine but is not
/// used in samples/showcase/showcase.scene.json: adding a feature includes showing it.

#include "cordite/engine/EngineComponents.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/world/SceneSerializer.h"

#include "support/TestSupport.h"

#include <set>
#include <string>

using namespace cordite;

namespace {

std::filesystem::path showcasePath() {
    return test::sourceDirectory() / "samples" / "showcase" / "showcase.scene.json";
}

/// The showcase loaded into a world, with the engine's components.
struct Showcase {
    ComponentRegistry components;
    World world;

    Showcase() {
        REQUIRE(registerEngineComponents(components).has_value());
        const Status loaded = loadScene(showcasePath(), world, components);
        REQUIRE_MESSAGE(loaded.has_value(), loaded.error().describe());
    }
};

} // namespace

TEST_SUITE("ShowcaseScene") {
    TEST_CASE("the showcase scene loads with the engine's components") {
        const Showcase showcase;
        CHECK(showcase.world.sceneName() == "Showcase");
        CHECK(showcase.world.entityCount() >= 9);
        CHECK(showcase.world.roots().size() >= 2);
    }

    TEST_CASE("every component type of the engine is used in the showcase") {
        const Showcase showcase;
        for (const ComponentDescriptor* descriptor : showcase.components.components()) {
            CAPTURE(descriptor->type.name());
            bool used = false;
            showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
                used = used || descriptor->has(showcase.world.registry(), entity.handle);
            });
            CHECK_MESSAGE(used, "add an entity with a " << descriptor->type.name()
                                                        << " component to samples/showcase/showcase.scene.json");
        }
    }

    TEST_CASE("every named option of every component is used in the showcase") {
        const Showcase showcase;
        for (const ComponentDescriptor* descriptor : showcase.components.components()) {
            for (const PropertyInfo& property : descriptor->type.properties()) {
                if (property.type() != PropertyType::Enum) {
                    continue;
                }

                std::set<std::string> seen;
                showcase.world.traverse([&](Entity entity, uint32_t /*depth*/) {
                    if (const void* instance = descriptor->getConst(showcase.world.registry(), entity.handle)) {
                        seen.insert(property.get(instance).get<std::string>());
                    }
                });
                for (const EnumOption& option : property.enumOptions()) {
                    CAPTURE(descriptor->type.name());
                    CAPTURE(property.name());
                    CHECK_MESSAGE(seen.contains(option.name), "no entity in the showcase uses '" << option.name << "'");
                }
            }
        }
    }

    TEST_CASE("the showcase covers hierarchy, disabled entities and a camera to render from") {
        Showcase showcase;
        World& world = showcase.world;

        bool hasGrandchild = false;
        bool hasDisabledEntity = false;
        bool hasEntityDisabledByAncestor = false;
        world.traverse([&](Entity entity, uint32_t depth) {
            hasGrandchild = hasGrandchild || depth >= 2;
            hasDisabledEntity = hasDisabledEntity || !world.isEnabled(entity);
            hasEntityDisabledByAncestor =
                hasEntityDisabledByAncestor || (world.isEnabled(entity) && !world.isEnabledInHierarchy(entity));
        });
        CHECK(hasGrandchild);
        CHECK(hasDisabledEntity);
        CHECK(hasEntityDisabledByAncestor);

        // A game started on this scene has a camera, and it looks at the stage.
        const Entity camera = findPrimaryCamera(world);
        REQUIRE_FALSE(camera.isNull());
        world.updateWorldTransforms();
        const CameraView view =
            makeCameraView(world.worldMatrix(camera), *world.tryGet<CameraComponent>(camera), 16.0f / 9.0f);
        const std::optional<Vec2> stage = worldToScreen(view, Vec3(0.0f, 0.5f, 0.0f), Vec2(1600.0f, 900.0f));
        REQUIRE(stage.has_value());
        CHECK(stage->x == doctest::Approx(800.0f).epsilon(0.2));
        CHECK(stage->y == doctest::Approx(450.0f).epsilon(0.2));
    }

    TEST_CASE("saving the showcase keeps everything that is in the file") {
        const Showcase showcase;
        const Json saved = serializeScene(showcase.world, showcase.components);

        // The file is written by hand, with comments; what counts is that loading and
        // saving it loses nothing.
        World reloaded;
        REQUIRE(deserializeScene(reloaded, saved, showcase.components).has_value());
        CHECK(serializeScene(reloaded, showcase.components) == saved);

        const Json original = test::unwrap(readJsonFile(showcasePath()));
        CHECK(saved["entities"].size() == original["entities"].size());
        CHECK(saved["name"] == original["name"]);
    }
}
