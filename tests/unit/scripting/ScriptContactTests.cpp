#include "cordite/animation/SequenceSystem.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// A script that says everything it is told about what its body touches: who is told,
/// what, who the other is, and whatever comes with it.
constexpr const char* kListener = R"(
    local Listener = {}
    local function tell(self, what, other, contact, ...)
        local line = self.entity:name() .. " " .. what .. " " .. tostring(other)
        if contact ~= nil then
            line = line .. " at " .. tostring(contact.position) .. " towards " .. tostring(contact.normal) ..
                   " with " .. tostring(contact.speed)
        end
        print(line, select("#", ...))
    end
    function Listener:collisionBegan(...) tell(self, "met", ...) end
    function Listener:collisionEnded(...) tell(self, "parted from", ...) end
    function Listener:triggerEntered(...) tell(self, "is in a trigger with", ...) end
    function Listener:triggerLeft(...) tell(self, "is out of a trigger with", ...) end
    return Listener
)";

/// The same, said shortly: for the tests that let the simulation speak, whose numbers
/// have been through a solver.
constexpr const char* kWitness = R"(
    local Witness = {}
    local function whole(vector)
        return string.format("%d %d %d", math.floor(vector.x + 0.5), math.floor(vector.y + 0.5),
                             math.floor(vector.z + 0.5))
    end
    function Witness:collisionBegan(other, contact)
        print(self.entity:name() .. " met " .. other:name() .. " towards " .. whole(contact.normal), contact.speed > 2)
    end
    function Witness:collisionEnded(other) print(self.entity:name() .. " parted from " .. tostring(other)) end
    function Witness:triggerEntered(other) print(self.entity:name() .. " is in a trigger with " .. other:name()) end
    function Witness:triggerLeft(other) print(self.entity:name() .. " is out of a trigger with " .. tostring(other)) end
    return Witness
)";

CollisionEvent event(CollisionEventType type, Entity first, Entity second) {
    return {.type = type,
            .first = first,
            .second = second,
            .position = Vec3(1.0f, 0.0f, 2.0f),
            .normal = Vec3(0.0f, -1.0f, 0.0f),
            .speed = 3.5f};
}

} // namespace

TEST_SUITE("ScriptContacts") {
    TEST_CASE("the scripts of both entities are told that their bodies met: each of the other, and of the way to it") {
        ScriptStage stage;
        const Uuid listener = stage.write("scripts/listener.lua", kListener);
        const Entity crate = stage.add("Crate", listener);
        const Entity floor = stage.add("Floor", listener);
        stage.update();

        // The crate is the first of the two, and the floor lies below it; to the floor,
        // which is told second, the crate lies above.
        const std::array met = {event(CollisionEventType::CollisionBegan, crate, floor)};
        const ScriptStatistics statistics = stage.scripts->tellContacts(stage.context(), met);
        CHECK(stage.said() == Lines{"Crate met Entity(Floor) at (1.0, 0.0, 2.0) towards (0.0, -1.0, 0.0) with 3.5\t0",
                                    "Floor met Entity(Crate) at (1.0, 0.0, 2.0) towards (0.0, 1.0, 0.0) with 3.5\t0"});
        // Counted on from the update before: that called nothing, this two.
        CHECK(statistics.calls == 2);
        CHECK(statistics.instances == 2);

        // What parts has no place where it touches: the other entity is all that is said.
        const std::array parted = {event(CollisionEventType::CollisionEnded, crate, floor)};
        CHECK(stage.scripts->tellContacts(stage.context(), parted).calls == 4);
        CHECK(stage.said() == Lines{"Crate parted from Entity(Floor)\t0", "Floor parted from Entity(Crate)\t0"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("of a trigger both are told too: the entity that has it, and what moved into it") {
        ScriptStage stage;
        const Uuid listener = stage.write("scripts/listener.lua", kListener);
        const Entity zone = stage.add("Zone", listener);
        const Entity ball = stage.add("Ball", listener);
        stage.update();

        const std::array events = {event(CollisionEventType::TriggerEntered, zone, ball),
                                   event(CollisionEventType::TriggerLeft, zone, ball)};
        stage.scripts->tellContacts(stage.context(), events);
        CHECK(stage.said() ==
              Lines{"Zone is in a trigger with Entity(Ball) at (1.0, 0.0, 2.0) towards (0.0, -1.0, 0.0) with 3.5\t0",
                    "Ball is in a trigger with Entity(Zone) at (1.0, 0.0, 2.0) towards (0.0, 1.0, 0.0) with 3.5\t0",
                    "Zone is out of a trigger with Entity(Ball)\t0", "Ball is out of a trigger with Entity(Zone)\t0"});
    }

    TEST_CASE("only a script that runs, and has a function for it, is told") {
        ScriptStage stage;
        const Uuid listener = stage.write("scripts/listener.lua", kListener);
        const Entity crate = stage.add("Crate", listener);
        // No script at all; a script without such functions; one that is switched off;
        // one that an error has stopped; one that has its instance and has not started.
        const Entity wall = stage.world.createEntity("Wall");
        const Entity quiet = stage.add("Quiet", stage.write("scripts/quiet.lua", "return {update = function() end}"));
        const Entity off = stage.add("Off", listener);
        const Entity broken =
            stage.add("Broken", stage.write("scripts/broken.lua", "return {start = function() error('broken') end, "
                                                                  "collisionBegan = function() print('never') end}"));
        // The last is put to sleep by the start of another before its own turn to start
        // has come, and woken by an update after: there, switched on, and not begun.
        stage.add("Sleeper", stage.write("scripts/sleeper.lua", "return {start = function() "
                                                                "cordite.world.find('Late'):setEnabled(false) end}"));
        stage.add("Waker", stage.write("scripts/waker.lua", "return {update = function() "
                                                            "cordite.world.find('Late'):setEnabled(true) end}"));
        const Entity late = stage.add("Late", listener);
        stage.update();
        stage.world.setEnabled(off, false);
        CHECK(stage.world.isEnabled(late));
        static_cast<void>(stage.errors());
        REQUIRE(stage.problems().size() == 1);

        std::vector<CollisionEvent> events;
        for (const Entity other : {wall, quiet, off, broken, late}) {
            events.push_back(event(CollisionEventType::CollisionBegan, crate, other));
            events.push_back(event(CollisionEventType::CollisionBegan, other, crate));
        }
        const ScriptStatistics statistics = stage.scripts->tellContacts(stage.context(), events);
        // The crate hears of all ten; nobody else of anything.
        const Lines said = stage.said();
        REQUIRE(said.size() == 10);
        for (const std::string& line : said) {
            CAPTURE(line);
            CHECK(line.starts_with("Crate met Entity("));
        }
        // An update of four calls - the start that broke and the one that put to sleep,
        // the updates of the quiet one and of the one that woke - and ten here.
        CHECK(statistics.calls == 14);
        CHECK(stage.problems().size() == 1);
    }

    TEST_CASE("an entity that is gone is still said, and says that it is gone") {
        ScriptStage stage;
        const Entity floor = stage.add("Floor", stage.write("scripts/listener.lua", kListener));
        const Entity crate = stage.world.createEntity("Crate");
        stage.update();
        stage.world.destroyEntity(crate);

        const std::array events = {event(CollisionEventType::CollisionEnded, crate, floor),
                                   event(CollisionEventType::TriggerLeft, floor, crate)};
        stage.scripts->tellContacts(stage.context(), events);
        CHECK(stage.said() ==
              Lines{"Floor parted from Entity(gone)\t0", "Floor is out of a trigger with Entity(gone)\t0"});
        // It is the value the script had for the entity while it was there: what was kept
        // under it is found.
        stage.add("Keeper", stage.write("scripts/keeper.lua", R"(
            local Keeper = {}
            function Keeper:start() self.touching = {} end
            function Keeper:collisionBegan(other) self.touching[other] = other:name() end
            function Keeper:collisionEnded(other)
                print(other:isValid(), self.touching[other])
                self.touching[other] = nil
            end
            return Keeper
        )"));
        const Entity keeper = stage.world.findByName("Keeper");
        const Entity visitor = stage.world.createEntity("Visitor");
        stage.update();
        const std::array met = {event(CollisionEventType::CollisionBegan, keeper, visitor)};
        stage.scripts->tellContacts(stage.context(), met);
        stage.world.destroyEntity(visitor);
        const std::array parted = {event(CollisionEventType::CollisionEnded, keeper, visitor)};
        stage.scripts->tellContacts(stage.context(), parted);
        CHECK(stage.said() == Lines{"false\tVisitor"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a script that is told of a contact may destroy its entity, and stops alone when it is wrong") {
        ScriptStage stage;
        const Entity fragile = stage.add("Fragile", stage.write("scripts/fragile.lua", R"(
            local Fragile = {}
            function Fragile:collisionBegan(other)
                print("fragile met " .. other:name())
                self.entity:destroy()
            end
            function Fragile:destroy() print("fragile is gone") end
            return Fragile
        )"));
        const Entity wrong = stage.add("Wrong", stage.write("scripts/wrong.lua", R"(
            local Wrong = {}
            function Wrong:collisionBegan(other) error("it hurt") end
            function Wrong:collisionEnded(other) print("never") end
            return Wrong
        )"));
        const Entity floor = stage.add("Floor", stage.write("scripts/listener.lua", kListener));
        stage.update();

        const std::array events = {
            event(CollisionEventType::CollisionBegan, fragile, floor),
            event(CollisionEventType::CollisionBegan, wrong, floor),
            // Until the telling is over, the fragile one is there, and hears on.
            event(CollisionEventType::CollisionBegan, fragile, wrong),
            event(CollisionEventType::CollisionEnded, wrong, floor),
        };
        stage.scripts->tellContacts(stage.context(), events);
        const Lines said = stage.said();
        REQUIRE(said.size() == 6);
        CHECK(said[0] == "fragile met Floor");
        CHECK(said[1].starts_with("Floor met Entity(Fragile) "));
        CHECK(said[2].starts_with("Floor met Entity(Wrong) "));
        CHECK(said[3] == "fragile met Wrong");
        CHECK(said[4] == "Floor parted from Entity(Wrong)\t0");
        // Destroyed once all were told, with the word it is owed.
        CHECK(said[5] == "fragile is gone");
        CHECK_FALSE(stage.world.isValid(fragile));
        CHECK(stage.world.isValid(wrong));
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("'Wrong' (scripts/wrong.lua): scripts/wrong.lua:3: it hurt") == 0);
        static_cast<void>(stage.errors());
    }

    TEST_CASE("what a script calls by the name of a contact has to be a function") {
        ScriptStage stage;
        stage.add("Wrong", stage.write("scripts/wrong.lua", "return {triggerEntered = true}"));
        stage.update();
        REQUIRE(stage.problems().size() == 2);
        CHECK(stage.problems()[0] == "scripts/wrong.lua: its 'triggerEntered' is a boolean, not a function");
        static_cast<void>(stage.errors());
    }

    TEST_CASE("the system counts how often it has called each of the functions it calls") {
        ScriptStage stage;
        const auto counts = [&] {
            Lines lines;
            for (const ScriptCallbackInfo& callback : stage.scripts->callbacks()) {
                lines.push_back(std::string(callback.name) + " " + std::to_string(callback.calls));
            }
            return lines;
        };
        CHECK(counts() == Lines{"start 0", "input 0", "update 0", "sequenceEvent 0", "collisionBegan 0",
                                "collisionEnded 0", "triggerEntered 0", "triggerLeft 0", "destroy 0"});

        const Entity first = stage.add("First", stage.write("scripts/all.lua", R"(
            local All = {}
            function All:start() end
            function All:update() end
            function All:sequenceEvent() end
            function All:collisionBegan() end
            function All:collisionEnded() end
            function All:triggerEntered() end
            function All:triggerLeft() end
            function All:destroy() end
            return All
        )"));
        const Entity second = stage.add("Second", stage.world.tryGet<ScriptComponent>(first)->script);
        stage.update();
        stage.update();
        const std::array events = {event(CollisionEventType::CollisionBegan, first, second),
                                   event(CollisionEventType::TriggerEntered, first, second),
                                   event(CollisionEventType::TriggerEntered, second, first),
                                   event(CollisionEventType::TriggerLeft, first, stage.world.createEntity("Wall"))};
        stage.scripts->tellContacts(stage.context(), events);
        const std::array passed = {SequenceEvent{.player = second, .name = "cue"}};
        stage.scripts->tellSequenceEvents(stage.context(), passed);
        stage.scripts->clear(stage.context());
        // Of all instances together, since the system was made: clearing it ends them,
        // and does not forget.
        CHECK(counts() == Lines{"start 2", "input 0", "update 4", "sequenceEvent 1", "collisionBegan 2",
                                "collisionEnded 0", "triggerEntered 4", "triggerLeft 1", "destroy 2"});
    }

    TEST_CASE("what the simulation sees is what the scripts hear") {
        ScriptStage stage;
        stage.simulate();
        World& world = stage.world;
        const Uuid witness = stage.write("scripts/witness.lua", kWitness);
        const Entity floor = stage.add("Floor", witness);
        world.add<ColliderComponent>(floor) = {.size = Vec3(50.0f, 1.0f, 50.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
        // A crate that hangs a metre over the floor, and falls.
        const Entity crate = stage.add("Crate", witness);
        world.transform(crate).position = Vec3(0.0f, 1.5f, 0.0f);
        world.add<ColliderComponent>(crate);
        world.add<RigidBodyComponent>(crate) = {.allowSleep = false};
        // A trigger off to the side, a ball's width above the floor.
        const Entity zone = stage.add("Zone", witness);
        world.transform(zone).position = Vec3(10.0f, 3.0f, 0.0f);
        world.add<ColliderComponent>(zone) = {.size = Vec3(2.0f), .trigger = true};

        const auto run = [&](int frames) {
            Lines said;
            for (int frame = 0; frame < frames; ++frame) {
                stage.update(1.0 / 60.0);
                for (std::string& line : stage.said()) {
                    said.push_back(std::move(line));
                }
            }
            return said;
        };

        // It lands within a second, at more than four metres a second: each is told of
        // the other, and of where it lies.
        Lines said = run(60);
        std::ranges::sort(said);
        CHECK(said == Lines{"Crate met Floor towards 0 -1 0\ttrue", "Floor met Crate towards 0 1 0\ttrue"});

        // Put into the trigger - an entity is where it is put - it is told that, and
        // that it no longer stands on the floor.
        world.transform(crate).position = Vec3(10.0f, 3.0f, 0.0f);
        world.tryGet<RigidBodyComponent>(crate)->gravityScale = 0.0f;
        world.tryGet<RigidBodyComponent>(crate)->linearVelocity = Vec3(0.0f);
        said = run(3);
        std::ranges::sort(said);
        CHECK(said == Lines{"Crate is in a trigger with Zone", "Crate parted from Entity(Floor)",
                            "Floor parted from Entity(Crate)", "Zone is in a trigger with Crate"});

        // Destroyed in there, it has left - and is told nothing, being gone.
        world.destroyEntity(crate);
        CHECK(run(3) == Lines{"Zone is out of a trigger with Entity(gone)"});
        CHECK(stage.problems().empty());
    }
}
