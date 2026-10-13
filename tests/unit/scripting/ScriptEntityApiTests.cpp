#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;
using cordite::test::unwrap;

// Inside ScriptRuntime::protect() a test uses CHECK, never REQUIRE or unwrap(): Lua takes
// whatever is thrown through it for an error of a script, and the test would go on.

namespace {

using Lines = std::vector<std::string>;

/// A script whose `update` is `body`, with the entity as `entity`.
std::string updating(std::string_view body) {
    return "return {update = function(self)\n    local entity = self.entity\n" + std::string(body) + "\nend}";
}

/// What an entity that runs `body` as its update is stopped with.
std::string failure(std::string_view body) {
    ScriptStage stage;
    stage.add("Door", stage.write("scripts/door.lua", updating(body)));
    stage.update();
    const std::vector<std::string> problems = stage.problems();
    REQUIRE(problems.size() == 1);
    // The first line: what went wrong, without the calls that led there.
    return problems[0].substr(0, problems[0].find('\n'));
}

} // namespace

TEST_SUITE("ScriptEntityApi") {
    TEST_CASE("a script reads and changes what its entity is called and whether it is on") {
        ScriptStage stage;
        const Uuid script = stage.write("scripts/door.lua", updating(R"(
            print(entity:name(), entity:id(), entity:isValid(), entity:isEnabled())
            entity:setName(entity:name() .. "!")
            print(tostring(entity), entity)
        )"));
        const Entity door = stage.add("Door", script);
        const std::string id = stage.world.id(door).toString();

        stage.update();
        CHECK(stage.said() == Lines{"Door\t" + id + "\ttrue\ttrue", "Entity(Door!)\tEntity(Door!)"});
        CHECK(stage.world.name(door) == "Door!");
        CHECK(stage.problems().empty());
    }

    TEST_CASE("the same entity is the same value to every script, and a key like any other") {
        ScriptStage stage;
        const Uuid script = stage.write("scripts/meeting.lua", R"(
            local Meeting = {}
            local seen = {}                    -- by entity
            local order = {}

            function Meeting:start()
                seen[self.entity] = self.entity:name()
                order[#order + 1] = self.entity
            end

            function Meeting:update()
                local mine = 0
                for index, entity in ipairs(order) do
                    if entity == self.entity then mine = index end
                end
                print(self.entity:name(), mine, seen[self.entity], rawequal(order[mine], self.entity), #order)
            end

            return Meeting
        )");
        stage.add("First", script);
        stage.add("Second", script);
        stage.update();
        CHECK(stage.said() == Lines{"First\t1\tFirst\ttrue\t2", "Second\t2\tSecond\ttrue\t2"});
    }

    TEST_CASE("an entity that is switched off by its script is off") {
        ScriptStage stage;
        const Uuid script = stage.write("scripts/door.lua", updating(R"(
            entity:setEnabled(false)
            print(entity:isEnabled(), entity:isValid())
        )"));
        const Entity door = stage.add("Door", script);
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"false\ttrue"});
        CHECK_FALSE(stage.world.isEnabled(door));
    }

    TEST_CASE("an entity that is gone can be asked whether it is there, and nothing else") {
        ScriptStage stage;
        const Uuid watcher = stage.write("scripts/watcher.lua", R"(
            local Watcher = { properties = { watches = false } }
            local watched

            function Watcher:start()
                if not self.properties.watches then watched = self.entity end
            end

            function Watcher:update()
                if self.properties.watches then
                    print(watched:isValid(), tostring(watched))
                    if not watched:isValid() then
                        print(pcall(watched.name, watched))
                        print(pcall(watched.setEnabled, watched, true))
                        print(pcall(watched.destroy, watched))
                    end
                end
            end

            return Watcher
        )");
        const Entity watched = stage.add("Watched", watcher);
        stage.add("Watcher", watcher, unwrap(parseJson(R"({"watches": true})")));
        stage.update();
        CHECK(stage.said() == Lines{"true\tEntity(Watched)"});

        stage.world.destroyEntity(watched);
        stage.update();
        CHECK(stage.said() == Lines{
                                  "false\tEntity(gone)",
                                  // Called through pcall() a function has no name where it is called,
                                  // and Lua finds the one it was registered under.
                                  "false\tbad argument #1 to 'cordite.Entity.name' (the entity is gone: it was "
                                  "destroyed)",
                                  "false\tbad argument #1 to 'cordite.Entity.setEnabled' (the entity is gone: it "
                                  "was destroyed)",
                                  "false\tbad argument #1 to 'cordite.Entity.destroy' (the entity is gone: it was "
                                  "destroyed)",
                              });
        CHECK(stage.problems().empty());
    }

    TEST_CASE("what is asked of an entity wrongly is an error that says how") {
        CHECK(failure("entity:setName()") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'setName' (string expected, got no "
              "value)");
        CHECK(failure("entity:setName({})") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'setName' (string expected, got "
              "table)");
        CHECK(failure("entity:setEnabled(1)") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'setEnabled' (boolean expected, got "
              "number)");
        CHECK(failure("entity.name('Door')") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'name' (Entity expected, got "
              "string)");
        CHECK(failure("entity.name()") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'name' (Entity expected, got no "
              "value)");
        CHECK(failure("cordite.Entity.isValid({})") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: bad argument #1 to 'isValid' (Entity expected, got "
              "table)");
        CHECK(failure("entity:fly()") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: attempt to call a nil value (method 'fly')");
        CHECK(failure("entity.health = 3") ==
              "'Door' (scripts/door.lua): scripts/door.lua:3: attempt to index a Entity value (local 'entity')");
    }

    TEST_CASE("the functions of entities are said in words, and counted") {
        ScriptStage stage;
        stage.add("Door", stage.write("scripts/door.lua", updating("entity:name() entity:name() entity:isValid()")));
        stage.update();

        std::vector<std::string> names;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            if (!function.name.starts_with("cordite.Entity.")) {
                continue;
            }
            CAPTURE(function.name);
            names.push_back(function.name.substr(15));
            CHECK(function.signature.find('(') != std::string::npos);
            CHECK(function.description.size() > 20);
            CHECK(function.description.back() == '.');
            if (function.name == "cordite.Entity.name") {
                CHECK(function.calls == 2);
            } else if (function.name == "cordite.Entity.isValid") {
                CHECK(function.calls == 1);
            } else {
                CHECK(function.calls == 0);
            }
        }
        // Everything a script can ask of an entity, in the order it was given to scripts.
        CHECK(
            names ==
            std::vector<std::string>{
                "isValid",  "id",          "name",   "setName",   "isEnabled",     "setEnabled",       "destroy",
                "script",   "__tostring",  "parent", "setParent", "children",      "position",         "setPosition",
                "rotation", "setRotation", "scale",  "setScale",  "worldPosition", "setWorldPosition", "worldRotation",
                "forward",  "right",       "up",     "translate", "rotate",        "lookAt",           "has",
                "get",      "set",         "add",    "remove",    "addForce",      "addImpulse",       "addTorque",
            });
    }

    TEST_CASE("outside an update there is no world for a script to reach") {
        ScriptStage stage;
        const Entity door = stage.world.createEntity("Door");

        std::string said;
        const Status status = stage.scripts->runtime().protect([&](lua_State* state) {
            // The value itself needs no world: it is only a name for the entity.
            pushEntity(state, door);
            CHECK(checkEntityValue(state, -1) == door);
            lua_getfield(state, -1, "name");
            lua_pushvalue(state, -2);
            if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
                said = lua_tostring(state, -1);
            }
        });
        REQUIRE_MESSAGE(status.has_value(), test::errorOf(status));
        CHECK(said == "there is no world to reach from here: no update of the scripts is under way");

        // The null entity is nil to a script.
        bool isNil = false;
        const Status null = stage.scripts->runtime().protect([&](lua_State* state) {
            pushEntity(state, Entity{});
            isNil = lua_isnil(state, -1);
        });
        REQUIRE_MESSAGE(null.has_value(), test::errorOf(null));
        CHECK(isNil);
    }

    TEST_CASE("the value of an entity that no script holds is not kept") {
        ScriptStage stage;
        const size_t atRest = stage.update().memoryBytes;
        const Status pushed = stage.scripts->runtime().protect([&](lua_State* state) {
            for (int index = 0; index < 20000; ++index) {
                pushEntity(state, stage.world.createEntity("Crowd"));
                lua_pop(state, 1);
            }
        });
        REQUIRE_MESSAGE(pushed.has_value(), test::errorOf(pushed));
        stage.scripts->runtime().collectGarbage();
        // The table they were looked up in is as large as it grew; the values are gone.
        CHECK(stage.update().memoryBytes < atRest + size_t{20000} * 40);
    }
}
