#include "cordite/scripting/ScriptSystem.h"

#include "cordite/core/Json.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/world/SceneSerializer.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <fmt/format.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::kScriptFrame;
using cordite::test::ScriptStage;
using cordite::test::unwrap;

namespace {

using Lines = std::vector<std::string>;

/// A script that says what happens to it.
constexpr std::string_view kTalker = R"(
local Talker = {}

function Talker:start()
    print("start " .. self.entity:name())
end

function Talker:update(deltaSeconds)
    print("update " .. self.entity:name() .. " " .. deltaSeconds)
end

function Talker:destroy()
    print("destroy " .. self.entity:name())
end

return Talker
)";

/// A script that only says that it starts and ends.
constexpr std::string_view kQuiet = R"(
local Quiet = {}
function Quiet:start() print("start " .. self.entity:name()) end
function Quiet:destroy() print("destroy " .. self.entity:name()) end
return Quiet
)";

bool contains(const std::string& text, std::string_view fragment) {
    return text.find(fragment) != std::string::npos;
}

} // namespace

TEST_SUITE("ScriptSystem") {
    TEST_CASE("a script runs for the entity that names it: start once, then update every time") {
        ScriptStage stage;
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        stage.add("Door", talker);

        const ScriptStatistics first = stage.update();
        CHECK(stage.said() == Lines{"start Door", "update Door 0.5"});
        CHECK(first.scripts == 1);
        CHECK(first.instances == 1);
        CHECK(first.stopped == 0);
        CHECK(first.calls == 2);
        CHECK(first.memoryBytes > 0);

        const ScriptStatistics second = stage.update(0.25f);
        CHECK(stage.said() == Lines{"update Door 0.25"});
        CHECK(second.calls == 1);
        CHECK(second.instances == 1);

        // Less than no time, and a time that is no number, is no time.
        stage.update(-1.0f);
        stage.update(std::numeric_limits<float>::quiet_NaN());
        stage.update(std::numeric_limits<float>::infinity());
        CHECK(stage.said() == Lines{"update Door 0.0", "update Door 0.0", "update Door 0.0"});
        CHECK(stage.problems().empty());
        CHECK(stage.errors().empty());
    }

    TEST_CASE("a script is what it has: one without functions runs, and nothing is called") {
        ScriptStage stage;
        const Uuid empty = stage.write("scripts/empty.lua", "return {}");
        const Uuid starter = stage.write("scripts/starter.lua", "return {start = function(self) print('once') end}");
        stage.add("Empty", empty);
        stage.add("Starter", starter);
        // A Script that names no script asks for nothing.
        stage.add("Unset", Uuid());

        const ScriptStatistics first = stage.update();
        CHECK(first.scripts == 2);
        CHECK(first.instances == 2);
        CHECK(first.calls == 1);
        CHECK(stage.update().calls == 0);
        CHECK(stage.said() == Lines{"once"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("instances start and update in the order of the hierarchy, parents first") {
        ScriptStage stage;
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        // Made in an order that is not that of the hierarchy.
        const Entity leaf = stage.add("Leaf", talker);
        const Entity second = stage.add("Second", talker);
        const Entity branch = stage.add("Branch", talker);
        const Entity first = stage.add("First", talker);
        REQUIRE(stage.world.setParent(leaf, branch).has_value());
        REQUIRE(stage.world.setParent(branch, first).has_value());
        stage.world.setSiblingIndex(first, 0);

        stage.update(1.0f);
        CHECK(stage.said() == Lines{"start First", "start Branch", "start Leaf", "start Second", "update First 1.0",
                                    "update Branch 1.0", "update Leaf 1.0", "update Second 1.0"});
        static_cast<void>(second);
    }

    TEST_CASE("every entity has a table of its own, and the functions are the script's") {
        ScriptStage stage;
        const Uuid counter = stage.write("scripts/counter.lua", R"(
            local Counter = {}
            local instances = 0                      -- the script's own: one for all its instances

            function Counter:start()
                instances = instances + 1
                self.count = 0
            end

            function Counter:update()
                self.count = self.count + 1
                print(self.entity:name() .. " " .. self.count .. " of " .. instances)
                -- What the instance does not have itself it finds in the script.
                assert(self.update == Counter.update and rawget(self, "update") == nil)
            end

            return Counter
        )");
        stage.add("A", counter);
        stage.update();
        stage.add("B", counter);
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"A 1 of 1", "A 2 of 2", "B 1 of 2", "A 3 of 2", "B 2 of 2"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("properties are the script's, with the values the component gives") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", R"(
            local Door = {
                properties = { speed = 2.0, opens = "north", locked = false, stops = {1, 2}, lamp = {on = true}, extra = {} },
            }

            function Door:start()
                local p = self.properties
                print(table.concat({self.entity:name(), p.speed, p.opens, tostring(p.locked), #p.stops,
                                    tostring(p.lamp.on), tostring(next(p.extra))}, " "))
                -- A table of an instance is its own: what one does to it the others do not see.
                p.stops[#p.stops + 1] = 99
                p.lamp.on = false
            end

            return Door
        )");
        stage.add("Plain", door);
        stage.add("Fast", door, unwrap(parseJson(R"({"speed": 7, "locked": true})")));
        stage.add(
            "Odd", door,
            unwrap(parseJson(R"({"opens": "south", "stops": [], "lamp": {"on": false, "dim": 1}, "extra": [5]})")));
        stage.add("Named", door, unwrap(parseJson(R"({"extra": {"a": 1}})")));
        stage.update();

        CHECK(stage.said() == Lines{"Plain 2.0 north false 2 true nil", "Fast 7 north true 2 true nil",
                                    "Odd 2.0 south false 0 false 1", "Named 2.0 north false 2 true a"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a component may only give what the script declares, and of the kind it declares") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", R"(
            local Door = { properties = { speed = 2.0, opens = "north", stops = {1, 2}, lamp = {on = true}, any = cordite.null } }
            function Door:start() print("start " .. self.entity:name()) end
            return Door
        )");
        const Uuid bare = stage.write("scripts/bare.lua", "return {start = function(self) print('bare') end}");
        stage.add("Typo", door, unwrap(parseJson(R"({"sped": 3})")));
        stage.add("Text", door, unwrap(parseJson(R"({"speed": "fast"})")));
        stage.add("Number", door, unwrap(parseJson(R"({"opens": 3})")));
        stage.add("List", door, unwrap(parseJson(R"({"lamp": [1]})")));
        stage.add("Names", door, unwrap(parseJson(R"({"stops": {"a": 1}})")));
        stage.add("Flag", door, unwrap(parseJson(R"({"speed": true})")));
        stage.add("Undeclared", bare, unwrap(parseJson(R"({"speed": 1})")));
        // What the script declares as nothing in particular takes anything.
        stage.add("Free", door, unwrap(parseJson(R"({"any": [1, "two"], "speed": 4})")));

        const ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"start Free"});
        CHECK(statistics.instances == 8);
        CHECK(statistics.stopped == 7);
        const Lines expected = {
            "'Typo' (scripts/door.lua): its Script gives 'sped' a value, which the script does not declare among "
            "its properties",
            "'Text' (scripts/door.lua): its Script gives 'speed' text, where the script declares a number",
            "'Number' (scripts/door.lua): its Script gives 'opens' a number, where the script declares text",
            "'List' (scripts/door.lua): its Script gives 'lamp' a list, where the script declares a table of names",
            "'Names' (scripts/door.lua): its Script gives 'stops' a table of names, where the script declares a list",
            "'Flag' (scripts/door.lua): its Script gives 'speed' true or false, where the script declares a number",
            "'Undeclared' (scripts/bare.lua): its Script gives 'speed' a value, which the script does not declare "
            "among its properties",
        };
        CHECK(stage.problems() == expected);
        // Said once each, and not again with every update.
        CHECK(stage.errors() == expected);
        stage.update();
        CHECK(stage.errors().empty());
        CHECK(stage.problems() == expected);
        CHECK(stage.said().empty());
    }

    TEST_CASE("an error stops the instance, and is said once") {
        ScriptStage stage;
        const Uuid fragile = stage.write("scripts/fragile.lua", R"(
            local Fragile = { properties = { breaks = 0 } }
            function Fragile:start() self.turn = 0 end
            function Fragile:update()
                self.turn = self.turn + 1
                if self.turn == self.properties.breaks then
                    local door = nil
                    door.open = true
                end
                print(self.entity:name() .. " " .. self.turn)
            end
            function Fragile:destroy() print("destroy " .. self.entity:name()) end
            return Fragile
        )");
        const Entity breaks = stage.add("Breaks", fragile, unwrap(parseJson(R"({"breaks": 2})")));
        stage.add("Holds", fragile);

        stage.update();
        CHECK(stage.said() == Lines{"Breaks 1", "Holds 1"});
        CHECK(stage.problems().empty());

        const ScriptStatistics broken = stage.update();
        CHECK(stage.said() == Lines{"Holds 2"});
        CHECK(broken.instances == 2);
        CHECK(broken.stopped == 1);
        REQUIRE(stage.scripts->problems().size() == 1);
        const ScriptProblem& problem = stage.scripts->problems()[0];
        CHECK(problem.entity == breaks);
        CHECK(problem.script == fragile);
        CHECK_MESSAGE(problem.message.starts_with("'Breaks' (scripts/fragile.lua): scripts/fragile.lua:8: attempt to "
                                                  "index a nil value (local 'door')\nstack traceback:"),
                      problem.message);
        const Lines said = stage.errors();
        REQUIRE(said.size() == 1);
        CHECK(said[0] == problem.message);

        // From then on nothing of it is called, and nothing more is said - not even
        // that it ends, when its entity is destroyed by a script's doing.
        const ScriptStatistics later = stage.update();
        CHECK(stage.said() == Lines{"Holds 3"});
        CHECK(later.calls == 1);
        CHECK(stage.errors().empty());
        stage.scripts->destroyLater(breaks);
        stage.update();
        CHECK(stage.said() == Lines{"Holds 4"});
        CHECK_FALSE(stage.world.isValid(breaks));
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a start that an error cuts short is not followed by updates, nor by an end") {
        ScriptStage stage;
        const Uuid stillborn = stage.write("scripts/stillborn.lua", R"(
            local Stillborn = {}
            function Stillborn:start() error("no key") end
            function Stillborn:update() print("update") end
            function Stillborn:destroy() print("destroy") end
            return Stillborn
        )");
        const Entity entity = stage.add("Vault", stillborn);
        stage.update();
        stage.update();
        CHECK(stage.said().empty());
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].starts_with("'Vault' (scripts/stillborn.lua): scripts/stillborn.lua:3: no key\n"));
        CHECK(stage.errors().size() == 1);

        stage.world.remove<ScriptComponent>(entity);
        stage.update();
        CHECK(stage.said().empty());
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a script that does not end is stopped like any other error") {
        ScriptStage stage({.runtime = {.instructionLimit = 100'000}});
        const Uuid endless =
            stage.write("scripts/endless.lua", "return {update = function(self) while true do end end}");
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        stage.add("Spinner", endless);
        stage.add("Door", talker);

        stage.update();
        CHECK(stage.said() == Lines{"start Door", "update Door 0.5"});
        REQUIRE(stage.problems().size() == 1);
        CHECK_MESSAGE(contains(stage.problems()[0], "ran for more than 100000 instructions"), stage.problems()[0]);
        stage.update();
        CHECK(stage.said() == Lines{"update Door 0.5"});
        CHECK(stage.errors().size() == 1);
    }

    TEST_CASE("a script that cannot be loaded is a problem, and the entities that name it wait") {
        ScriptStage stage;
        const Uuid broken = stage.write("scripts/broken.lua", "local Door = {}\nfunction Door:start(\nreturn Door");
        const Uuid number = stage.write("scripts/number.lua", "return 42");
        const Uuid nothing = stage.write("scripts/nothing.lua", "local unused = 1");
        const Uuid falls = stage.write("scripts/falls.lua", "local Door = {}\nDoor.speed = missing.speed\nreturn Door");
        const Uuid list = stage.write("scripts/list.lua", "return {properties = {1, 2}}");
        const Uuid text = stage.write("scripts/text.lua", "return {properties = 'fast'}");
        const Uuid function = stage.write("scripts/function.lua", "return {properties = {open = print}}");
        const Uuid update = stage.write("scripts/update.lua", "return {update = 5}");
        const Uuid unknown = Uuid::parse("0123abcd-0123-4abc-8def-0123456789ab").value_or(Uuid());
        REQUIRE_FALSE(unknown.isNil());
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        int count = 0;
        for (const Uuid& script : {broken, number, nothing, falls, list, text, function, update, unknown}) {
            stage.add(fmt::format("Entity{}", ++count), script);
        }
        stage.add("Second", broken);
        stage.add("Door", talker);

        const ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"start Door", "update Door 0.5"});
        CHECK(statistics.scripts == 1);
        CHECK(statistics.instances == 11);
        CHECK(statistics.stopped == 10);

        const Lines problems = stage.problems();
        REQUIRE(problems.size() == 19);
        // The scripts first, each once however many entities name it.
        CHECK(problems[0] == "scripts/broken.lua:3: <name> or '...' expected near 'return'");
        CHECK(problems[1] == "scripts/number.lua: it returns a number; a script returns the table of its functions");
        CHECK(problems[2] == "scripts/nothing.lua: it returns nothing; a script returns the table of its functions");
        CHECK_MESSAGE(problems[3].starts_with("scripts/falls.lua:2: 'missing' is not defined"), problems[3]);
        CHECK(problems[4] == "scripts/list.lua: its 'properties' are a list; they have names");
        CHECK(problems[5] == "scripts/text.lua: its 'properties' are a string, not a table");
        CHECK(problems[6] ==
              "scripts/function.lua: its 'properties' cannot be kept as data: 'open': a function cannot be stored");
        CHECK(problems[7] == "scripts/update.lua: its 'update' is a number, not a function");
        CHECK(problems[8] == "there is no asset with the id 0123abcd-0123-4abc-8def-0123456789ab");
        // Then the entities that wait for them.
        CHECK(problems[9] == "'Entity1' (scripts/broken.lua): its script could not be loaded");
        CHECK(problems[17] == "'Entity9' (0123abcd-0123-4abc-8def-0123456789ab): its script could not be loaded");
        CHECK(problems[18] == "'Second' (scripts/broken.lua): its script could not be loaded");
        // What is wrong with a script is said once; of the entities nothing is said besides.
        CHECK(stage.errors().size() == 9);
        stage.update();
        CHECK(stage.errors().empty());
        CHECK(stage.problems() == problems);

        const ScriptProblem& first = stage.scripts->problems()[0];
        CHECK(first.script == broken);
        CHECK(first.entity.isNull());
        CHECK(stage.scripts->problems()[9].script == broken);
        CHECK_FALSE(stage.scripts->problems()[9].entity.isNull());
    }

    TEST_CASE("an instance ends when its Script is taken off, names another script or is given other properties") {
        ScriptStage stage;
        const Uuid quiet = stage.write("scripts/quiet.lua", kQuiet);
        const Uuid other = stage.write("scripts/other.lua", "return {start = function(self) print('other start') end}");
        const Entity entity = stage.add("Door", quiet);
        stage.update();
        CHECK(stage.said() == Lines{"start Door"});

        // Another script: the instance that was is told, and one of the other starts.
        stage.world.tryGet<ScriptComponent>(entity)->script = other;
        CHECK(stage.update().instances == 1);
        CHECK(stage.said() == Lines{"destroy Door", "other start"});

        stage.world.tryGet<ScriptComponent>(entity)->script = quiet;
        stage.update();
        CHECK(stage.said() == Lines{"start Door"});

        stage.world.remove<ScriptComponent>(entity);
        const ScriptStatistics gone = stage.update();
        CHECK(stage.said() == Lines{"destroy Door"});
        CHECK(gone.instances == 0);
        CHECK(gone.scripts == 2);

        // A Script that names nothing is one that asks for nothing.
        stage.world.add<ScriptComponent>(entity).script = quiet;
        stage.update();
        stage.world.tryGet<ScriptComponent>(entity)->script = Uuid();
        CHECK(stage.update().instances == 0);
        CHECK(stage.said() == Lines{"start Door", "destroy Door"});
    }

    TEST_CASE("other properties make another instance") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", R"(
            local Door = { properties = { speed = 1 } }
            function Door:start() print("start at " .. self.properties.speed) end
            function Door:destroy() print("destroy at " .. self.properties.speed) end
            return Door
        )");
        const Entity entity = stage.add("Door", door);
        stage.update();
        stage.world.tryGet<ScriptComponent>(entity)->properties = unwrap(parseJson(R"({"speed": 5})"));
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"start at 1", "destroy at 1", "start at 5"});
    }

    TEST_CASE("an entity that a script destroys is gone after the update, and its scripts are told first") {
        ScriptStage stage;
        const Uuid quiet = stage.write("scripts/quiet.lua", kQuiet);
        const Uuid bomb = stage.write("scripts/bomb.lua", R"(
            local Bomb = { properties = { fuse = 2 } }
            function Bomb:start() self.left = self.properties.fuse end
            function Bomb:update()
                self.left = self.left - 1
                if self.left == 0 then
                    self.entity:destroy()
                    self.entity:destroy()                       -- asking twice is asking once
                    print("boom, and still " .. tostring(self.entity:isValid()))
                end
            end
            function Bomb:destroy() print("destroy " .. self.entity:name()) end
            return Bomb
        )");
        const Entity crate = stage.add("Crate", bomb);
        const Entity lid = stage.add("Lid", quiet, Json::object(), crate);
        const Entity label = stage.add("Label", quiet, Json::object(), lid);
        const Entity plain = stage.world.createEntity("Nail");
        REQUIRE(stage.world.setParent(plain, crate).has_value());
        const Entity bystander = stage.add("Wall", quiet);

        stage.update();
        CHECK(stage.said() == Lines{"start Lid", "start Label", "start Wall"});
        const ScriptStatistics after = stage.update();
        // What hangs lowest is told first.
        CHECK(stage.said() == Lines{"boom, and still true", "destroy Label", "destroy Lid", "destroy Crate"});
        for (const Entity entity : {crate, lid, label, plain}) {
            CHECK_FALSE(stage.world.isValid(entity));
        }
        CHECK(stage.world.isValid(bystander));
        CHECK(after.instances == 1);
        CHECK(stage.world.entityCount() == 1);
        stage.update();
        CHECK(stage.said().empty());
        CHECK(stage.problems().empty());
    }

    TEST_CASE("what a script does while it is told that it ends is done too") {
        ScriptStage stage;
        const Uuid chain = stage.write("scripts/chain.lua", R"(
            local Chain = {}
            local links = {}                                    -- every entity that runs this script
            function Chain:start() links[#links + 1] = self.entity end
            function Chain:update() if self.entity:name() == "Link1" then self.entity:destroy() end end
            function Chain:destroy()
                print("destroy " .. self.entity:name())
                -- Pulls the next one along, and what is already going is not pulled twice.
                for _, link in ipairs(links) do
                    if link:isValid() then link:destroy() end
                end
            end
            return Chain
        )");
        stage.add("Link1", chain);
        stage.add("Link2", chain);
        stage.add("Link3", chain);
        stage.update();
        CHECK(stage.said() == Lines{"destroy Link1", "destroy Link2", "destroy Link3"});
        CHECK(stage.world.entityCount() == 0);
        CHECK(stage.update().instances == 0);
    }

    TEST_CASE("an entity that something else destroys takes its instance along without a word") {
        ScriptStage stage;
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        const Entity doomed = stage.add("Doomed", talker);
        stage.add("Stays", talker);
        stage.update();
        static_cast<void>(stage.said());

        stage.world.destroyEntity(doomed);
        const ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"update Stays 0.5"});
        CHECK(statistics.instances == 1);
        CHECK(stage.problems().empty());
        CHECK(stage.errors().empty());

        // The same for a whole world that is emptied.
        stage.world.clear();
        CHECK(stage.update().instances == 0);
        CHECK(stage.said().empty());
    }

    TEST_CASE("an entity that is switched off is left alone") {
        ScriptStage stage;
        const Uuid talker = stage.write("scripts/talker.lua", kTalker);
        const Entity room = stage.world.createEntity("Room");
        const Entity lamp = stage.add("Lamp", talker, Json::object(), room);
        stage.world.setEnabled(lamp, false);

        // Not started while it is off.
        CHECK(stage.update().instances == 0);
        CHECK(stage.said().empty());
        stage.world.setEnabled(lamp, true);
        stage.update();
        CHECK(stage.said() == Lines{"start Lamp", "update Lamp 0.5"});

        // Off because what it hangs under is: nothing is called, and nothing has ended.
        stage.world.setEnabled(room, false);
        const ScriptStatistics off = stage.update();
        CHECK(stage.said().empty());
        CHECK(off.instances == 1);
        CHECK(off.calls == 0);
        stage.world.setEnabled(room, true);
        stage.update();
        CHECK(stage.said() == Lines{"update Lamp 0.5"});
    }

    TEST_CASE("a script that switches an entity off has done so for the rest of the update") {
        ScriptStage stage;
        const Uuid switcher = stage.write("scripts/switcher.lua", R"(
            local Switcher = { properties = { target = false } }
            local targets = {}
            function Switcher:start()
                if self.properties.target then targets[#targets + 1] = self.entity end
            end
            function Switcher:update()
                print("update " .. self.entity:name())
                for _, target in ipairs(targets) do target:setEnabled(false) end
            end
            return Switcher
        )");
        stage.add("First", switcher);
        const Entity target = stage.add("Target", switcher, unwrap(parseJson(R"({"target": true})")));
        stage.update();
        CHECK(stage.said() == Lines{"update First"});
        CHECK_FALSE(stage.world.isEnabled(target));
        stage.update();
        CHECK(stage.said() == Lines{"update First"});
    }

    TEST_CASE(
        "when the assets are refreshed, scripts that changed are loaded again and instances keep what they know") {
        ScriptStage stage;
        const Uuid counter = stage.write("scripts/counter.lua", R"(
            local Counter = {}
            function Counter:start() self.count = 0 print("start") end
            function Counter:update() self.count = self.count + 1 print("old " .. self.count) end
            return Counter
        )");
        // What a script keeps for itself - not for an instance - lasts as long as its
        // text: a script that is loaded again starts counting anew.
        const Uuid other = stage.write(
            "scripts/other.lua", "local calls = 0\n"
                                 "return {update = function(self) calls = calls + 1 print('other ' .. calls) end}");
        stage.add("Counter", counter);
        stage.add("Other", other);
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"start", "old 1", "other 1", "old 2", "other 2"});

        // A refresh that changes nothing changes nothing.
        REQUIRE(stage.assets->refresh().has_value());
        stage.update();
        CHECK(stage.said() == Lines{"old 3", "other 3"});

        // New text: the functions are the new ones, the instance is the one it was -
        // it is not started again, and it knows what it knew.
        stage.write("scripts/counter.lua", R"(
            local Counter = {}
            function Counter:start() self.count = 100 print("start again") end
            function Counter:update() self.count = self.count + 10 print("new " .. self.count) end
            function Counter:destroy() print("destroy at " .. self.count) end
            return Counter
        )");
        stage.update();
        // The script beside it has the text it had, and was left alone.
        CHECK(stage.said() == Lines{"new 13", "other 4"});
        CHECK(stage.update().scripts == 2);
        CHECK(stage.said() == Lines{"new 23", "other 5"});

        // A function that the new text has and the old had not is called from then on.
        stage.world.remove<ScriptComponent>(stage.world.findByName("Counter"));
        stage.update();
        CHECK(stage.said() == Lines{"destroy at 23", "other 6"});
        CHECK(stage.problems().empty());

        // Loaded again, the script that counted for itself counts from nothing.
        stage.write("scripts/other.lua",
                    "local calls = 0\n"
                    "return {update = function(self) calls = calls + 1 print('another ' .. calls) end}");
        stage.update();
        CHECK(stage.said() == Lines{"another 1"});
    }

    TEST_CASE("new text that cannot be loaded leaves the old running, and text that is put right starts what waited") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", "return {update = function(self) print('first') end}");
        stage.add("Door", door);
        stage.update();
        CHECK(stage.said() == Lines{"first"});

        stage.write("scripts/door.lua", "return {update = function(self) print('second') end");
        const ScriptStatistics kept = stage.update();
        CHECK(stage.said() == Lines{"first"});
        CHECK(kept.scripts == 1);
        CHECK(kept.stopped == 0);
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0] == "scripts/door.lua:1: '}' expected near <eof>");
        CHECK(stage.errors().size() == 1);
        // The same text again is not tried again, and not complained of again.
        REQUIRE(stage.assets->refresh().has_value());
        stage.update();
        CHECK(stage.errors().empty());
        CHECK(stage.problems().size() == 1);
        CHECK(stage.said() == Lines{"first"});

        stage.write("scripts/door.lua", "return {update = function(self) print('third') end}");
        stage.update();
        CHECK(stage.said() == Lines{"third"});
        CHECK(stage.problems().empty());

        // A script that never loaded: the entities that name it start once it does.
        const Uuid late = stage.write("scripts/late.lua", "return nil");
        stage.add("Late", late);
        stage.update();
        CHECK(stage.problems().size() == 2);
        static_cast<void>(stage.said());
        stage.write("scripts/late.lua", "return {start = function(self) print('late start') end}");
        const ScriptStatistics started = stage.update();
        // What is new starts before anything is updated.
        CHECK(stage.said() == Lines{"late start", "third"});
        CHECK(started.stopped == 0);
        CHECK(stage.problems().empty());
    }

    TEST_CASE("an instance that an error stopped is given another go by text that is new") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", R"(
            local Door = {}
            function Door:start() self.opened = 0 print("start") end
            function Door:update() self.opened = self.opened + 1 error("jammed at " .. self.opened, 0) end
            return Door
        )");
        const Uuid vault = stage.write("scripts/vault.lua", "return {start = function(self) error('no key', 0) end}");
        stage.add("Door", door);
        stage.add("Vault", vault);
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"start"});
        CHECK(stage.problems().size() == 2);

        stage.write("scripts/door.lua", R"(
            local Door = {}
            function Door:start() print("start again") end
            function Door:update() self.opened = self.opened + 1 print("opened " .. self.opened) end
            return Door
        )");
        stage.update();
        // It had started, and is not started again; it goes on from where it stopped.
        CHECK(stage.said() == Lines{"opened 2"});
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].starts_with("'Vault' (scripts/vault.lua): no key"));

        // One whose start never came to an end is started.
        stage.write("scripts/vault.lua", "return {start = function(self) print('vault start') end}");
        stage.update();
        CHECK(stage.said() == Lines{"vault start", "opened 3"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("clearing ends every instance and forgets every script") {
        ScriptStage stage;
        const Uuid quiet = stage.write("scripts/quiet.lua", kQuiet);
        const Entity room = stage.add("Room", quiet);
        stage.add("Lamp", quiet, Json::object(), room);
        const Entity gone = stage.add("Gone", quiet);
        stage.update();
        static_cast<void>(stage.said());
        stage.world.destroyEntity(gone);

        stage.scripts->clear(stage.context());
        // The last made first, and nothing for what is no longer there.
        CHECK(stage.said() == Lines{"destroy Lamp", "destroy Room"});
        CHECK(stage.scripts->problems().empty());

        // The world is as it was, and the next update starts from nothing.
        const ScriptStatistics again = stage.update();
        CHECK(stage.said() == Lines{"start Room", "start Lamp"});
        CHECK(again.scripts == 1);
        CHECK(again.instances == 2);
    }

    TEST_CASE("what scripts hold is given back when their instances end") {
        ScriptStage stage;
        const Uuid hoarder = stage.write("scripts/hoarder.lua", R"(
            local Hoarder = {}
            function Hoarder:start() self.hoard = string.rep("x", 1024 * 1024) end
            return Hoarder
        )");
        const size_t atRest = stage.update().memoryBytes;
        std::vector<Entity> entities;
        for (int index = 0; index < 8; ++index) {
            entities.push_back(stage.add("Hoarder", hoarder));
        }
        CHECK(stage.update().memoryBytes > atRest + size_t{8} * 1024 * 1024);

        for (const Entity entity : entities) {
            stage.world.destroyEntity(entity);
        }
        stage.update();
        // Lua gives memory back when it sees fit, or when it is asked to.
        stage.scripts->runtime().collectGarbage();
        CHECK(stage.update().memoryBytes < atRest + size_t{64} * 1024);
    }

    TEST_CASE("a Script is saved with its scene and read back as it was") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", "return {properties = {speed = 1, stops = {}}}");
        const Entity entity =
            stage.add("Door", door, unwrap(parseJson(R"({"speed": 2.5, "stops": ["north", {"at": [1, 2, 3]}]})")));
        static_cast<void>(entity);

        const Json scene = serializeScene(stage.world, stage.components);
        const Json& saved = scene["entities"][0]["components"]["Script"];
        CHECK(dumpJson(saved, -1) ==
              fmt::format(R"({{"script":"{}","properties":{{"speed":2.5,"stops":["north",{{"at":[1,2,3]}}]}}}})",
                          door.toString()));

        World loaded;
        REQUIRE(deserializeScene(loaded, scene, stage.components).has_value());
        const ScriptComponent* script = loaded.tryGet<ScriptComponent>(loaded.findByName("Door"));
        REQUIRE(script != nullptr);
        CHECK(*script == *stage.world.tryGet<ScriptComponent>(entity));
        CHECK(dumpJson(serializeScene(loaded, stage.components)) == dumpJson(scene));

        // What a scene says of a Script is checked like everything else in it.
        Json wrong = scene;
        wrong["entities"][0]["components"]["Script"]["properties"] = Json::array({1, 2});
        World refused;
        const Status status = deserializeScene(refused, wrong, stage.components);
        REQUIRE_FALSE(status.has_value());
        const std::string refusal = status.error().message;
        CHECK_MESSAGE(contains(refusal, "property 'properties': expected an object, found array"), refusal);
    }
}
