#include "cordite/world/EntityPath.h"

#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

/// Room / Table / Lamp, a Chair beside the table, and a root of its own: Eye.
struct Furnished {
    Furnished() {
        room = world.createEntity("Room");
        table = world.createEntity("Table");
        lamp = world.createEntity("Lamp");
        chair = world.createEntity("Chair");
        eye = world.createEntity("Eye");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        REQUIRE(world.setParent(chair, room).has_value());
    }

    World world;
    Entity room;
    Entity table;
    Entity lamp;
    Entity chair;
    Entity eye;
};

} // namespace

TEST_SUITE("EntityPath") {
    TEST_CASE("the path of an entity is the names leading to it from a root") {
        const Furnished furnished;
        CHECK(entityPath(furnished.world, furnished.lamp) == "Room/Table/Lamp");
        CHECK(entityPath(furnished.world, furnished.room) == "Room");
        CHECK(entityPath(furnished.world, furnished.eye) == "Eye");
    }

    TEST_CASE("a path leads from the roots, or from an entity, and back to the same entity") {
        const Furnished furnished;
        const World& world = furnished.world;
        CHECK(unwrap(findEntityByPath(world, "Room/Table/Lamp")) == furnished.lamp);
        CHECK(unwrap(findEntityByPath(world, entityPath(world, furnished.chair))) == furnished.chair);
        // From an entity: its children are where the path begins.
        CHECK(unwrap(findEntityByPath(world, "Table/Lamp", furnished.room)) == furnished.lamp);
        CHECK(unwrap(findEntityByPath(world, "Lamp", furnished.table)) == furnished.lamp);
        // A slash in front sets out from the roots, whatever the entity.
        CHECK(unwrap(findEntityByPath(world, "/Eye", furnished.table)) == furnished.eye);
        CHECK(unwrap(findEntityByPath(world, "/Room/Chair", furnished.lamp)) == furnished.chair);
        // Empty parts add nothing; a path of no names is where it sets out from.
        CHECK(unwrap(findEntityByPath(world, "Room//Table/", Entity{})) == furnished.table);
        CHECK(unwrap(findEntityByPath(world, "", furnished.table)) == furnished.table);
    }

    TEST_CASE("a path that leads nowhere says where it stopped") {
        const Furnished furnished;
        const World& world = furnished.world;

        struct Case {
            std::string path;
            Entity from;
            ErrorCode code;
            std::string fragment;
        };

        const std::vector<Case> cases = {
            {"Room/Sofa", {}, ErrorCode::NotFound, "'Sofa' in the path 'Room/Sofa' does not name a child of 'Room'"},
            {"Kitchen", {}, ErrorCode::NotFound, "'Kitchen' in the path 'Kitchen' does not name a root entity"},
            {"Lamp", furnished.room, ErrorCode::NotFound, "does not name a child of 'Room'"},
            // Names are compared as they are.
            {"room", {}, ErrorCode::NotFound, "does not name a root entity"},
            {"", {}, ErrorCode::InvalidArgument, "does not name an entity"},
            {"/", furnished.room, ErrorCode::InvalidArgument, "'/' does not name an entity"},
        };
        for (const Case& entry : cases) {
            CAPTURE(entry.path);
            const Result<Entity> found = findEntityByPath(world, entry.path, entry.from);
            REQUIRE_FALSE(found.has_value());
            CHECK(found.error().code == entry.code);
            CHECK(found.error().message.find(entry.fragment) != std::string::npos);
        }
    }

    TEST_CASE("a part that names several entities is refused, and they are listed") {
        Furnished furnished;
        World& world = furnished.world;
        const Entity secondLamp = world.createEntity("Lamp");
        REQUIRE(world.setParent(secondLamp, furnished.table).has_value());
        const Result<Entity> found = findEntityByPath(world, "Room/Table/Lamp");
        REQUIRE_FALSE(found.has_value());
        CHECK(found.error().code == ErrorCode::InvalidArgument);
        CHECK(found.error().message.find("matches 2 entities") != std::string::npos);
        CHECK(found.error().message.find(world.id(secondLamp).toString()) != std::string::npos);
        CHECK(found.error().message.find(world.id(furnished.lamp).toString()) != std::string::npos);
    }

    TEST_CASE("a long list of entities is shortened") {
        World world;
        std::vector<Entity> crates;
        for (int index = 0; index < 6; ++index) {
            crates.push_back(world.createEntity("Crate"));
        }
        const std::string listed = describeEntities(world, crates);
        CHECK(listed.find(world.id(crates[3]).toString()) != std::string::npos);
        CHECK(listed.find(world.id(crates[4]).toString()) == std::string::npos);
        CHECK(listed.ends_with(" and 2 more"));
        CHECK(describeEntities(world, {}).empty());
    }

    TEST_CASE("a path cannot set out from an entity that is gone") {
        Furnished furnished;
        furnished.world.destroyEntity(furnished.table);
        const Result<Entity> found = findEntityByPath(furnished.world, "Lamp", furnished.table);
        REQUIRE_FALSE(found.has_value());
        CHECK(found.error().code == ErrorCode::InvalidArgument);
        CHECK(found.error().message.find("is gone") != std::string::npos);
    }
}
