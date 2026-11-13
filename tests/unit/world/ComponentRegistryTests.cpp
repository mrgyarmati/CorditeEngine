#include "cordite/world/ComponentRegistry.h"

#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

struct Health {
    int32_t points = 100;
    bool invulnerable = false;
};

TypeInfo describeHealth() {
    return TypeBuilder<Health>("Health")
        .property("points", &Health::points, {.minimum = 0.0})
        .property("invulnerable", &Health::invulnerable)
        .build();
}

/// Records the value a Health component has at the moment it is given to an entity.
struct ConstructionProbe {
    std::vector<int32_t> pointsSeen;

    void onHealthConstructed(entt::registry& registry, entt::entity entity) {
        pointsSeen.push_back(registry.get<Health>(entity).points);
    }
};

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

} // namespace

TEST_SUITE("ComponentRegistry") {
    TEST_CASE("the world module registers its components in a fixed order") {
        ComponentRegistry registry;
        REQUIRE(registerWorldComponents(registry).has_value());

        REQUIRE(registry.components().size() == 4);
        CHECK(registry.components()[0]->type.name() == "Transform");
        CHECK(registry.components()[1]->type.name() == "Camera");
        CHECK(registry.components()[2]->type.name() == "PrefabInstance");
        CHECK(registry.components()[3]->type.name() == "PrefabLink");

        const ComponentDescriptor* transform = registry.find("Transform");
        REQUIRE(transform != nullptr);
        CHECK(transform == registry.components()[0]);
        // Every entity has a Transform, so it cannot be added or removed.
        CHECK_FALSE(transform->traits.optional);
        CHECK_FALSE(transform->traits.description.empty());

        const ComponentDescriptor* camera = registry.find("Camera");
        REQUIRE(camera != nullptr);
        CHECK(camera->traits.optional);
        CHECK_FALSE(camera->traits.description.empty());

        CHECK(registry.find("camera") == nullptr);
        CHECK(registry.find("") == nullptr);
    }

    TEST_CASE("every property of a built-in component is documented") {
        ComponentRegistry registry;
        REQUIRE(registerWorldComponents(registry).has_value());

        for (const ComponentDescriptor* descriptor : registry.components()) {
            for (const PropertyInfo& property : descriptor->type.properties()) {
                CAPTURE(descriptor->type.name());
                CAPTURE(property.name());
                CHECK_FALSE(property.attributes().description.empty());
            }
        }
    }

    TEST_CASE("a name can be registered only once") {
        ComponentRegistry registry;
        REQUIRE(registry.add<Health>(describeHealth()).has_value());

        const Status again = registry.add<Health>(describeHealth());
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::AlreadyExists);
        CHECK(again.error().message.find("Health") != std::string::npos);
        CHECK(registry.components().size() == 1);

        REQUIRE(registerWorldComponents(registry).has_value());
        const Status twice = registerWorldComponents(registry);
        REQUIRE_FALSE(twice.has_value());
        CHECK(twice.error().code == ErrorCode::AlreadyExists);
        CHECK(registry.components().size() == 5);
    }

    TEST_CASE("a component type needs a name") {
        ComponentRegistry registry;
        const Status status = registry.add<Health>(TypeBuilder<Health>("").build());
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::InvalidArgument);
        CHECK(registry.components().empty());
    }

    TEST_CASE("descriptors stay where they are when more components are registered") {
        ComponentRegistry registry;
        REQUIRE(registry.add<Health>(describeHealth()).has_value());
        const ComponentDescriptor* health = registry.find("Health");

        REQUIRE(registerWorldComponents(registry).has_value());
        CHECK(registry.find("Health") == health);
        CHECK(health->type.name() == "Health");
    }

    TEST_CASE("a descriptor gives access to instances without knowing the type") {
        ComponentRegistry registry;
        REQUIRE(registry.add<Health>(describeHealth()).has_value());
        const ComponentDescriptor* health = registry.find("Health");
        REQUIRE(health != nullptr);

        World world;
        const Entity entity = world.createEntity();
        entt::registry& storage = world.registry();

        CHECK_FALSE(health->has(storage, entity.handle));
        CHECK(health->get(storage, entity.handle) == nullptr);
        CHECK(health->getConst(storage, entity.handle) == nullptr);

        void* instance = health->add(storage, entity.handle);
        REQUIRE(instance != nullptr);
        CHECK(health->has(storage, entity.handle));
        CHECK(instance == world.tryGet<Health>(entity));
        CHECK(health->get(storage, entity.handle) == instance);
        CHECK(health->getConst(storage, entity.handle) == instance);

        // The instance is edited through the reflected type.
        REQUIRE(health->type.fromJson(instance, json(R"({"points": 25})")).has_value());
        CHECK(world.tryGet<Health>(entity)->points == 25);
        CHECK(health->type.toJson(instance) == json(R"({"points": 25, "invulnerable": false})"));

        // Adding again returns the same instance and keeps its values.
        CHECK(health->add(storage, entity.handle) == instance);
        CHECK(world.tryGet<Health>(entity)->points == 25);

        health->remove(storage, entity.handle);
        CHECK_FALSE(world.has<Health>(entity));
        CHECK_NOTHROW(health->remove(storage, entity.handle));
    }

    TEST_CASE("emplace gives an entity a component made of defaults plus a patch") {
        ComponentRegistry registry;
        REQUIRE(registry.add<Health>(describeHealth()).has_value());
        const ComponentDescriptor* health = registry.find("Health");

        World world;
        const Entity entity = world.createEntity();
        const PropertyPatch patch = unwrap(health->type.parse(json(R"({"points": 40})")));

        health->emplace(world.registry(), entity.handle, patch);
        CHECK(world.tryGet<Health>(entity)->points == 40);
        CHECK_FALSE(world.tryGet<Health>(entity)->invulnerable);

        // An existing instance is replaced, not merged: what the patch leaves out goes
        // back to its default.
        world.tryGet<Health>(entity)->invulnerable = true;
        health->emplace(world.registry(), entity.handle, unwrap(health->type.parse(json(R"({"points": 5})"))));
        CHECK(world.tryGet<Health>(entity)->points == 5);
        CHECK_FALSE(world.tryGet<Health>(entity)->invulnerable);
    }

    TEST_CASE("a component is complete before listeners see it") {
        ComponentRegistry registry;
        REQUIRE(registry.add<Health>(describeHealth()).has_value());
        const ComponentDescriptor* health = registry.find("Health");

        ConstructionProbe probe;
        World world;
        world.registry().on_construct<Health>().connect<&ConstructionProbe::onHealthConstructed>(probe);

        const Entity entity = world.createEntity();
        health->emplace(world.registry(), entity.handle, unwrap(health->type.parse(json(R"({"points": 7})"))));
        CHECK(probe.pointsSeen == std::vector<int32_t>{7});
    }

    TEST_CASE("the built-in components validate what they are given") {
        ComponentRegistry registry;
        REQUIRE(registerWorldComponents(registry).has_value());
        const ComponentDescriptor* camera = registry.find("Camera");
        const ComponentDescriptor* transform = registry.find("Transform");

        World world;
        const Entity entity = world.createEntity();
        void* instance = camera->add(world.registry(), entity.handle);

        REQUIRE(camera->type
                    .fromJson(instance,
                              json(R"({"projection": "orthographic", "orthographicHeight": 4.5, "primary": false})"))
                    .has_value());
        const CameraComponent& component = *world.tryGet<CameraComponent>(entity);
        CHECK(component.projection == CameraProjection::Orthographic);
        CHECK(component.orthographicHeight == 4.5f);
        CHECK_FALSE(component.primary);

        for (const char* text : {R"({"verticalFovDegrees": 0})", R"({"verticalFovDegrees": 180})",
                                 R"({"nearPlane": 0})", R"({"farPlane": -5})", R"({"orthographicHeight": 0})",
                                 R"({"projection": "fisheye"})", R"({"primary": "yes"})"}) {
            CAPTURE(text);
            CHECK_FALSE(camera->type.fromJson(instance, json(text)).has_value());
        }

        // Transform is reached the same way, although every entity has one from the start.
        CHECK(transform->has(world.registry(), entity.handle));
        REQUIRE(transform->type
                    .fromJson(transform->get(world.registry(), entity.handle), json(R"({"position": [1, 2, 3]})"))
                    .has_value());
        CHECK(world.transform(entity).position == Vec3(1.0f, 2.0f, 3.0f));
    }

    TEST_CASE("the camera schema tells tools what can be set") {
        ComponentRegistry registry;
        REQUIRE(registerWorldComponents(registry).has_value());

        const Json schema = registry.find("Camera")->type.describe();
        CHECK(schema["name"] == "Camera");
        REQUIRE(schema["properties"].size() == 6);
        CHECK(schema["properties"][0]["name"] == "projection");
        CHECK(schema["properties"][0]["options"] == json(R"(["perspective", "orthographic"])"));
        CHECK(schema["properties"][1]["name"] == "verticalFovDegrees");
        CHECK(schema["properties"][1]["minimum"] == 1.0);
        CHECK(schema["properties"][1]["maximum"] == 179.0);
        CHECK(schema["properties"][1]["unit"] == "deg");
    }
}
