#include "cordite/world/Prefab.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::sameRotation;
using cordite::test::unwrap;

namespace {

/// A component that refers to another entity: what is tied to what.
struct Tether {
    Uuid to;
    float length = 1.0f;
};

TypeInfo describeTether() {
    return TypeBuilder<Tether>("Tether")
        .property("to", &Tether::to, {.semantic = PropertySemantic::EntityReference})
        .property("length", &Tether::length, {.minimum = 0.0})
        .build();
}

const Uuid kCartPrefab = *Uuid::parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
const Uuid kOtherPrefab = *Uuid::parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd");

/// A yard with a cart in it: a body with two wheels under it, the back one tied to the
/// front one, and the cart itself tied to a post that is no part of it.
struct Yard {
    World world;
    ComponentRegistry registry;
    Entity yard;
    Entity post;
    Entity cart;
    Entity front;
    Entity back;

    Yard() {
        REQUIRE(registerWorldComponents(registry).has_value());
        REQUIRE(registry.add<Tether>(describeTether()).has_value());
        yard = world.createEntity("Yard");
        post = world.createEntity("Post");
        cart = world.createEntity("Cart");
        front = world.createEntity("Front wheel");
        back = world.createEntity("Back wheel");
        REQUIRE(world.setParent(cart, yard).has_value());
        REQUIRE(world.setParent(front, cart).has_value());
        REQUIRE(world.setParent(back, cart).has_value());
        world.transform(cart).position = Vec3(4.0f, 0.5f, -2.0f);
        world.transform(front).position = Vec3(0.0f, -0.25f, 1.0f);
        world.transform(back).position = Vec3(0.0f, -0.25f, -1.0f);
        world.add<Tether>(back) = {.to = world.id(front), .length = 2.0f};
        world.add<Tether>(cart) = {.to = world.id(post), .length = 5.0f};
        world.add<CameraComponent>(front).verticalFovDegrees = 40.0f;
    }

    /// What a prefab made of the cart holds.
    [[nodiscard]] Json cartEntities() const { return serializePrefabEntities(world, registry, cart); }

    /// The names of everything in the world, in the order of the hierarchy, indented.
    [[nodiscard]] std::vector<std::string> outline() const {
        std::vector<std::string> lines;
        world.traverse(
            [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
        return lines;
    }
};

/// The error of a result that has to be one.
template <typename T>
Error errorIn(const Result<T>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_SUITE("Prefab") {
    TEST_CASE("a prefab is the entities below its root, the root first and hanging under nothing") {
        const Yard yard;
        const World& world = yard.world;
        const Json entities = yard.cartEntities();
        REQUIRE(entities.is_array());
        REQUIRE(entities.size() == 3);

        // The cart hangs under the yard; in the prefab it hangs under nothing, and its
        // wheels under it.
        CHECK(entities[0]["name"] == "Cart");
        CHECK(entities[0]["id"] == world.id(yard.cart).toString());
        CHECK(entities[0]["parent"].is_null());
        CHECK(entities[1]["name"] == "Front wheel");
        CHECK(entities[1]["parent"] == world.id(yard.cart).toString());
        CHECK(entities[2]["name"] == "Back wheel");
        CHECK(entities[2]["parent"] == world.id(yard.cart).toString());
        // Everything they are made of is there, as a scene would have it.
        CHECK(entities[0]["components"]["Transform"]["position"] == unwrap(parseJson("[4.0, 0.5, -2.0]")));
        CHECK(entities[1]["components"]["Camera"]["verticalFovDegrees"] == 40.0);
        CHECK(entities[2]["components"]["Tether"]["to"] == world.id(yard.front).toString());
        // What the cart is tied to outside itself is still named: it is found again, or
        // not, where the prefab is placed.
        CHECK(entities[0]["components"]["Tether"]["to"] == world.id(yard.post).toString());
        // The world it was made of is as it was.
        CHECK(world.parent(yard.cart) == yard.yard);

        // An entity that hangs under nothing, and has nothing under it, is a prefab too.
        const Json post = serializePrefabEntities(world, yard.registry, yard.post);
        REQUIRE(post.size() == 1);
        CHECK(post[0]["name"] == "Post");
    }

    TEST_CASE("a prefab is not an instance of another: the mark of its root is left out") {
        Yard yard;
        yard.world.add<PrefabInstanceComponent>(yard.cart).prefab = kOtherPrefab;
        yard.world.add<PrefabInstanceComponent>(yard.back).prefab = kOtherPrefab;
        const Json entities = yard.cartEntities();
        CHECK_FALSE(entities[0]["components"].contains("PrefabInstance"));
        // What is placed from a prefab inside it stays marked: that is where it came from.
        CHECK(entities[2]["components"]["PrefabInstance"]["prefab"] == kOtherPrefab.toString());
        // The entity it was made of keeps its mark.
        CHECK(yard.world.tryGet<PrefabInstanceComponent>(yard.cart)->prefab == kOtherPrefab);
    }

    TEST_CASE("placing a prefab makes copies with ids of their own, tied to one another as the originals were") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const size_t before = world.entityCount();

        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        CHECK(world.entityCount() == before + 3);
        CHECK(copy != yard.cart);
        CHECK(world.name(copy) == "Cart");
        // A root, since nobody said under what; and where the prefab has it.
        CHECK(world.parent(copy).isNull());
        CHECK(world.transform(copy).position == Vec3(4.0f, 0.5f, -2.0f));
        const std::vector<Entity> wheels = world.children(copy);
        REQUIRE(wheels.size() == 2);
        CHECK(world.name(wheels[0]) == "Front wheel");
        CHECK(world.name(wheels[1]) == "Back wheel");
        CHECK(world.transform(wheels[1]).position == Vec3(0.0f, -0.25f, -1.0f));
        CHECK(world.tryGet<CameraComponent>(wheels[0])->verticalFovDegrees == 40.0f);

        // Every one of them has an id nothing had before.
        std::set<Uuid> ids;
        world.traverse([&](Entity entity, uint32_t /*depth*/) { ids.insert(world.id(entity)); });
        CHECK(ids.size() == before + 3);
        // The back wheel of the copy is tied to the front wheel of the copy, not to the
        // one it was copied from - and the original to its own, as before.
        CHECK(world.tryGet<Tether>(wheels[1])->to == world.id(wheels[0]));
        CHECK(world.tryGet<Tether>(wheels[1])->length == 2.0f);
        CHECK(world.tryGet<Tether>(yard.back)->to == world.id(yard.front));
        // What it is tied to outside itself is the same post for both.
        CHECK(world.tryGet<Tether>(copy)->to == world.id(yard.post));

        // The root says where it came from; what hangs under it does not.
        REQUIRE(world.has<PrefabInstanceComponent>(copy));
        CHECK(world.tryGet<PrefabInstanceComponent>(copy)->prefab == kCartPrefab);
        CHECK_FALSE(world.has<PrefabInstanceComponent>(wheels[0]));
        CHECK_FALSE(world.has<PrefabInstanceComponent>(yard.cart));

        // Placed again, it is another one again.
        const Entity second = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        CHECK(second != copy);
        CHECK(world.id(second) != world.id(copy));
        CHECK(world.entityCount() == before + 6);
        CHECK(yard.outline() == std::vector<std::string>{"Yard", ".Cart", "..Front wheel", "..Back wheel", "Post",
                                                         "Cart", ".Front wheel", ".Back wheel", "Cart", ".Front wheel",
                                                         ".Back wheel"});
    }

    TEST_CASE("a prefab is placed under what it is told, where it is told, by the name it is told") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();

        // Under an entity: its last child.
        const Entity under =
            unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab, {.parent = yard.yard}));
        CHECK(world.parent(under) == yard.yard);
        CHECK(world.children(yard.yard) == std::vector<Entity>{yard.cart, under});
        // As it is in the prefab, relative to what it hangs under now.
        CHECK(world.transform(under).position == Vec3(4.0f, 0.5f, -2.0f));
        CHECK(world.name(under) == "Cart");

        // Named, put and turned: the turn is made a rotation of length one.
        const Quat quarter = glm::angleAxis(glm::radians(90.0f), Vec3(0.0f, 1.0f, 0.0f));
        const Entity placed = unwrap(instantiatePrefab(
            world, yard.registry, entities, kCartPrefab,
            {.parent = yard.post,
             .name = "Wagon",
             .position = Vec3(1.0f, 2.0f, 3.0f),
             .rotation = Quat(quarter.w * 4.0f, quarter.x * 4.0f, quarter.y * 4.0f, quarter.z * 4.0f)}));
        CHECK(world.name(placed) == "Wagon");
        CHECK(world.parent(placed) == yard.post);
        CHECK(world.transform(placed).position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(sameRotation(world.transform(placed).rotation, quarter, 1.0e-6f));
        CHECK(glm::length(world.transform(placed).rotation) == doctest::Approx(1.0f).epsilon(1.0e-6));
        // What hangs under it is where it was, relative to it.
        CHECK(world.transform(world.children(placed)[0]).position == Vec3(0.0f, -0.25f, 1.0f));
        CHECK(world.name(world.children(placed)[0]) == "Front wheel");

        // A turn that is a rotation already is the entity's as it was given, to the last
        // bit: an eighth about the post, which normalizing once more would change.
        const Quat eighth(0.9238795f, 0.0f, 0.38268343f, 0.0f);
        const Entity turned =
            unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab, {.rotation = eighth}));
        CHECK(world.transform(turned).rotation == eighth);
        CHECK(world.transform(turned).position == Vec3(4.0f, 0.5f, -2.0f));

        // Only the position: it is turned as the prefab has it.
        world.transform(yard.cart).rotation = quarter;
        const Entity moved = unwrap(instantiatePrefab(world, yard.registry, yard.cartEntities(), kCartPrefab,
                                                      {.position = Vec3(0.0f, 0.0f, 9.0f)}));
        CHECK(world.transform(moved).position == Vec3(0.0f, 0.0f, 9.0f));
        CHECK(world.transform(moved).rotation == quarter);
    }

    TEST_CASE("the mark of an instance names the prefab it was placed from, whatever its entities say") {
        Yard yard;
        World& world = yard.world;
        // A prefab file somebody wrote by hand, with the mark of another prefab on its root.
        Json entities = yard.cartEntities();
        entities[0]["components"]["PrefabInstance"]["prefab"] = kOtherPrefab.toString();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        CHECK(world.tryGet<PrefabInstanceComponent>(copy)->prefab == kCartPrefab);
    }

    TEST_CASE("what is no prefab is not placed, and nothing of it stays") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const size_t before = world.entityCount();
        const std::vector<std::string> outline = yard.outline();
        const auto refused = [&](const Json& prefab, const PrefabInstanceOptions& options = {}) {
            const Error error = errorIn(instantiatePrefab(world, yard.registry, prefab, kCartPrefab, options));
            CHECK(world.entityCount() == before);
            CHECK(yard.outline() == outline);
            return error;
        };

        // Two entities that hang under nothing, and none.
        Json twoRoots = entities;
        twoRoots[2]["parent"] = nullptr;
        Error error = refused(twoRoots);
        CHECK(error.code == ErrorCode::ParseError);
        CHECK(error.message ==
              "the entities must have one root with all the others below it, but 2 of them have no parent among them");
        error = refused(Json::array());
        CHECK(error.code == ErrorCode::ParseError);
        CHECK(error.message ==
              "the entities must have one root with all the others below it, but 0 of them have no parent among them");

        // Entities that are wrong, as a scene would be wrong.
        Json unknown = entities;
        unknown[1]["components"]["Engine"] = Json::object();
        CHECK_FALSE(refused(unknown).message.empty());
        Json outOfRange = entities;
        outOfRange[2]["components"]["Tether"]["length"] = -1.0;
        CHECK_FALSE(refused(outOfRange).message.empty());
        CHECK_FALSE(refused(unwrap(parseJson(R"({"entities": []})"))).message.empty());
        CHECK_FALSE(refused(unwrap(parseJson(R"(["Cart"])"))).message.empty());
        Json sameTwice = entities;
        sameTwice[2]["id"] = sameTwice[1]["id"];
        CHECK_FALSE(refused(sameTwice).message.empty());

        // A place that is none.
        const Entity gone = world.createEntity("Gone");
        world.destroyEntity(gone);
        error = refused(entities, {.parent = gone});
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "the parent for the new entities is not a valid entity");
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        error = refused(entities, {.position = Vec3(0.0f, notANumber, 0.0f)});
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "the position of a prefab instance must be finite");
        for (const Quat& rotation : {Quat(0.0f, 0.0f, 0.0f, 0.0f), Quat(notANumber, 0.0f, 0.0f, 0.0f),
                                     Quat(1.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f)}) {
            error = refused(entities, {.rotation = rotation});
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message == "the rotation of a prefab instance must be finite and must not be zero");
        }

        // And after all that, the prefab that is one is still placed.
        CHECK(instantiatePrefab(world, yard.registry, entities, kCartPrefab).has_value());
    }

    TEST_CASE("the mark of a prefab instance is a component like the others") {
        ComponentRegistry registry;
        REQUIRE(registerWorldComponents(registry).has_value());
        const ComponentDescriptor* descriptor = registry.find("PrefabInstance");
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->traits.optional);
        CHECK_FALSE(descriptor->traits.description.empty());
        const TypeInfo& type = descriptor->type;
        REQUIRE(type.properties().size() == 1);
        const PropertyInfo* prefab = type.findProperty("prefab");
        REQUIRE(prefab != nullptr);
        CHECK(prefab->attributes().semantic == PropertySemantic::AssetReference);
        CHECK(prefab->attributes().referenceType == "Prefab");
        CHECK_FALSE(prefab->attributes().description.empty());

        PrefabInstanceComponent mark;
        CHECK(type.toJson(&mark) == unwrap(parseJson(R"({"prefab": null})")));
        mark.prefab = kCartPrefab;
        const Json saved = type.toJson(&mark);
        CHECK(saved == unwrap(parseJson(R"({"prefab": "cccccccc-cccc-4ccc-8ccc-cccccccccccc"})")));
        PrefabInstanceComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.prefab == kCartPrefab);
        CHECK_FALSE(type.fromJson(&loaded, unwrap(parseJson(R"({"prefab": "prefabs/cart.prefab.json"})"))).has_value());
    }

    TEST_CASE("every copy is linked to the entity of the prefab it was made of, and a prefab holds no links") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        // The prefab's entities have the ids of what it was made of.
        const auto sourceOf = [&world](Entity entity) {
            const auto* link = world.tryGet<PrefabLinkComponent>(entity);
            REQUIRE(link != nullptr);
            CHECK(link->overrides.empty());
            return link->source;
        };
        CHECK(sourceOf(copy) == world.id(yard.cart));
        std::set<Uuid> wheels;
        for (const Entity wheel : world.children(copy)) {
            wheels.insert(sourceOf(wheel));
        }
        CHECK(wheels == std::set<Uuid>{world.id(yard.front), world.id(yard.back)});

        // A prefab made of an instance is no copy of anything.
        const Json made = serializePrefabEntities(world, yard.registry, copy);
        for (const Json& entity : made) {
            CHECK_FALSE(entity["components"].contains("PrefabLink"));
        }
    }

    TEST_CASE("an instance takes what its prefab is changed to, but for what it keeps of its own") {
        Yard yard;
        World& world = yard.world;
        Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab,
                                                     {.name = "My cart", .position = Vec3(9.0f, 0.0f, 0.0f)}));
        const auto wheel = [&](std::string_view name) {
            for (const Entity child : world.children(copy)) {
                if (world.name(child) == name) {
                    return child;
                }
            }
            FAIL("no wheel " << std::string(name));
            return Entity{};
        };
        const Entity front = wheel("Front wheel");
        const Entity back = wheel("Back wheel");
        // The back wheel keeps a length of its own, and a camera the prefab does not have.
        world.tryGet<Tether>(back)->length = 3.0f;
        world.add<CameraComponent>(back).verticalFovDegrees = 30.0f;
        world.tryGet<PrefabLinkComponent>(back)->overrides = {"Tether.length", "+Camera"};

        // The prefab changes: its root moves and is renamed, the front wheel is renamed, sees
        // wider and is switched off, the back one is tied more loosely.
        const auto prefabEntity = [&entities](std::string_view name) -> Json& {
            for (Json& entity : entities) {
                if (entity["name"].get<std::string>() == name) {
                    return entity;
                }
            }
            FAIL("no entity " << std::string(name));
            return entities[0];
        };
        prefabEntity("Cart")["components"]["Transform"]["position"] = Json::array({1, 2, 3});
        prefabEntity("Cart")["name"] = "Wagon";
        prefabEntity("Front wheel")["components"]["Camera"]["verticalFovDegrees"] = 80.0;
        prefabEntity("Front wheel")["enabled"] = false;
        prefabEntity("Front wheel")["name"] = "Leading wheel";
        prefabEntity("Back wheel")["components"]["Tether"]["length"] = 6.0;

        const PrefabSync sync = unwrap(syncPrefabInstance(world, yard.registry, copy, entities));
        CHECK(sync.changed());
        CHECK(sync.entitiesCreated == 0);
        CHECK(sync.entitiesDestroyed == 0);
        CHECK(sync.componentsAdded == 0);
        CHECK(sync.componentsRemoved == 0);
        // The root keeps where it was put and what it was called.
        CHECK(world.transform(copy).position == Vec3(9.0f, 0.0f, 0.0f));
        CHECK(world.name(copy) == "My cart");
        CHECK(world.name(front) == "Leading wheel");
        CHECK_FALSE(world.isEnabled(front));
        CHECK(world.tryGet<CameraComponent>(front)->verticalFovDegrees == 80.0f);
        CHECK(world.tryGet<Tether>(back)->length == 3.0f);
        CHECK(world.tryGet<CameraComponent>(back) != nullptr);

        // Brought up to date, it is up to date.
        CHECK_FALSE(unwrap(syncPrefabInstance(world, yard.registry, copy, entities)).changed());

        // A component the prefab gains is gained; one it loses is lost, unless kept.
        prefabEntity("Leading wheel")["components"].erase("Camera");
        prefabEntity("Back wheel")["components"]["Camera"] = Json::object();
        world.tryGet<PrefabLinkComponent>(front)->overrides = {};
        world.tryGet<PrefabLinkComponent>(back)->overrides = {"-Tether"};
        world.remove<Tether>(back);
        const PrefabSync components = unwrap(syncPrefabInstance(world, yard.registry, copy, entities));
        CHECK(components.componentsRemoved == 1);
        CHECK(world.tryGet<CameraComponent>(front) == nullptr);
        CHECK(world.tryGet<Tether>(back) == nullptr);
        CHECK(world.tryGet<CameraComponent>(back) != nullptr);
    }

    TEST_CASE("an instance gains what its prefab gains, loses what it loses, and keeps what was added to it") {
        Yard yard;
        World& world = yard.world;
        Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        const Entity added = world.createEntity("Lantern");
        REQUIRE(world.setParent(added, copy).has_value());

        // The back wheel is gone from the prefab; a seat is new in it, with a cushion on it.
        const Uuid back = world.id(yard.back);
        Json kept = Json::array();
        for (const Json& entity : entities) {
            if (entity["id"].get<std::string>() != back.toString()) {
                kept.push_back(entity);
            }
        }
        entities = std::move(kept);
        const Uuid seat = *Uuid::parse("eeeeeeee-eeee-4eee-8eee-000000000001");
        const Uuid cushion = *Uuid::parse("eeeeeeee-eeee-4eee-8eee-000000000002");
        entities.push_back(Json::parse(
            fmt::format(R"({{"id": "{}", "name": "Seat", "enabled": true, "parent": "{}", "components": {{}}}})",
                        seat.toString(), world.id(yard.cart).toString())));
        entities.push_back(Json::parse(fmt::format(
            R"({{"id": "{}", "name": "Cushion", "enabled": true, "parent": "{}", "components": {{"Tether": {{"to": "{}", "length": 1}}}}}})",
            cushion.toString(), seat.toString(), seat.toString())));

        const PrefabSync sync = unwrap(syncPrefabInstance(world, yard.registry, copy, entities));
        CHECK(sync.entitiesDestroyed == 1);
        CHECK(sync.entitiesCreated == 2);
        std::vector<std::string> below;
        for (const Entity child : world.children(copy)) {
            below.push_back(world.name(child));
        }
        CHECK(below == std::vector<std::string>{"Front wheel", "Lantern", "Seat"});
        const Entity made = world.children(copy).back();
        CHECK(world.tryGet<PrefabLinkComponent>(made)->source == seat);
        REQUIRE(world.children(made).size() == 1);
        CHECK(world.tryGet<PrefabLinkComponent>(world.children(made).front())->source == cushion);
        // Tied to the seat of the prefab, the new cushion is tied to the new seat.
        CHECK(world.tryGet<Tether>(world.children(made).front())->to == world.id(made));
        CHECK(world.tryGet<PrefabLinkComponent>(added) == nullptr);
    }

    TEST_CASE("a link written in a prefab's file says nothing of the links of an instance") {
        Yard yard;
        World& world = yard.world;
        Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        const PrefabLinkComponent before = *world.tryGet<PrefabLinkComponent>(copy);
        world.tryGet<PrefabLinkComponent>(copy)->overrides = {"enabled"};

        // Written by hand, or by an editor that made the prefab of an instance before links were
        // left out: another source, and overrides of its own.
        entities[0]["components"]["PrefabLink"] =
            Json::parse(R"({"source": "dddddddd-dddd-4ddd-8ddd-000000000001", "overrides": ["name"]})");
        const PrefabSync sync = unwrap(syncPrefabInstance(world, yard.registry, copy, entities));
        CHECK(sync.entitiesCreated == 0);
        CHECK(sync.entitiesDestroyed == 0);
        CHECK(sync.componentsAdded == 0);
        CHECK(sync.componentsRemoved == 0);
        CHECK(sync.valuesChanged == 0);
        // Nothing changed - the back wheel of the copy is still tied to the front wheel of the
        // copy, which the prefab's own front wheel stands for.
        Entity copyFront;
        Entity copyBack;
        for (const Entity child : world.children(copy)) {
            (world.name(child) == "Front wheel" ? copyFront : copyBack) = child;
        }
        CHECK(world.tryGet<Tether>(copyBack)->to == world.id(copyFront));
        const PrefabLinkComponent* after = world.tryGet<PrefabLinkComponent>(copy);
        CHECK(after->source == before.source);
        CHECK(after->overrides == std::vector<std::string>{"enabled"});

        // Placed, the copy is linked to the entity of the file, not to what its link said.
        const Entity placed = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        CHECK(world.tryGet<PrefabLinkComponent>(placed)->source == before.source);
        CHECK(world.tryGet<PrefabLinkComponent>(placed)->overrides.empty());
    }

    TEST_CASE("what is no linked instance, or no prefab, is not brought up to date, and nothing changes") {
        Yard yard;
        World& world = yard.world;
        Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        CHECK(errorIn(syncPrefabInstance(world, yard.registry, yard.yard, entities)).code ==
              ErrorCode::InvalidArgument);
        CHECK(errorIn(syncPrefabInstance(world, yard.registry, Entity{}, entities)).code == ErrorCode::InvalidArgument);

        // A copy placed before prefabs were linked stays what it is.
        world.remove<PrefabLinkComponent>(copy);
        entities[0]["name"] = "Wagon";
        world.setName(copy, "Old cart");
        CHECK_FALSE(unwrap(syncPrefabInstance(world, yard.registry, copy, entities)).changed());
        world.add<PrefabLinkComponent>(copy).source = world.id(yard.cart);

        // A prefab that is wrong changes nothing, and says what is wrong.
        const Json before = serializePrefabEntities(world, yard.registry, copy);
        Json wrong = entities;
        wrong[1]["components"]["Wheel"] = Json::object();
        wrong[1]["components"]["Camera"] = Json::parse(R"({"verticalFovDegrees": 80.0})");
        CHECK(errorIn(syncPrefabInstance(world, yard.registry, copy, wrong)).message.find("Wheel") !=
              std::string::npos);
        CHECK(errorIn(syncPrefabInstance(world, yard.registry, copy, Json::array())).code == ErrorCode::ParseError);
        Json twoRoots = entities;
        twoRoots[1]["parent"] = nullptr;
        CHECK(errorIn(syncPrefabInstance(world, yard.registry, copy, twoRoots)).code == ErrorCode::ParseError);
        CHECK(serializePrefabEntities(world, yard.registry, copy) == before);
    }

    TEST_CASE("what an instance keeps of its own is written into its prefab, and is the prefab's then") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab,
                                                     {.name = "My cart", .position = Vec3(9.0f, 0.0f, 0.0f)}));
        Entity front;
        Entity back;
        for (const Entity child : world.children(copy)) {
            (world.name(child) == "Front wheel" ? front : back) = child;
        }
        // The back wheel is tied more loosely and sees; the front one is renamed, switched off
        // and sees no more; a lantern is hung on the cart, tied to the front wheel.
        world.tryGet<Tether>(back)->length = 3.0f;
        world.add<CameraComponent>(back).verticalFovDegrees = 30.0f;
        world.tryGet<PrefabLinkComponent>(back)->overrides = {"Tether.length", "+Camera"};
        world.setName(front, "Leading wheel");
        world.setEnabled(front, false);
        world.remove<CameraComponent>(front);
        world.tryGet<PrefabLinkComponent>(front)->overrides = {"name", "enabled", "-Camera"};
        const Entity lantern = world.createEntity("Lantern");
        REQUIRE(world.setParent(lantern, copy).has_value());
        world.add<Tether>(lantern) = {.to = world.id(front), .length = 0.5f};
        const Entity wick = world.createEntity("Wick");
        REQUIRE(world.setParent(wick, lantern).has_value());

        const Json applied =
            unwrap(prefabEntitiesWithOverrides(world, yard.registry, copy, entities, {.entity = copy}));
        // Nothing of the world has changed.
        CHECK(world.tryGet<PrefabLinkComponent>(back)->overrides.size() == 2);
        CHECK_FALSE(world.has<PrefabLinkComponent>(lantern));

        const auto entityOf = [&applied](const Uuid& id) -> const Json& {
            for (const Json& entity : applied) {
                if (entity["id"].get<std::string>() == id.toString()) {
                    return entity;
                }
            }
            FAIL("no entity " << id.toString());
            return applied.front();
        };
        REQUIRE(applied.size() == 5);
        // The root keeps the prefab's name and place.
        CHECK(applied[0] == entities[0]);
        const Json& prefabFront = entityOf(world.id(yard.front));
        CHECK(prefabFront["name"] == "Leading wheel");
        CHECK(prefabFront["enabled"] == false);
        CHECK_FALSE(prefabFront["components"].contains("Camera"));
        const Json& prefabBack = entityOf(world.id(yard.back));
        CHECK(prefabBack["components"]["Tether"]["length"] == 3.0);
        // Still tied to the prefab's own front wheel, which the copy's stands for.
        CHECK(prefabBack["components"]["Tether"]["to"] == world.id(yard.front).toString());
        CHECK(prefabBack["components"]["Camera"]["verticalFovDegrees"] == 30.0);
        // The lantern and its wick, with their own ids, under the prefab's root; the lantern
        // tied to the prefab's front wheel.
        const Json& prefabLantern = entityOf(world.id(lantern));
        CHECK(prefabLantern["parent"] == world.id(yard.cart).toString());
        CHECK(prefabLantern["components"]["Tether"]["to"] == world.id(yard.front).toString());
        CHECK(entityOf(world.id(wick))["parent"] == world.id(lantern).toString());

        // Marked applied, the instance keeps nothing of its own, and is what its prefab is.
        markPrefabApplied(world, copy, {.entity = copy});
        for (const Entity entity : {copy, front, back}) {
            CHECK(world.tryGet<PrefabLinkComponent>(entity)->overrides.empty());
        }
        CHECK(world.tryGet<PrefabLinkComponent>(lantern)->source == world.id(lantern));
        CHECK(world.tryGet<PrefabLinkComponent>(wick)->source == world.id(wick));
        CHECK_FALSE(unwrap(syncPrefabInstance(world, yard.registry, copy, applied)).changed());
        CHECK(world.transform(copy).position == Vec3(9.0f, 0.0f, 0.0f));

        // Placed again, the prefab is all of it, its lantern tied to its own front wheel.
        const Entity again = unwrap(instantiatePrefab(world, yard.registry, applied, kCartPrefab));
        std::vector<std::string> below;
        Entity againFront;
        Entity againLantern;
        for (const Entity child : world.children(again)) {
            below.push_back(world.name(child));
            if (world.name(child) == "Leading wheel") {
                againFront = child;
            } else if (world.name(child) == "Lantern") {
                againLantern = child;
            }
        }
        CHECK(below == std::vector<std::string>{"Leading wheel", "Back wheel", "Lantern"});
        CHECK(world.tryGet<Tether>(againLantern)->to == world.id(againFront));
        CHECK(world.children(againLantern).size() == 1);
    }

    TEST_CASE("one override of one entity is applied alone") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        Entity back;
        for (const Entity child : world.children(copy)) {
            if (world.name(child) == "Back wheel") {
                back = child;
            }
        }
        world.tryGet<Tether>(back)->length = 3.0f;
        world.setName(back, "Rear wheel");
        world.tryGet<PrefabLinkComponent>(back)->overrides = {"name", "Tether.length"};

        const PrefabApply apply{.entity = back, .entry = "Tether.length"};
        const Json applied = unwrap(prefabEntitiesWithOverrides(world, yard.registry, copy, entities, apply));
        Json expected = entities;
        for (Json& entity : expected) {
            if (entity["id"].get<std::string>() == world.id(yard.back).toString()) {
                entity["components"]["Tether"]["length"] = 3.0;
            }
        }
        CHECK(applied == expected);
        markPrefabApplied(world, copy, apply);
        CHECK(world.tryGet<PrefabLinkComponent>(back)->overrides == std::vector<std::string>{"name"});

        // All of one entity that is not the root: its own overrides, and nothing else.
        const Json all = unwrap(prefabEntitiesWithOverrides(world, yard.registry, copy, applied, {.entity = back}));
        CHECK(all.size() == applied.size());
        for (const Json& entity : all) {
            if (entity["id"].get<std::string>() == world.id(yard.back).toString()) {
                CHECK(entity["name"] == "Rear wheel");
            } else {
                CHECK(std::ranges::find(applied, entity) != applied.end());
            }
        }
    }

    TEST_CASE("what cannot be applied is refused, and nothing is written") {
        Yard yard;
        World& world = yard.world;
        const Json entities = yard.cartEntities();
        const Entity copy = unwrap(instantiatePrefab(world, yard.registry, entities, kCartPrefab));
        const Entity wheel = world.children(copy).front();
        world.tryGet<PrefabLinkComponent>(wheel)->overrides = {"enabled"};
        const Json before = serializeScene(world, yard.registry);

        SUBCASE("an override the entity does not keep") {
            const PrefabApply apply{.entity = wheel, .entry = "name"};
            CHECK(errorIn(prefabEntitiesWithOverrides(world, yard.registry, copy, entities, apply)).code ==
                  ErrorCode::InvalidArgument);
        }
        SUBCASE("an entity of no instance, or of another") {
            CHECK(errorIn(prefabEntitiesWithOverrides(world, yard.registry, copy, entities, {.entity = yard.post}))
                      .code == ErrorCode::InvalidArgument);
            CHECK(errorIn(prefabEntitiesWithOverrides(world, yard.registry, yard.post, entities, {.entity = wheel}))
                      .code == ErrorCode::InvalidArgument);
        }
        SUBCASE("an instance linked to another prefab, or to none") {
            Json other = entities;
            other[0]["id"] = "eeeeeeee-eeee-4eee-8eee-000000000009";
            for (Json& entity : other) {
                if (entity["parent"] == entities[0]["id"]) {
                    entity["parent"] = other[0]["id"];
                }
            }
            CHECK(errorIn(prefabEntitiesWithOverrides(world, yard.registry, copy, other, {.entity = copy})).code ==
                  ErrorCode::InvalidArgument);
        }
        SUBCASE("an instance of another prefab inside") {
            const Entity inner = world.createEntity("Inner");
            REQUIRE(world.setParent(inner, copy).has_value());
            world.add<PrefabInstanceComponent>(inner).prefab = kOtherPrefab;
            const Error refused =
                errorIn(prefabEntitiesWithOverrides(world, yard.registry, copy, entities, {.entity = copy}));
            CHECK(refused.code == ErrorCode::InvalidArgument);
            CHECK(refused.message.find("'Inner'") != std::string::npos);
            world.destroyEntity(inner);
        }
        SUBCASE("entities that are no prefab") {
            CHECK(errorIn(prefabEntitiesWithOverrides(world, yard.registry, copy, Json::array(), {.entity = copy}))
                      .code == ErrorCode::ParseError);
        }
        CHECK(serializeScene(world, yard.registry) == before);
    }
}
