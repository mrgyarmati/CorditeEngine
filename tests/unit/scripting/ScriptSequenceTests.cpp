#include "cordite/animation/SequenceSystem.h"
#include "cordite/core/Uuid.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// A script that says every event it is told, and how much more it was given.
constexpr const char* kAudience = R"(
    local Audience = {}
    function Audience:sequenceEvent(name, ...)
        print(self.entity:name() .. " heard " .. name, select("#", ...))
    end
    return Audience
)";

} // namespace

TEST_SUITE("ScriptSequences") {
    TEST_CASE("the script of the entity that plays a sequence is told its events, in order, by their names") {
        ScriptStage stage;
        const Uuid audience = stage.write("scripts/audience.lua", kAudience);
        const Entity director = stage.add("Director", audience);
        const Entity second = stage.add("Second", audience);
        stage.update();

        const std::vector<SequenceEvent> events = {{.player = director, .name = "lights"},
                                                   {.player = second, .name = "thunder"},
                                                   {.player = director, .name = "curtain"}};
        const ScriptStatistics statistics = stage.scripts->tellSequenceEvents(stage.context(), events);
        CHECK(stage.said() ==
              Lines{"Director heard lights\t0", "Second heard thunder\t0", "Director heard curtain\t0"});
        // Counted on from the update before, which called nothing.
        CHECK(statistics.calls == 3);
        CHECK(stage.problems().empty());
        // Nothing to tell is nothing told.
        CHECK(stage.scripts->tellSequenceEvents(stage.context(), {}).calls == 3);
    }

    TEST_CASE("only a script that runs, has started and has a function for it, is told") {
        ScriptStage stage;
        const Uuid audience = stage.write("scripts/audience.lua", kAudience);
        const Entity listening = stage.add("Listening", audience);
        const Entity bare = stage.world.createEntity("Bare");
        const Entity deaf = stage.add("Deaf", stage.write("scripts/deaf.lua", "return {update = function() end}"));
        const Entity off = stage.add("Off", audience);
        const Entity broken =
            stage.add("Broken", stage.write("scripts/broken.lua", "return {start = function() error('broken') end, "
                                                                  "sequenceEvent = function() print('never') end}"));
        // The last is put to sleep by the start of another before its own turn to start
        // has come, and woken by an update after: there, switched on, and not begun.
        stage.add("Sleeper", stage.write("scripts/sleeper.lua", "return {start = function() "
                                                                "cordite.world.find('Late'):setEnabled(false) end}"));
        stage.add("Waker", stage.write("scripts/waker.lua", "return {update = function() "
                                                            "cordite.world.find('Late'):setEnabled(true) end}"));
        const Entity late = stage.add("Late", audience);
        stage.update();
        stage.world.setEnabled(off, false);
        REQUIRE(stage.world.isEnabled(late));
        static_cast<void>(stage.errors());

        std::vector<SequenceEvent> events;
        for (const Entity entity : {bare, deaf, off, broken, late, listening}) {
            events.push_back({.player = entity, .name = "cue"});
        }
        stage.scripts->tellSequenceEvents(stage.context(), events);
        CHECK(stage.said() == Lines{"Listening heard cue\t0"});
    }

    TEST_CASE("an error where an event is told stops that script, and is said with where it was") {
        ScriptStage stage;
        const Entity stagehand =
            stage.add("Stagehand", stage.write("scripts/clumsy.lua", "return {sequenceEvent = function(self, name)\n"
                                                                     "    error('dropped the ' .. name)\nend}"));
        const Entity calm = stage.add("Calm", stage.write("scripts/audience.lua", kAudience));
        stage.update();
        const std::vector<SequenceEvent> events = {{.player = stagehand, .name = "sandbag"},
                                                   {.player = calm, .name = "sandbag"},
                                                   {.player = stagehand, .name = "rope"}};
        stage.scripts->tellSequenceEvents(stage.context(), events);
        // The second event of the one that failed finds it stopped.
        CHECK(stage.said() == Lines{"Calm heard sandbag\t0"});
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("dropped the sandbag") != std::string::npos);
        CHECK(stage.problems()[0].find("clumsy.lua:2") != std::string::npos);
        static_cast<void>(stage.errors());
    }

    TEST_CASE("what a script destroys when it is told an event is gone once the telling is over") {
        ScriptStage stage;
        const Entity director =
            stage.add("Director",
                      stage.write("scripts/director.lua",
                                  "return {sequenceEvent = function(self) cordite.world.find('Prop'):destroy() end}"));
        const Entity prop = stage.world.createEntity("Prop");
        stage.update();
        const std::vector<SequenceEvent> events = {{.player = director, .name = "strike"}};
        stage.scripts->tellSequenceEvents(stage.context(), events);
        CHECK_FALSE(stage.world.isValid(prop));
        CHECK(stage.problems().empty());
    }
}
