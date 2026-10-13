#include "cordite/world/Prefab.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

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
}
