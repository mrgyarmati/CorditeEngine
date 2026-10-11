#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::AssertionFailure;
using cordite::test::unwrap;

namespace {

/// The names of all entities in traversal order, indented by depth with dots.
std::vector<std::string> outline(const World& world) {
    std::vector<std::string> lines;
    world.traverse(
        [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
    return lines;
}

Vec3 worldPosition(const World& world, Entity entity) {
    return Vec3(world.computeWorldMatrix(entity)[3]);
}

/// A component other than the built-in ones, for observing construction and destruction.
struct Marker {
    int value = 0;
};

/// Counts destroyed Markers.
struct DestructionCounter {
    int count = 0;

    void onMarkerDestroyed(entt::registry& /*registry*/, entt::entity /*handle*/) { ++count; }
};

/// Records what a destruction listener can still see of the entity that is losing its
/// Marker.
struct DestructionLog {
    const World* world = nullptr;
    std::vector<std::string> entries;

    void onMarkerDestroyed(entt::registry& registry, entt::entity handle) {
        const Entity entity{handle};
        const Entity parent = world->parent(entity);
        entries.push_back(world->name(entity) + " under " + (parent.isNull() ? "nothing" : world->name(parent)) +
                          " with " + std::to_string(registry.get<Hierarchy>(handle).children.size()) + " children");
    }
};

} // namespace

TEST_SUITE("World") {
    TEST_CASE("a new entity is a valid, enabled root with an identity transform") {
        World world;
        CHECK(world.entityCount() == 0);

        const Entity entity = world.createEntity("Crate");
        CHECK(world.isValid(entity));
        CHECK_FALSE(entity.isNull());
        CHECK(world.entityCount() == 1);
        CHECK(world.name(entity) == "Crate");
        CHECK_FALSE(world.id(entity).isNil());
        CHECK(world.isEnabled(entity));
        CHECK(world.parent(entity).isNull());
        CHECK(world.transform(entity) == Transform());
        CHECK(world.roots() == std::vector<Entity>{entity});
        CHECK(world.name(world.createEntity()) == "Entity");
    }

    TEST_CASE("the null entity and stale handles are not valid") {
        World world;
        CHECK_FALSE(world.isValid(Entity{}));
        CHECK(Entity{}.isNull());

        const Entity entity = world.createEntity();
        world.destroyEntity(entity);
        CHECK_FALSE(world.isValid(entity));
        CHECK(world.entityCount() == 0);

        // A new entity may reuse the slot, but the old handle stays invalid.
        const Entity next = world.createEntity();
        CHECK(world.isValid(next));
        CHECK_FALSE(world.isValid(entity));
        CHECK_FALSE(next == entity);

        // Destroying again, or destroying nothing, is harmless.
        CHECK_NOTHROW(world.destroyEntity(entity));
        CHECK_NOTHROW(world.destroyEntity(Entity{}));
        CHECK(world.entityCount() == 1);
    }

    TEST_CASE("entities can be used as hash map keys") {
        World world;
        const Entity first = world.createEntity();
        const Entity second = world.createEntity();

        std::unordered_set<Entity> set = {first, second, first};
        CHECK(set.size() == 2);
        CHECK(set.contains(second));
        CHECK_FALSE(set.contains(Entity{}));
    }

    TEST_CASE("entities are found by id and by name") {
        World world;
        const Entity first = world.createEntity("Lamp");
        const Entity second = world.createEntity("Lamp");
        const Entity third = world.createEntity("Door");

        CHECK(world.findById(world.id(second)) == second);
        CHECK(world.findById(Uuid::generate()).isNull());
        CHECK(world.findById(Uuid()).isNull());

        // With duplicate names, the first in hierarchy order wins.
        CHECK(world.findByName("Lamp") == first);
        CHECK(world.findByName("Door") == third);
        CHECK(world.findByName("lamp").isNull());

        world.destroyEntity(first);
        CHECK(world.findByName("Lamp") == second);
    }

    TEST_CASE("createEntityWithId keeps the given id and rejects bad ones") {
        World world;
        const Uuid id = Uuid::generate();

        const Entity entity = unwrap(world.createEntityWithId(id, "Loaded"));
        CHECK(world.id(entity) == id);

        const Result<Entity> duplicate = world.createEntityWithId(id, "Again");
        REQUIRE_FALSE(duplicate.has_value());
        CHECK(duplicate.error().code == ErrorCode::AlreadyExists);

        const Result<Entity> nil = world.createEntityWithId(Uuid(), "Nil");
        REQUIRE_FALSE(nil.has_value());
        CHECK(nil.error().code == ErrorCode::InvalidArgument);
        CHECK(world.entityCount() == 1);

        // An id becomes available again once its entity is gone.
        world.destroyEntity(entity);
        CHECK(world.createEntityWithId(id, "Reused").has_value());
    }

    TEST_CASE("names and the enabled flag can be changed") {
        World world;
        const Entity entity = world.createEntity("Old");
        world.setName(entity, "New");
        CHECK(world.name(entity) == "New");

        world.setEnabled(entity, false);
        CHECK_FALSE(world.isEnabled(entity));
    }

    TEST_CASE("setParent builds a hierarchy that traverse walks parents first") {
        World world;
        const Entity room = world.createEntity("Room");
        const Entity table = world.createEntity("Table");
        const Entity lamp = world.createEntity("Lamp");
        const Entity outside = world.createEntity("Outside");

        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());

        CHECK(world.parent(lamp) == table);
        CHECK(world.parent(table) == room);
        CHECK(world.children(room) == std::vector<Entity>{table});
        CHECK(world.roots() == std::vector<Entity>{room, outside});
        CHECK(outline(world) == std::vector<std::string>{"Room", ".Table", "..Lamp", "Outside"});

        CHECK(world.isDescendantOf(lamp, room));
        CHECK(world.isDescendantOf(lamp, table));
        CHECK_FALSE(world.isDescendantOf(room, lamp));
        CHECK_FALSE(world.isDescendantOf(lamp, lamp));
        CHECK_FALSE(world.isDescendantOf(outside, room));
        CHECK_FALSE(world.isDescendantOf(lamp, Entity{}));
    }

    TEST_CASE("traverseSubtree visits one entity and everything below it") {
        World world;
        const Entity room = world.createEntity("Room");
        const Entity table = world.createEntity("Table");
        const Entity lamp = world.createEntity("Lamp");
        const Entity chair = world.createEntity("Chair");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        REQUIRE(world.setParent(chair, room).has_value());

        std::vector<std::string> visited;
        world.traverseSubtree(table, [&](Entity entity, uint32_t depth) {
            visited.push_back(std::to_string(depth) + world.name(entity));
        });
        CHECK(visited == std::vector<std::string>{"0Table", "1Lamp"});

        visited.clear();
        world.traverseSubtree(Entity{}, [&](Entity /*entity*/, uint32_t /*depth*/) { visited.emplace_back("?"); });
        CHECK(visited.empty());
    }

    TEST_CASE("an entity cannot become its own ancestor") {
        World world;
        const Entity a = world.createEntity("A");
        const Entity b = world.createEntity("B");
        const Entity c = world.createEntity("C");
        REQUIRE(world.setParent(b, a).has_value());
        REQUIRE(world.setParent(c, b).has_value());

        const Status self = world.setParent(a, a);
        REQUIRE_FALSE(self.has_value());
        CHECK(self.error().code == ErrorCode::InvalidArgument);

        const Status cycle = world.setParent(a, c);
        REQUIRE_FALSE(cycle.has_value());
        CHECK(cycle.error().code == ErrorCode::InvalidArgument);

        // The failed attempts changed nothing.
        CHECK(outline(world) == std::vector<std::string>{"A", ".B", "..C"});
    }

    TEST_CASE("setParent rejects invalid handles") {
        World world;
        const Entity alive = world.createEntity("Alive");
        const Entity dead = world.createEntity("Dead");
        world.destroyEntity(dead);

        CHECK_FALSE(world.setParent(dead, alive).has_value());
        CHECK_FALSE(world.setParent(alive, dead).has_value());
        CHECK_FALSE(world.setParent(Entity{}, alive).has_value());
        CHECK(world.parent(alive).isNull());
    }

    TEST_CASE("reparenting to null makes an entity a root again") {
        World world;
        const Entity parent = world.createEntity("Parent");
        const Entity child = world.createEntity("Child");
        REQUIRE(world.setParent(child, parent).has_value());
        REQUIRE(world.setParent(child, Entity{}).has_value());

        CHECK(world.parent(child).isNull());
        CHECK(world.children(parent).empty());
        CHECK(world.roots() == std::vector<Entity>{parent, child});
    }

    TEST_CASE("setting the parent an entity already has changes nothing") {
        World world;
        const Entity parent = world.createEntity("Parent");
        const Entity first = world.createEntity("First");
        const Entity second = world.createEntity("Second");
        REQUIRE(world.setParent(first, parent).has_value());
        REQUIRE(world.setParent(second, parent).has_value());

        // Neither the order among the siblings nor the transform is touched.
        world.transform(first).position = Vec3(1.0f, 2.0f, 3.0f);
        REQUIRE(world.setParent(first, parent).has_value());
        CHECK(world.children(parent) == std::vector<Entity>{first, second});
        CHECK(world.transform(first).position == Vec3(1.0f, 2.0f, 3.0f));

        REQUIRE(world.setParent(parent, Entity{}).has_value());
        CHECK(world.roots() == std::vector<Entity>{parent});
    }

    TEST_CASE("reparenting keeps the world placement by default") {
        World world;
        const Entity parent = world.createEntity("Parent");
        world.transform(parent).position = Vec3(10.0f, 0.0f, 0.0f);
        world.transform(parent).rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
        world.transform(parent).scale = Vec3(2.0f);

        const Entity child = world.createEntity("Child");
        world.transform(child).position = Vec3(1.0f, 2.0f, 3.0f);
        const Vec3 before = worldPosition(world, child);

        REQUIRE(world.setParent(child, parent).has_value());
        CHECK_NEAR(worldPosition(world, child), before, 1e-4f);
        // The local transform had to change to compensate for the parent.
        CHECK(world.transform(child).position != Vec3(1.0f, 2.0f, 3.0f));

        REQUIRE(world.setParent(child, Entity{}).has_value());
        CHECK_NEAR(worldPosition(world, child), before, 1e-4f);
        CHECK_NEAR(world.transform(child).position, Vec3(1.0f, 2.0f, 3.0f), 1e-4f);
    }

    TEST_CASE("reparenting can keep the local transform instead") {
        World world;
        const Entity parent = world.createEntity("Parent");
        world.transform(parent).position = Vec3(10.0f, 0.0f, 0.0f);
        const Entity child = world.createEntity("Child");
        world.transform(child).position = Vec3(1.0f, 0.0f, 0.0f);

        REQUIRE(world.setParent(child, parent, false).has_value());
        CHECK(world.transform(child).position == Vec3(1.0f, 0.0f, 0.0f));
        CHECK_NEAR(worldPosition(world, child), Vec3(11.0f, 0.0f, 0.0f), 1e-5f);
    }

    TEST_CASE("siblings can be reordered") {
        World world;
        const Entity parent = world.createEntity("Parent");
        const Entity a = world.createEntity("A");
        const Entity b = world.createEntity("B");
        const Entity c = world.createEntity("C");
        for (const Entity child : {a, b, c}) {
            REQUIRE(world.setParent(child, parent).has_value());
        }
        CHECK(world.siblingIndex(a) == 0);
        CHECK(world.siblingIndex(c) == 2);

        world.setSiblingIndex(c, 0);
        CHECK(world.children(parent) == std::vector<Entity>{c, a, b});
        world.setSiblingIndex(c, 1);
        CHECK(world.children(parent) == std::vector<Entity>{a, c, b});
        // An index past the end moves it last.
        world.setSiblingIndex(a, 99);
        CHECK(world.children(parent) == std::vector<Entity>{c, b, a});
        CHECK(world.siblingIndex(a) == 2);

        // Roots are siblings of each other too.
        const Entity other = world.createEntity("Other");
        CHECK(world.siblingIndex(other) == 1);
        world.setSiblingIndex(other, 0);
        CHECK(world.roots() == std::vector<Entity>{other, parent});
        CHECK_NOTHROW(world.setSiblingIndex(Entity{}, 0));
    }

    TEST_CASE("destroying an entity destroys its whole subtree and nothing else") {
        World world;
        const Entity room = world.createEntity("Room");
        const Entity table = world.createEntity("Table");
        const Entity lamp = world.createEntity("Lamp");
        const Entity bulb = world.createEntity("Bulb");
        const Entity chair = world.createEntity("Chair");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        REQUIRE(world.setParent(bulb, lamp).has_value());
        REQUIRE(world.setParent(chair, room).has_value());
        world.setSiblingIndex(table, 1);
        const Uuid lampId = world.id(lamp);

        world.destroyEntity(table);

        CHECK_FALSE(world.isValid(table));
        CHECK_FALSE(world.isValid(lamp));
        CHECK_FALSE(world.isValid(bulb));
        CHECK(world.isValid(room));
        CHECK(world.isValid(chair));
        CHECK(world.findById(lampId).isNull());
        CHECK(world.entityCount() == 2);
        CHECK(outline(world) == std::vector<std::string>{"Room", ".Chair"});

        world.destroyEntity(room);
        CHECK(world.entityCount() == 0);
        CHECK(world.roots().empty());
    }

    TEST_CASE("destroying a middle sibling keeps the others in order") {
        World world;
        const Entity parent = world.createEntity("Parent");
        std::vector<Entity> children;
        for (const char* name : {"A", "B", "C", "D"}) {
            children.push_back(world.createEntity(name));
            REQUIRE(world.setParent(children.back(), parent).has_value());
        }

        world.destroyEntity(children[1]);
        CHECK(outline(world) == std::vector<std::string>{"Parent", ".A", ".C", ".D"});
        world.destroyEntity(children[0]);
        CHECK(outline(world) == std::vector<std::string>{"Parent", ".C", ".D"});
    }

    TEST_CASE("component listeners see a consistent entity while it is destroyed") {
        // Declared first so that it outlives the world, which notifies when it goes away.
        DestructionLog log;
        World world;
        log.world = &world;
        world.registry().on_destroy<Marker>().connect<&DestructionLog::onMarkerDestroyed>(log);

        const Entity room = world.createEntity("Room");
        const Entity table = world.createEntity("Table");
        const Entity lamp = world.createEntity("Lamp");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        for (const Entity entity : {room, table, lamp}) {
            world.add<Marker>(entity);
        }

        // Children go first, each still attached to its parent and already childless.
        world.destroyEntity(table);
        CHECK(log.entries ==
              std::vector<std::string>{"Lamp under Table with 0 children", "Table under Room with 0 children"});

        // Clearing notifies for everything that is left.
        log.entries.clear();
        world.clear();
        CHECK(log.entries == std::vector<std::string>{"Room under nothing with 0 children"});
    }

    TEST_CASE("destroying a world destroys its components first") {
        DestructionCounter counter;
        {
            World world;
            world.registry().on_destroy<Marker>().connect<&DestructionCounter::onMarkerDestroyed>(counter);

            world.add<Marker>(world.createEntity());
            world.add<Marker>(world.createEntity());
            CHECK(counter.count == 0);
        }
        CHECK(counter.count == 2);
    }

    TEST_CASE("clear removes everything") {
        World world;
        const Entity parent = world.createEntity("Parent");
        const Entity child = world.createEntity("Child");
        REQUIRE(world.setParent(child, parent).has_value());

        world.clear();
        CHECK(world.entityCount() == 0);
        CHECK(world.roots().empty());
        CHECK_FALSE(world.isValid(parent));
        CHECK_FALSE(world.isValid(child));
        CHECK(world.isValid(world.createEntity("Fresh")));
        CHECK(outline(world) == std::vector<std::string>{"Fresh"});
    }

    TEST_CASE("a disabled ancestor disables the whole subtree") {
        World world;
        const Entity parent = world.createEntity("Parent");
        const Entity child = world.createEntity("Child");
        const Entity grandchild = world.createEntity("Grandchild");
        REQUIRE(world.setParent(child, parent).has_value());
        REQUIRE(world.setParent(grandchild, child).has_value());
        CHECK(world.isEnabledInHierarchy(grandchild));

        world.setEnabled(parent, false);
        CHECK_FALSE(world.isEnabledInHierarchy(parent));
        CHECK_FALSE(world.isEnabledInHierarchy(grandchild));
        // Its own flag is untouched, so re-enabling the parent restores it.
        CHECK(world.isEnabled(grandchild));

        world.setEnabled(parent, true);
        CHECK(world.isEnabledInHierarchy(grandchild));
        CHECK_FALSE(world.isEnabledInHierarchy(Entity{}));
    }

    TEST_CASE("world transforms combine the transforms up the hierarchy") {
        World world;
        const Entity parent = world.createEntity("Parent");
        world.transform(parent).position = Vec3(0.0f, 0.0f, -10.0f);
        world.transform(parent).rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
        world.transform(parent).scale = Vec3(2.0f);

        const Entity child = world.createEntity("Child");
        REQUIRE(world.setParent(child, parent, false).has_value());
        world.transform(child).position = Vec3(1.0f, 0.0f, 0.0f);

        world.updateWorldTransforms();

        // The child's offset is scaled by 2 and turned 90 degrees about +Y (+X becomes -Z).
        const Vec3 expected(0.0f, 0.0f, -12.0f);
        CHECK_NEAR(Vec3(world.worldMatrix(child)[3]), expected, 1e-5f);
        CHECK(test::nearlyEqual(world.worldMatrix(child), world.computeWorldMatrix(child), 1e-5f));
        CHECK(test::nearlyEqual(world.worldMatrix(parent), world.transform(parent).toMatrix(), 1e-6f));
    }

    TEST_CASE("worldMatrix reflects the last update, computeWorldMatrix the present") {
        World world;
        const Entity entity = world.createEntity();
        world.updateWorldTransforms();

        world.transform(entity).position = Vec3(5.0f, 0.0f, 0.0f);
        CHECK_NEAR(Vec3(world.worldMatrix(entity)[3]), Vec3(0.0f), 0.0f);
        CHECK_NEAR(Vec3(world.computeWorldMatrix(entity)[3]), Vec3(5.0f, 0.0f, 0.0f), 0.0f);

        world.updateWorldTransforms();
        CHECK_NEAR(Vec3(world.worldMatrix(entity)[3]), Vec3(5.0f, 0.0f, 0.0f), 0.0f);
    }

    TEST_CASE("components can be added, queried and removed") {
        World world;
        const Entity entity = world.createEntity();
        CHECK_FALSE(world.has<CameraComponent>(entity));
        CHECK(world.tryGet<CameraComponent>(entity) == nullptr);

        world.add<CameraComponent>(entity).verticalFovDegrees = 45.0f;
        CHECK(world.has<CameraComponent>(entity));
        // Adding again returns the existing component.
        CHECK(world.add<CameraComponent>(entity).verticalFovDegrees == 45.0f);
        CHECK(world.tryGet<CameraComponent>(entity)->verticalFovDegrees == 45.0f);
        CHECK(std::as_const(world).tryGet<CameraComponent>(entity)->verticalFovDegrees == 45.0f);

        world.remove<CameraComponent>(entity);
        CHECK_FALSE(world.has<CameraComponent>(entity));
        CHECK_NOTHROW(world.remove<CameraComponent>(entity));

        // Constructor arguments are forwarded.
        CHECK(world.add<Marker>(entity, 42).value == 42);

        // Queries on invalid entities are safe.
        CHECK_FALSE(world.has<CameraComponent>(Entity{}));
        CHECK(world.tryGet<CameraComponent>(Entity{}) == nullptr);
        CHECK_NOTHROW(world.remove<CameraComponent>(Entity{}));
    }

    TEST_CASE("systems can iterate components through the registry") {
        World world;
        for (int index = 0; index < 5; ++index) {
            const Entity entity = world.createEntity("Entity");
            if (index % 2 == 0) {
                world.add<CameraComponent>(entity).verticalFovDegrees = static_cast<float>(index);
            }
        }

        int cameras = 0;
        for (const auto [handle, camera, info] : world.registry().view<CameraComponent, EntityInfo>().each()) {
            ++cameras;
            CHECK(world.isValid(Entity{handle}));
            CHECK(info.name == "Entity");
            CHECK(camera.verticalFovDegrees >= 0.0f);
        }
        CHECK(cameras == 3);
    }

    TEST_CASE("a hierarchy deeper than any call stack is handled") {
        World world;
        // Deep enough to overflow the stack of a recursive traversal, shallow enough for
        // the ancestor check in setParent, which walks the whole chain each time.
        Entity previous;
        constexpr uint32_t kDepth = 8000;
        for (uint32_t index = 0; index < kDepth; ++index) {
            const Entity entity = world.createEntity("Link");
            world.transform(entity).position = Vec3(1.0f, 0.0f, 0.0f);
            if (!previous.isNull()) {
                REQUIRE(world.setParent(entity, previous, false).has_value());
            }
            previous = entity;
        }

        world.updateWorldTransforms();
        CHECK(world.worldMatrix(previous)[3].x == doctest::Approx(static_cast<float>(kDepth)));
        CHECK(world.roots().size() == 1);

        uint32_t deepest = 0;
        world.traverse([&](Entity /*entity*/, uint32_t depth) { deepest = std::max(deepest, depth); });
        CHECK(deepest == kDepth - 1);

        world.destroyEntity(world.roots().front());
        CHECK(world.entityCount() == 0);
    }

    TEST_CASE("the scene name is a label that can be changed") {
        World world;
        CHECK(world.sceneName() == "Untitled");
        world.setSceneName("Depot");
        CHECK(world.sceneName() == "Depot");
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("using an invalid handle where a valid one is required is a programmer error") {
        World world;
        CHECK_THROWS_AS((void)world.name(Entity{}), AssertionFailure);
        CHECK_THROWS_AS((void)world.id(Entity{}), AssertionFailure);
        CHECK_THROWS_AS((void)world.transform(Entity{}), AssertionFailure);
        CHECK_THROWS_AS((void)world.worldMatrix(Entity{}), AssertionFailure);
        CHECK_THROWS_AS(world.setName(Entity{}, "x"), AssertionFailure);
        CHECK_THROWS_AS((void)world.add<Marker>(Entity{}), AssertionFailure);
    }
#endif
}
