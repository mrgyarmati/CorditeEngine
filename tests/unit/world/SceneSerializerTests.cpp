#include "cordite/world/SceneSerializer.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/math/MathReflection.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

constexpr const char* kIdA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
constexpr const char* kIdB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

/// A struct nested in Follower, to cover entity references below the top level.
struct Link {
    Uuid target;
};

/// A component that refers to other entities in every way a component can.
struct Follower {
    Uuid leader;
    std::vector<Uuid> friends;
    Link link;
    float distance = 1.0f;
};

const TypeInfo& linkType() {
    static const TypeInfo type = TypeBuilder<Link>("Link")
                                     .property("target", &Link::target, {.semantic = PropertySemantic::EntityReference})
                                     .build();
    return type;
}

TypeInfo describeFollower() {
    return TypeBuilder<Follower>("Follower")
        .property("leader", &Follower::leader, {.semantic = PropertySemantic::EntityReference})
        .property("friends", &Follower::friends, {.semantic = PropertySemantic::EntityReference})
        .objectProperty("link", &Follower::link, linkType())
        .property("distance", &Follower::distance, {.minimum = 0.0})
        .build();
}

ComponentRegistry makeRegistry() {
    ComponentRegistry registry;
    REQUIRE(registerWorldComponents(registry).has_value());
    REQUIRE(registry.add<Follower>(describeFollower()).has_value());
    return registry;
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

Uuid uuid(std::string_view text) {
    const std::optional<Uuid> parsed = Uuid::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// Records what a listener can see of an entity at the moment it gets its camera.
struct CameraProbe {
    const World* world = nullptr;
    std::vector<std::string> seen;

    void onCameraConstructed(entt::registry& registry, entt::entity handle) {
        const Entity entity{handle};
        const Entity parent = world->parent(entity);
        const int fov = static_cast<int>(registry.get<CameraComponent>(handle).verticalFovDegrees);
        seen.push_back(world->name(entity) + " under " + (parent.isNull() ? "nothing" : world->name(parent)) + " fov " +
                       std::to_string(fov) + (world->isEnabled(entity) ? "" : " disabled"));
    }
};

/// A scene document around the given "entities" array.
Json sceneWith(std::string_view entities) {
    return json(
        fmt::format(R"({{"format": "cordite.scene", "version": 1, "name": "Test", "entities": {}}})", entities));
}

/// The names of all entities in traversal order, indented by depth with dots.
std::vector<std::string> outline(const World& world) {
    std::vector<std::string> lines;
    world.traverse(
        [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
    return lines;
}

/// Fills a world with a small hierarchy that uses every kind of data a scene holds:
///
///     Room
///       Table            (moved, rotated, scaled)
///         Lamp           (disabled, follows Table and Chair)
///       Chair
///     Eye                (camera)
struct SampleScene {
    Entity room;
    Entity table;
    Entity lamp;
    Entity chair;
    Entity eye;

    explicit SampleScene(World& world) {
        world.setSceneName("Depot");
        room = world.createEntity("Room");
        table = world.createEntity("Table");
        lamp = world.createEntity("Lamp");
        chair = world.createEntity("Chair");
        eye = world.createEntity("Eye");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        REQUIRE(world.setParent(chair, room).has_value());

        world.transform(table).position = Vec3(1.5f, 0.0f, -2.25f);
        world.transform(table).rotation = glm::angleAxis(toRadians(30.0f), kWorldUp);
        world.transform(table).scale = Vec3(1.0f, 0.5f, 1.0f);
        world.transform(lamp).position = Vec3(0.1f, 0.7f, 0.0f);
        world.setEnabled(lamp, false);

        Follower& follower = world.add<Follower>(lamp);
        follower.leader = world.id(table);
        follower.friends = {world.id(chair), world.id(room)};
        follower.link.target = world.id(chair);
        follower.distance = 2.5f;

        world.transform(eye).position = Vec3(0.0f, 1.7f, 4.0f);
        CameraComponent& camera = world.add<CameraComponent>(eye);
        camera.verticalFovDegrees = 70.0f;
        camera.nearPlane = 0.05f;
    }
};

} // namespace

TEST_SUITE("SceneSerializer") {
    TEST_CASE("a scene document lists entities parents first with their components") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);

        const Json document = serializeScene(world, registry);
        CHECK(document["format"] == "cordite.scene");
        CHECK(document["version"] == kSceneFormatVersion);
        CHECK(document["name"] == "Depot");

        const Json& entities = document["entities"];
        REQUIRE(entities.size() == 5);
        std::vector<std::string> names;
        for (const Json& entity : entities) {
            names.push_back(entity["name"].get<std::string>());
        }
        CHECK(names == std::vector<std::string>{"Room", "Table", "Lamp", "Chair", "Eye"});

        const Json& room = entities[0];
        CHECK(room["id"] == world.id(scene.room).toString());
        CHECK(room["parent"].is_null());
        CHECK(room["enabled"] == true);
        // Every entity has a Transform; nothing else was added to this one.
        CHECK(room["components"].size() == 1);
        CHECK(room["components"]["Transform"] ==
              json(R"({"position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]})"));

        const Json& lamp = entities[2];
        CHECK(lamp["parent"] == world.id(scene.table).toString());
        CHECK(lamp["enabled"] == false);
        CHECK(lamp["components"]["Transform"]["position"] == json("[0.1, 0.7, 0.0]"));
        CHECK(lamp["components"]["Follower"]["leader"] == world.id(scene.table).toString());
        CHECK(lamp["components"]["Follower"]["friends"].size() == 2);
        CHECK(lamp["components"]["Follower"]["link"]["target"] == world.id(scene.chair).toString());

        const Json& eye = entities[4];
        CHECK(eye["components"]["Camera"]["projection"] == "perspective");
        CHECK(eye["components"]["Camera"]["verticalFovDegrees"] == 70.0);
        // Components appear in registration order.
        CHECK(eye["components"].begin().key() == "Transform");
    }

    TEST_CASE("an empty world is a valid scene") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const Json document = serializeScene(world, registry);
        CHECK(document["entities"] == Json::array());
        CHECK(document["name"] == "Untitled");

        World loaded;
        loaded.createEntity("Leftover");
        REQUIRE(deserializeScene(loaded, document, registry).has_value());
        CHECK(loaded.entityCount() == 0);
    }

    TEST_CASE("a scene survives a round trip unchanged") {
        const ComponentRegistry registry = makeRegistry();
        World original;
        const SampleScene scene(original);
        const Json document = serializeScene(original, registry);

        World loaded;
        const Status status = deserializeScene(loaded, document, registry);
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));

        CHECK(serializeScene(loaded, registry) == document);
        CHECK(loaded.sceneName() == "Depot");
        CHECK(outline(loaded) == std::vector<std::string>{"Room", ".Table", "..Lamp", ".Chair", "Eye"});

        // Identities survive, so references between entities still resolve.
        const Entity lamp = loaded.findById(original.id(scene.lamp));
        REQUIRE(loaded.isValid(lamp));
        CHECK_FALSE(loaded.isEnabled(lamp));
        const Follower* follower = loaded.tryGet<Follower>(lamp);
        REQUIRE(follower != nullptr);
        CHECK(loaded.name(loaded.findById(follower->leader)) == "Table");
        CHECK(follower->distance == 2.5f);

        // Values survive exactly, not approximately.
        const Entity table = loaded.findById(original.id(scene.table));
        CHECK(loaded.transform(table) == original.transform(scene.table));
        const Entity eye = loaded.findByName("Eye");
        CHECK(loaded.tryGet<CameraComponent>(eye)->nearPlane == 0.05f);
    }

    TEST_CASE("sibling order survives a round trip") {
        const ComponentRegistry registry = makeRegistry();
        World original;
        const Entity parent = original.createEntity("Parent");
        for (const char* name : {"C", "A", "B"}) {
            REQUIRE(original.setParent(original.createEntity(name), parent).has_value());
        }
        original.setSiblingIndex(original.createEntity("First"), 0);

        World loaded;
        REQUIRE(deserializeScene(loaded, serializeScene(original, registry), registry).has_value());
        CHECK(outline(loaded) == std::vector<std::string>{"First", "Parent", ".C", ".A", ".B"});
    }

    TEST_CASE("loading replaces what the world contained") {
        const ComponentRegistry registry = makeRegistry();
        World source;
        const SampleScene scene(source);

        World world;
        world.setSceneName("Old");
        const Entity old = world.createEntity("Old entity");

        REQUIRE(deserializeScene(world, serializeScene(source, registry), registry).has_value());
        CHECK_FALSE(world.isValid(old));
        CHECK(world.findByName("Old entity").isNull());
        CHECK(world.entityCount() == 5);
        CHECK(world.sceneName() == "Depot");
    }

    TEST_CASE("scenes are saved to and loaded from files") {
        const ComponentRegistry registry = makeRegistry();
        const test::TempDirectory directory;
        const std::filesystem::path path = directory.path() / "depot.scene.json";

        World original;
        const SampleScene scene(original);
        REQUIRE(saveScene(path, original, registry).has_value());

        // The file is ordinary indented JSON that ends with a newline.
        const std::string text = unwrap(readTextFile(path));
        CHECK(text.starts_with("{\n    \"format\": \"cordite.scene\""));
        CHECK(text.ends_with("}\n"));

        World loaded;
        const Status status = loadScene(path, loaded, registry);
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(serializeScene(loaded, registry) == serializeScene(original, registry));

        // Saving what was loaded produces the same bytes, so files do not churn.
        const std::filesystem::path second = directory.path() / "again.scene.json";
        REQUIRE(saveScene(second, loaded, registry).has_value());
        CHECK(unwrap(readTextFile(second)) == text);
    }

    TEST_CASE("a hand-written scene may leave out everything that has a default") {
        const ComponentRegistry registry = makeRegistry();
        const test::TempDirectory directory;
        const std::filesystem::path path = directory.path() / "minimal.scene.json";
        REQUIRE(writeTextFile(path, fmt::format(R"({{
            // Comments are allowed in files.
            "format": "cordite.scene",
            "version": 1,
            "entities": [
                {{ "id": "{0}" }},
                {{
                    "id": "{1}",
                    "name": "Eye",
                    "parent": "{0}",
                    "components": {{
                        "Transform": {{ "position": [0, 1.7, 4] }},
                        "Camera": {{ "verticalFovDegrees": 45 }}
                    }}
                }}
            ]
        }})",
                                                kIdA, kIdB))
                    .has_value());

        World world;
        const Status status = loadScene(path, world, registry);
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(world.sceneName() == "Untitled");

        const Entity first = world.findById(uuid(kIdA));
        REQUIRE(world.isValid(first));
        CHECK(world.name(first) == "Entity");
        CHECK(world.isEnabled(first));
        CHECK(world.transform(first) == Transform());
        CHECK_FALSE(world.has<CameraComponent>(first));

        const Entity eye = world.findById(uuid(kIdB));
        REQUIRE(world.isValid(eye));
        CHECK(world.parent(eye) == first);
        CHECK(world.transform(eye).position == Vec3(0.0f, 1.7f, 4.0f));
        CHECK(world.transform(eye).scale == Vec3(1.0f));
        const CameraComponent* camera = world.tryGet<CameraComponent>(eye);
        REQUIRE(camera != nullptr);
        CHECK(camera->verticalFovDegrees == 45.0f);
        CHECK(camera->farPlane == CameraComponent().farPlane);
    }

    TEST_CASE("loading reports missing and malformed files with their path") {
        const ComponentRegistry registry = makeRegistry();
        const test::TempDirectory directory;
        World world;

        const Status missing = loadScene(directory.path() / "nowhere.scene.json", world, registry);
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().message.find("nowhere.scene.json") != std::string::npos);

        const std::filesystem::path broken = directory.path() / "broken.scene.json";
        REQUIRE(writeTextFile(broken, R"({"format": "cordite.scene", "version": 1, "entities": 5})").has_value());
        const Status invalid = loadScene(broken, world, registry);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::ParseError);
        CHECK(invalid.error().message.find("broken.scene.json") != std::string::npos);
        CHECK(invalid.error().message.find("'entities' must be an array") != std::string::npos);
    }

    TEST_CASE("an invalid scene is rejected and leaves the world untouched") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);
        const Json before = serializeScene(world, registry);

        struct Case {
            const char* what;
            std::string document;
            ErrorCode code;
            const char* fragment;
        };

        const auto entities = [](std::string_view list) {
            return fmt::format(R"({{"format": "cordite.scene", "version": 1, "entities": {}}})", list);
        };
        const std::vector<Case> cases = {
            {"not an object", "[]", ErrorCode::ParseError, "must be a JSON object"},
            {"no format", R"({"version": 1, "entities": []})", ErrorCode::ParseError, "not a Cordite scene"},
            {"another format", R"({"format": "other", "version": 1, "entities": []})", ErrorCode::ParseError,
             "not a Cordite scene"},
            {"format of the wrong type", R"({"format": 1, "version": 1, "entities": []})", ErrorCode::ParseError,
             "format"},
            {"no version", R"({"format": "cordite.scene", "entities": []})", ErrorCode::ParseError, "version"},
            {"version of the wrong type", R"({"format": "cordite.scene", "version": "1", "entities": []})",
             ErrorCode::ParseError, "version"},
            {"fractional version", R"({"format": "cordite.scene", "version": 1.5, "entities": []})",
             ErrorCode::ParseError, "version"},
            {"version zero", R"({"format": "cordite.scene", "version": 0, "entities": []})", ErrorCode::VersionMismatch,
             "version 0"},
            {"newer version", R"({"format": "cordite.scene", "version": 2, "entities": [], "future": {}})",
             ErrorCode::VersionMismatch, "version 2"},
            {"unknown top-level member", R"({"format": "cordite.scene", "version": 1, "entitites": []})",
             ErrorCode::ParseError, "unknown member 'entitites'"},
            {"name of the wrong type", R"({"format": "cordite.scene", "version": 1, "name": 5, "entities": []})",
             ErrorCode::ParseError, "name"},
            {"no entities", R"({"format": "cordite.scene", "version": 1})", ErrorCode::ParseError, "no 'entities'"},
            {"entities not an array", entities("{}"), ErrorCode::ParseError, "must be an array"},
            {"entity not an object", entities("[5]"), ErrorCode::ParseError, "entity #0"},
            {"entity without id", entities(R"([{"name": "Nameless"}])"), ErrorCode::ParseError, "'id'"},
            {"malformed id", entities(R"([{"id": "1234"}])"), ErrorCode::ParseError, "not a valid entity id"},
            {"nil id", entities(R"([{"id": "00000000-0000-0000-0000-000000000000"}])"), ErrorCode::ParseError,
             "not a valid entity id"},
            {"id of the wrong type", entities(R"([{"id": 7}])"), ErrorCode::ParseError, "'id'"},
            {"duplicate id", entities(fmt::format(R"([{{"id": "{0}"}}, {{"id": "{0}"}}])", kIdA)),
             ErrorCode::ParseError, "entity #1 repeats the id"},
            {"unknown entity member", entities(fmt::format(R"([{{"id": "{}", "componets": {{}}}}])", kIdA)),
             ErrorCode::ParseError, "unknown member 'componets'"},
            {"name of the wrong type", entities(fmt::format(R"([{{"id": "{}", "name": 5}}])", kIdA)),
             ErrorCode::ParseError, "'name'"},
            {"enabled of the wrong type", entities(fmt::format(R"([{{"id": "{}", "enabled": "yes"}}])", kIdA)),
             ErrorCode::ParseError, "'enabled'"},
            {"malformed parent", entities(fmt::format(R"([{{"id": "{}", "parent": "above"}}])", kIdA)),
             ErrorCode::ParseError, "'parent' must be an entity id or null"},
            {"parent of the wrong type", entities(fmt::format(R"([{{"id": "{}", "parent": 3}}])", kIdA)),
             ErrorCode::ParseError, "'parent'"},
            {"parent that does not exist", entities(fmt::format(R"([{{"id": "{}", "parent": "{}"}}])", kIdA, kIdB)),
             ErrorCode::ParseError, "does not exist"},
            {"parent listed after its child",
             entities(fmt::format(R"([{{"id": "{0}", "parent": "{1}"}}, {{"id": "{1}"}}])", kIdA, kIdB)),
             ErrorCode::ParseError, "must be listed before it"},
            {"entity that is its own parent", entities(fmt::format(R"([{{"id": "{0}", "parent": "{0}"}}])", kIdA)),
             ErrorCode::ParseError, "its own parent"},
            {"components not an object", entities(fmt::format(R"([{{"id": "{}", "components": []}}])", kIdA)),
             ErrorCode::ParseError, "'components' must be an object"},
            {"unknown component", entities(fmt::format(R"([{{"id": "{}", "components": {{"Jetpack": {{}}}}}}])", kIdA)),
             ErrorCode::NotFound, "unknown component 'Jetpack'"},
            {"component not an object",
             entities(fmt::format(R"([{{"id": "{}", "components": {{"Camera": 5}}}}])", kIdA)),
             ErrorCode::InvalidArgument, "Camera expects an object"},
            {"unknown property",
             entities(fmt::format(R"([{{"id": "{}", "components": {{"Camera": {{"zoom": 2}}}}}}])", kIdA)),
             ErrorCode::InvalidArgument, "Camera has no property 'zoom'"},
            {"property of the wrong type",
             entities(fmt::format(R"([{{"id": "{}", "components": {{"Camera": {{"primary": 1}}}}}}])", kIdA)),
             ErrorCode::InvalidArgument, "property 'primary'"},
            {"property out of range",
             entities(
                 fmt::format(R"([{{"id": "{}", "components": {{"Camera": {{"verticalFovDegrees": 500}}}}}}])", kIdA)),
             ErrorCode::OutOfRange, "property 'verticalFovDegrees'"},
            {"malformed vector",
             entities(fmt::format(R"([{{"id": "{}", "components": {{"Transform": {{"position": [1, 2]}}}}}}])", kIdA)),
             ErrorCode::InvalidArgument, "property 'position'"},
            {"error in a later entity",
             entities(fmt::format(R"([{{"id": "{}"}}, {{"id": "{}", "components": {{"Camera": {{"zoom": 2}}}}}}])",
                                  kIdA, kIdB)),
             ErrorCode::InvalidArgument, "zoom"},
        };

        for (const Case& entry : cases) {
            CAPTURE(entry.what);
            const Status status = deserializeScene(world, json(entry.document), registry);
            REQUIRE_FALSE(status.has_value());
            CAPTURE(status.error().message);
            CHECK(status.error().code == entry.code);
            CHECK(status.error().message.find(entry.fragment) != std::string::npos);
            CHECK(serializeScene(world, registry) == before);
            CHECK(world.isValid(scene.lamp));
        }
    }

    TEST_CASE("an error names the entity it is about") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const Status status = deserializeScene(
            world,
            sceneWith(fmt::format(R"([{{"id": "{}", "name": "Eye", "components": {{"Camera": {{"nearPlane": -1}}}}}}])",
                                  kIdA)),
            registry);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message ==
              fmt::format("entity 'Eye' ({}): Camera: property 'nearPlane': -1 is below the minimum 0.001", kIdA));
    }

    TEST_CASE("properties a component leaves out get their defaults, not earlier values") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const Entity eye = unwrap(world.createEntityWithId(uuid(kIdA), "Eye"));
        world.add<CameraComponent>(eye).farPlane = 50.0f;
        world.transform(eye).scale = Vec3(3.0f);

        REQUIRE(
            deserializeScene(
                world,
                sceneWith(fmt::format(
                    R"([{{"id": "{}", "components": {{"Camera": {{"nearPlane": 1}}, "Transform": {{}}}}}}])", kIdA)),
                registry)
                .has_value());

        const Entity loaded = world.findById(uuid(kIdA));
        CHECK(world.tryGet<CameraComponent>(loaded)->nearPlane == 1.0f);
        CHECK(world.tryGet<CameraComponent>(loaded)->farPlane == CameraComponent().farPlane);
        CHECK(world.transform(loaded) == Transform());
    }

    TEST_CASE("listeners see finished components on entities already in place") {
        const ComponentRegistry registry = makeRegistry();
        // Declared before the world so that it outlives it.
        CameraProbe probe;
        World world;
        probe.world = &world;
        world.registry().on_construct<CameraComponent>().connect<&CameraProbe::onCameraConstructed>(probe);

        REQUIRE(deserializeScene(world,
                                 sceneWith(fmt::format(R"([
                                     {{"id": "{0}", "name": "Rig"}},
                                     {{"id": "{1}", "name": "Eye", "parent": "{0}", "enabled": false,
                                       "components": {{"Camera": {{"verticalFovDegrees": 33}}}}}}
                                 ])",
                                                       kIdA, kIdB)),
                                 registry)
                    .has_value());
        CHECK(probe.seen == std::vector<std::string>{"Eye under Rig fov 33 disabled"});
    }

    TEST_CASE("serializeEntities writes the listed subtrees once each") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);

        const auto names = [](const Json& entities) {
            std::vector<std::string> result;
            for (const Json& entity : entities) {
                result.push_back(entity["name"].get<std::string>());
            }
            return result;
        };

        const Entity table[] = {scene.table};
        const Json subtree = serializeEntities(world, table, registry);
        CHECK(names(subtree) == std::vector<std::string>{"Table", "Lamp"});
        // The top-level entity remembers where it was attached.
        CHECK(subtree[0]["parent"] == world.id(scene.room).toString());
        CHECK(subtree[1]["parent"] == world.id(scene.table).toString());

        // Lamp is below Table and Table is listed twice: neither is written again.
        const Entity overlapping[] = {scene.lamp, scene.table, scene.eye, scene.table, Entity{}};
        CHECK(names(serializeEntities(world, overlapping, registry)) ==
              std::vector<std::string>{"Table", "Lamp", "Eye"});

        CHECK(serializeEntities(world, {}, registry) == Json::array());

        World other;
        const Entity foreign[] = {other.createEntity("Elsewhere")};
        other.destroyEntity(foreign[0]);
        CHECK(serializeEntities(other, foreign, registry) == Json::array());
    }

    TEST_CASE("instantiating without new ids puts entities back where they were") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);
        const Json before = serializeScene(world, registry);

        const Entity table[] = {scene.table};
        const Json snapshot = serializeEntities(world, table, registry);
        const size_t index = world.siblingIndex(scene.table);
        world.destroyEntity(scene.table);
        CHECK(world.entityCount() == 3);

        const std::vector<Entity> restored = unwrap(instantiateEntities(world, snapshot, registry));
        REQUIRE(restored.size() == 2);
        CHECK(world.name(restored[0]) == "Table");
        CHECK(world.name(restored[1]) == "Lamp");
        CHECK(world.parent(restored[0]) == scene.room);
        CHECK(world.parent(restored[1]) == restored[0]);

        // New entities are appended; with the sibling position put back as well, the
        // world is exactly what it was. This is what undoing a deletion does.
        CHECK(outline(world) == std::vector<std::string>{"Room", ".Chair", ".Table", "..Lamp", "Eye"});
        world.setSiblingIndex(restored[0], index);
        CHECK(serializeScene(world, registry) == before);
    }

    TEST_CASE("an entity whose recorded parent is gone becomes a root") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);

        const Entity lamp[] = {scene.lamp};
        const Json snapshot = serializeEntities(world, lamp, registry);
        world.destroyEntity(scene.table);

        const std::vector<Entity> restored = unwrap(instantiateEntities(world, snapshot, registry));
        REQUIRE(restored.size() == 1);
        CHECK(world.parent(restored[0]).isNull());
        CHECK(world.transform(restored[0]).position == Vec3(0.1f, 0.7f, 0.0f));
    }

    TEST_CASE("identifiers in use cannot be instantiated again") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);
        const Json before = serializeScene(world, registry);

        // The second entity of the snapshot still exists; the first does not.
        const Entity table[] = {scene.table};
        Json snapshot = serializeEntities(world, table, registry);
        snapshot[0]["id"] = kIdA;
        snapshot[1]["parent"] = kIdA;

        const Result<std::vector<Entity>> result = instantiateEntities(world, snapshot, registry);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::AlreadyExists);
        CHECK(result.error().message.find("Lamp") != std::string::npos);
        CHECK(serializeScene(world, registry) == before);
    }

    TEST_CASE("duplicating gives new identities and redirects references inside the group") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);

        const Entity room[] = {scene.room};
        const Json snapshot = serializeEntities(world, room, registry);
        const std::vector<Entity> copies =
            unwrap(instantiateEntities(world, snapshot, registry, {.generateNewIds = true}));

        // Room, Table, Lamp, Chair - in that order, like the originals.
        REQUIRE(copies.size() == 4);
        CHECK(world.entityCount() == 9);
        const Entity roomCopy = copies[0];
        const Entity tableCopy = copies[1];
        const Entity lampCopy = copies[2];
        const Entity chairCopy = copies[3];

        for (size_t index = 0; index < copies.size(); ++index) {
            CAPTURE(index);
            const Entity original = world.findById(uuid(snapshot[index]["id"].get<std::string>()));
            CHECK(copies[index] != original);
            CHECK(world.id(copies[index]) != world.id(original));
            CHECK(world.name(copies[index]) == world.name(original));
            CHECK(world.transform(copies[index]) == world.transform(original));
        }

        // The copy has the same shape as the original and sits next to it.
        CHECK(world.parent(roomCopy).isNull());
        CHECK(world.parent(tableCopy) == roomCopy);
        CHECK(world.parent(lampCopy) == tableCopy);
        CHECK(world.parent(chairCopy) == roomCopy);
        CHECK(world.roots() == std::vector<Entity>{scene.room, scene.eye, roomCopy});
        CHECK_FALSE(world.isEnabled(lampCopy));

        // References to entities that were copied now point at the copies...
        const Follower* follower = world.tryGet<Follower>(lampCopy);
        REQUIRE(follower != nullptr);
        CHECK(follower->leader == world.id(tableCopy));
        CHECK(follower->friends == std::vector<Uuid>{world.id(chairCopy), world.id(roomCopy)});
        CHECK(follower->link.target == world.id(chairCopy));
        CHECK(follower->distance == 2.5f);

        // ...and the original is untouched.
        const Follower* original = world.tryGet<Follower>(scene.lamp);
        CHECK(original->leader == world.id(scene.table));
        CHECK(original->link.target == world.id(scene.chair));
    }

    TEST_CASE("references to entities outside the duplicated group stay as they are") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);

        // Only Lamp is copied; Table, Chair and Room are not part of the group.
        const Entity lamp[] = {scene.lamp};
        const std::vector<Entity> copies = unwrap(
            instantiateEntities(world, serializeEntities(world, lamp, registry), registry, {.generateNewIds = true}));
        REQUIRE(copies.size() == 1);

        const Follower* follower = world.tryGet<Follower>(copies[0]);
        REQUIRE(follower != nullptr);
        CHECK(follower->leader == world.id(scene.table));
        CHECK(follower->friends == std::vector<Uuid>{world.id(scene.chair), world.id(scene.room)});
        // A duplicate appears beside its original.
        CHECK(world.children(scene.table) == std::vector<Entity>{scene.lamp, copies[0]});
    }

    TEST_CASE("the caller can choose where instantiated entities are attached") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);
        const Entity table[] = {scene.table};
        const Json snapshot = serializeEntities(world, table, registry);

        const std::vector<Entity> asRoots =
            unwrap(instantiateEntities(world, snapshot, registry, {.parent = Entity{}, .generateNewIds = true}));
        CHECK(world.parent(asRoots[0]).isNull());
        // Only the top-level entity is moved; the hierarchy below it is kept.
        CHECK(world.parent(asRoots[1]) == asRoots[0]);

        const std::vector<Entity> underEye =
            unwrap(instantiateEntities(world, snapshot, registry, {.parent = scene.eye, .generateNewIds = true}));
        CHECK(world.parent(underEye[0]) == scene.eye);
        CHECK(world.parent(underEye[1]) == underEye[0]);
        // Local transforms are kept as recorded.
        CHECK(world.transform(underEye[0]) == world.transform(scene.table));

        const size_t count = world.entityCount();
        Entity stale = world.createEntity("Stale");
        world.destroyEntity(stale);
        const Result<std::vector<Entity>> invalid =
            instantiateEntities(world, snapshot, registry, {.parent = stale, .generateNewIds = true});
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error().code == ErrorCode::InvalidArgument);
        CHECK(world.entityCount() == count);
    }

    TEST_CASE("a failed instantiation creates nothing") {
        const ComponentRegistry registry = makeRegistry();
        World world;
        const SampleScene scene(world);
        const Json before = serializeScene(world, registry);

        const Json broken = json(fmt::format(R"([
            {{"id": "{}", "name": "Fine"}},
            {{"id": "{}", "name": "Broken", "components": {{"Follower": {{"distance": -1}}}}}}
        ])",
                                             kIdA, kIdB));

        for (const bool generateNewIds : {false, true}) {
            CAPTURE(generateNewIds);
            const Result<std::vector<Entity>> result =
                instantiateEntities(world, broken, registry, {.generateNewIds = generateNewIds});
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == ErrorCode::OutOfRange);
            CHECK(result.error().message.find("entity 'Broken'") != std::string::npos);
            CHECK(serializeScene(world, registry) == before);
        }

        CHECK_FALSE(instantiateEntities(world, json("{}"), registry).has_value());
    }

    TEST_CASE("many entities load in order") {
        const ComponentRegistry registry = makeRegistry();
        World original;
        const Entity root = original.createEntity("Root");
        constexpr size_t kCount = 2000;
        for (size_t index = 0; index < kCount; ++index) {
            const Entity entity = original.createEntity(fmt::format("Entity {}", index));
            original.transform(entity).position = Vec3(static_cast<float>(index), 0.0f, 0.0f);
            if (index % 3 == 0) {
                REQUIRE(original.setParent(entity, root, false).has_value());
            }
        }

        World loaded;
        REQUIRE(deserializeScene(loaded, serializeScene(original, registry), registry).has_value());
        CHECK(loaded.entityCount() == kCount + 1);
        CHECK(outline(loaded) == outline(original));
        CHECK(loaded.transform(loaded.findByName("Entity 1999")).position.x == 1999.0f);
    }
}
