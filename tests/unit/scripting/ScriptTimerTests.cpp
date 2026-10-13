#include "cordite/core/Json.h"
#include "cordite/core/Uuid.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

// A frame of the stage is half a second (kScriptFrame): the clock of the scripts reads
// 0.5 in the first update, 1.0 in the second.

namespace {

using Lines = std::vector<std::string>;

/// The script of an entity "Timer" that runs `start` as its start and says when it is
/// updated.
Entity addTimed(ScriptStage& stage, const std::string& start, const char* name = "Timer") {
    const std::string path = std::string("scripts/") + name + ".lua";
    return stage.add(name, stage.write(path, "local Timed = {}\n"
                                             "function Timed:start()\n" +
                                                 start +
                                                 "\nend\n"
                                                 "function Timed:ring() print('ring ' .. self.entity:name()) end\n"
                                                 "function Timed:update() print('update') end\n"
                                                 "return Timed"));
}

} // namespace

TEST_SUITE("ScriptTimers") {
    TEST_CASE("a function is called once, when its time has come, before the update of its script") {
        ScriptStage stage;
        addTimed(stage, R"(
            cordite.time.after(1.0, function(instance) print("one", instance == self, cordite.time.seconds()) end)
            cordite.time.after(0.0, function() print("now") end)
            -- A function of the script is given as it is: it is called with the table of the script.
            cordite.time.after(0.4, self.ring)
            print(math.type(cordite.time.after(60, self.ring)))
        )");

        // No time at all has passed by the walk that follows the starts of the same frame.
        ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"integer", "now", "update"});
        // The start, one timer and the update; three wait on.
        CHECK(statistics.calls == 3);
        CHECK(statistics.timers == 3);
        stage.update();
        CHECK(stage.said() == Lines{"ring Timer", "update"});
        statistics = stage.update();
        CHECK(stage.said() == Lines{"one\ttrue\t1.5", "update"});
        CHECK(statistics.timers == 1);
        // Once is once.
        stage.update();
        stage.update();
        CHECK(stage.said() == Lines{"update", "update"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("timers are called in the order they were made, and what one makes waits for the next update") {
        ScriptStage stage;
        addTimed(stage, R"(
            cordite.time.after(0.2, function() print("first") end)
            cordite.time.after(0.1, function()
                print("second")
                -- Due at once, and still not called in the walk that made it.
                cordite.time.after(0.0, function() print("made by the second") end)
            end)
            cordite.time.after(0.2, function() print("third") end)
        )",
                 "Maker");
        addTimed(stage, "cordite.time.after(0.1, function() print('of another entity') end)", "Other");
        stage.update();
        CHECK(stage.said() == Lines{"update", "update"});
        stage.update();
        CHECK(stage.said() == Lines{"first", "second", "third", "of another entity", "update", "update"});
        stage.update();
        CHECK(stage.said() == Lines{"made by the second", "update", "update"});
    }

    TEST_CASE("a function is called again and again, and keeps time") {
        ScriptStage stage;
        addTimed(stage, R"(
            self.calls = 0
            cordite.time.every(1.0, function()
                self.calls = self.calls + 1
                print("tick", self.calls, cordite.time.seconds())
            end)
        )");
        // Made when the clock read 0.5: due at 1.5, 2.5, 3.5.
        Lines said;
        for (int frame = 0; frame < 7; ++frame) {
            stage.update();
            for (const std::string& line : stage.said()) {
                if (line != "update") {
                    said.push_back(line);
                }
            }
        }
        CHECK(said == Lines{"tick\t1\t1.5", "tick\t2\t2.5", "tick\t3\t3.5"});

        // A frame of three seconds and a bit: called once, not three times - and due
        // again when it would have been, at 7.5, not a second after it was called.
        stage.update(3.25);
        CHECK(stage.said() == Lines{"tick\t4\t6.75", "update"});
        stage.update(0.5);
        CHECK(stage.said() == Lines{"update"});
        stage.update(0.25);
        CHECK(stage.said() == Lines{"tick\t5\t7.5", "update"});

        CHECK(stage.refusal("cordite.time.every(0, print)") ==
              "bad argument #1 to 'every' (the time between two calls: a number of seconds above 0)");
        CHECK(stage.refusal("cordite.time.every(-1, print)") ==
              "bad argument #1 to 'every' (a time in seconds: a number from 0 up)");
        CHECK(stage.refusal("cordite.time.every(1)") == "bad argument #2 to 'every' (function expected, got no value)");
        CHECK(stage.refusal("cordite.time.after(0 / 0, print)") ==
              "bad argument #1 to 'after' (a time in seconds: a number from 0 up)");
        CHECK(stage.refusal("cordite.time.after(1 / 0, print)") ==
              "bad argument #1 to 'after' (a time in seconds: a number from 0 up)");
        CHECK(stage.refusal("cordite.time.after('soon', print)") ==
              "bad argument #1 to 'after' (number expected, got string)");
        CHECK(stage.refusal("cordite.time.after(1, 'ring')") ==
              "bad argument #2 to 'after' (function expected, got string)");
    }

    TEST_CASE("a timer that is cancelled is not called") {
        ScriptStage stage;
        addTimed(stage, R"(
            local never = cordite.time.after(0.6, function() print("never") end)
            cordite.time.cancel(never)
            -- One that stops itself, the third time.
            local count = 0
            local ticking
            ticking = cordite.time.every(0.5, function()
                count = count + 1
                print("tick " .. count)
                if count == 3 then cordite.time.cancel(ticking) end
            end)
            -- Nothing, a timer that was called and a number that never was are nothing to cancel.
            local once = cordite.time.after(0.0, function() print("once") end)
            cordite.time.after(0.6, function()
                cordite.time.cancel(once)
                cordite.time.cancel(nil)
                cordite.time.cancel()
                cordite.time.cancel(123456)
                cordite.time.cancel(-1)
                print("cancelled nothing")
            end)
        )");
        Lines said;
        for (int frame = 0; frame < 8; ++frame) {
            const ScriptStatistics statistics = stage.update();
            for (const std::string& line : stage.said()) {
                if (line != "update") {
                    said.push_back(line);
                }
            }
            if (frame == 7) {
                CHECK(statistics.timers == 0);
            }
        }
        // In the third frame two are due: the one that was made first is called first.
        CHECK(said == Lines{"once", "tick 1", "tick 2", "cancelled nothing", "tick 3"});
        CHECK(stage.refusal("cordite.time.cancel('it')") ==
              "bad argument #1 to 'cancel' (number expected, got string)");
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a task runs at once as far as its first wait, and goes on when the time has passed") {
        ScriptStage stage;
        addTimed(stage, R"(
            local task = cordite.time.run(function(first, second)
                print("begun", first, second)
                cordite.time.wait(1.0)
                print("a second later", cordite.time.seconds())
                cordite.time.wait()
                print("the frame after")
                cordite.time.wait(0)
                print("and the one after that")
            end, "a", 2)
            print("started", math.type(task))
        )");
        ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"begun\ta\t2", "started\tinteger", "update"});
        CHECK(statistics.timers == 1);
        stage.update();
        CHECK(stage.said() == Lines{"update"});
        stage.update();
        CHECK(stage.said() == Lines{"a second later\t1.5", "update"});
        stage.update();
        CHECK(stage.said() == Lines{"the frame after", "update"});
        statistics = stage.update();
        CHECK(stage.said() == Lines{"and the one after that", "update"});
        // At its end it is gone.
        CHECK(statistics.timers == 0);
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a task waits in loops and in the functions it calls, and is stopped by cancelling it") {
        ScriptStage stage;
        addTimed(stage, R"(
            local function countdown(from)
                for count = from, 1, -1 do
                    print(count)
                    cordite.time.wait(0.5)
                end
            end
            cordite.time.run(function()
                countdown(2)
                -- A protected call does not keep a task from waiting inside it.
                print(pcall(function()
                    cordite.time.wait(0.5)
                    error("caught", 0)
                end))
                print("lift-off")
            end)
            self.forever = cordite.time.run(function()
                while true do
                    cordite.time.wait(0.5)
                    print("again")
                end
            end)
            cordite.time.after(1.2, function() cordite.time.cancel(self.forever) end)
        )");
        Lines said;
        for (int frame = 0; frame < 6; ++frame) {
            stage.update();
            for (const std::string& line : stage.said()) {
                if (line != "update") {
                    said.push_back(line);
                }
            }
        }
        // The second task is cancelled by the timer of the fourth frame - which was made
        // after it, and so is called after it has gone on once more.
        CHECK(said == Lines{"2", "1", "again", "again", "false\tcaught", "lift-off", "again"});
        CHECK(stage.scripts->update(stage.context()).timers == 0);
        CHECK(stage.problems().empty());
    }

    TEST_CASE("wait is for a task, and an error in a task is an error of its script") {
        ScriptStage stage;
        CHECK(stage.refusal("cordite.time.wait(1)") ==
              "wait() is for a task: a function that was started with cordite.time.run()");
        // Nor in a coroutine the script resumes itself: nobody would resume it in time.
        CHECK(stage.refusal("local own = coroutine.wrap(function() cordite.time.wait(1) end)\nown()")
                  .ends_with(".lua:3: wait() is for a task: a function that was started with cordite.time.run()"));
        // Not even in a task: it is the task that waits, not a coroutine inside it.
        CHECK(stage
                  .refusal("cordite.time.run(function()\n"
                           "    local own = coroutine.wrap(function() cordite.time.wait(1) end)\n"
                           "    own()\n"
                           "end)")
                  .ends_with(".lua:4: wait() is for a task: a function that was started with cordite.time.run()"));
        CHECK(stage.refusal("cordite.time.run(function() cordite.time.wait(-1) end)") ==
              "bad argument #1 to 'wait' (a time in seconds: a number from 0 up)");
        CHECK(stage.refusal("cordite.time.run(5)") == "bad argument #1 to 'run' (function expected, got number)");

        // Before its first wait a task is part of the call that started it: what goes
        // wrong there is said with where the task was, and with who started it.
        const Entity early =
            addTimed(stage, "cordite.time.run(function() error('too early') end)\nprint('never')", "Early");
        stage.update();
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("'Early' (scripts/Early.lua): scripts/Early.lua:3: too early") == 0);
        CHECK(stage.problems()[0].find("in function 'cordite.time.run'") != std::string::npos);
        CHECK(stage.said().empty());
        stage.world.destroyEntity(early);
        stage.update();
        static_cast<void>(stage.errors());

        // Later it stops the script as any error does: no update follows, and nothing of
        // the task or of its other timers.
        addTimed(stage, R"(
            cordite.time.run(function()
                cordite.time.wait(0.5)
                local nothing = nil
                print(nothing.name)
            end)
            cordite.time.every(0.5, function() print("tick") end)
        )",
                 "Late");
        stage.update();
        CHECK(stage.said() == Lines{"update"});
        stage.update();
        CHECK(stage.said().empty());
        REQUIRE(stage.problems().size() == 1);
        const std::string late = stage.problems()[0];
        CAPTURE(late);
        CHECK(late.find("'Late' (scripts/Late.lua): scripts/Late.lua:7: attempt to index a nil value "
                        "(local 'nothing')") == 0);
        // With where the task was, which nothing else could say.
        CHECK(late.find("stack traceback:") != std::string::npos);
        stage.update();
        CHECK(stage.said().empty());
        static_cast<void>(stage.errors());
    }

    TEST_CASE("a task that does not end is stopped, as any script is") {
        ScriptSystemDesc desc;
        desc.runtime.instructionLimit = 100000;
        ScriptStage stage(desc);
        addTimed(stage, R"(
            cordite.time.run(function()
                cordite.time.wait(0.5)
                while true do end
            end)
        )");
        stage.update();
        CHECK(stage.problems().empty());
        stage.update();
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("ran for more than 100000 instructions without coming to an end") !=
              std::string::npos);
        static_cast<void>(stage.errors());
        static_cast<void>(stage.said());
        // The others go on.
        addTimed(stage, "cordite.time.after(0, function() print('still here') end)", "Other");
        stage.update();
        CHECK(stage.said() == Lines{"still here", "update"});
    }

    TEST_CASE("timers belong to the script that made them: they wait while it does not run, and go when it goes") {
        ScriptStage stage;
        const Entity resting = addTimed(stage, "cordite.time.every(0.5, function() print('resting') end)", "Resting");
        const Entity doomed = addTimed(stage, "cordite.time.every(0.5, function() print('doomed') end)", "Doomed");
        const Entity changed = addTimed(stage, "cordite.time.every(0.5, function() print('changed') end)", "Changed");
        const Entity broken = addTimed(stage, R"(
            cordite.time.every(0.5, function() print("broken") end)
            cordite.time.after(0.5, function() error("broke") end)
        )",
                                       "Broken");
        stage.update();
        static_cast<void>(stage.said());
        const auto ticks = [&] {
            stage.update();
            Lines lines;
            for (const std::string& line : stage.said()) {
                if (line != "update" && !line.starts_with("start")) {
                    lines.push_back(line);
                }
            }
            return lines;
        };
        // All four tick; the last then breaks, and its other timer with it.
        CHECK(ticks() == Lines{"resting", "doomed", "changed", "broken"});
        static_cast<void>(stage.errors());
        CHECK(stage.problems().size() == 1);

        // Switched off, a script's timers wait - and are called once when it is back,
        // not once for every time they would have been.
        stage.world.setEnabled(resting, false);
        // Destroyed, they are gone with it.
        stage.world.destroyEntity(doomed);
        // Given other properties, the entity has another instance of its script: one
        // that made no timer yet, and makes its own when it starts.
        stage.world.tryGet<ScriptComponent>(changed)->properties = Json::object();
        CHECK(ticks() == Lines{"changed"});
        stage.world.tryGet<ScriptComponent>(changed)->script = Uuid();
        CHECK(ticks().empty());
        CHECK(ticks().empty());
        stage.world.setEnabled(resting, true);
        CHECK(ticks() == Lines{"resting"});
        CHECK(ticks() == Lines{"resting"});
        static_cast<void>(broken);

        // Cleared, the system has none, and has let go of what they held - a megabyte of
        // text, here - until the scripts of the world start again and make their own:
        // one, and the two of the one that breaks.
        CHECK(stage.scripts->tellContacts(stage.context(), {}).timers == 2);
        addTimed(stage, R"(
            local heavy = string.rep("x", 1000000)
            cordite.time.after(60, function() print(#heavy) end)
        )",
                 "Heavy");
        stage.update();
        stage.scripts->runtime().collectGarbage();
        CHECK(stage.scripts->runtime().memoryBytes() > 1000000);
        stage.scripts->clear(stage.context());
        CHECK(stage.scripts->tellContacts(stage.context(), {}).timers == 0);
        stage.scripts->runtime().collectGarbage();
        CHECK(stage.scripts->runtime().memoryBytes() < 500000);
        CHECK(stage.scripts->update(stage.context(0.0)).timers == 4);
    }

    TEST_CASE("what a start that failed had asked for is not done: the start that is tried again asks again") {
        ScriptStage stage;
        const std::string broken = "local Late = {}\n"
                                   "function Late:start()\n"
                                   "    cordite.time.every(0.5, function() print('tick') end)\n"
                                   "    cordite.time.run(function() while true do cordite.time.wait(0.5) "
                                   "print('tock') end end)\n"
                                   "    error('not yet')\n"
                                   "end\n"
                                   "return Late";
        stage.add("Late", stage.write("scripts/late.lua", broken));
        // The timer and the task were made, and are let go of with the start that failed.
        CHECK(stage.update().timers == 0);
        stage.update();
        CHECK(stage.said().empty());
        REQUIRE(stage.problems().size() == 1);
        static_cast<void>(stage.errors());

        // The script is put right; its start is tried again, and what it asks for is
        // there once.
        std::string mended = broken;
        mended.replace(mended.find("    error('not yet')\n"), std::string("    error('not yet')\n").size(), "");
        stage.write("scripts/late.lua", mended);
        CHECK(stage.update().timers == 2);
        stage.update();
        CHECK(stage.said() == Lines{"tick", "tock"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a timer is made where a script of an entity runs, and a script has only so many") {
        ScriptStage stage;
        // Where a file is loaded no entity is being called: whose would the timer be?
        stage.add("Loose", stage.write("scripts/loose.lua", "cordite.time.after(1, print)\nreturn {}"));
        stage.add("Looser", stage.write("scripts/looser.lua", "cordite.time.run(print)\nreturn {}"));
        stage.update();
        REQUIRE(stage.problems().size() == 4);
        CHECK(stage.problems()[0].find("scripts/loose.lua:1: a timer belongs to the script of an entity: make it in "
                                       "a function the engine calls") != std::string::npos);
        CHECK(stage.problems()[1].find("scripts/looser.lua:1: a task belongs to the script of an entity: start it in "
                                       "a function the engine calls") != std::string::npos);
        static_cast<void>(stage.errors());
        stage.world.destroyEntity(stage.world.findByName("Loose"));
        stage.world.destroyEntity(stage.world.findByName("Looser"));
        stage.update();

        // What it makes in what it is told belongs to it too: in a timer, in a task.
        addTimed(stage, R"(
            cordite.time.after(0, function()
                cordite.time.after(0, function() print("from a timer") end)
            end)
            cordite.time.run(function()
                cordite.time.wait()
                cordite.time.after(0, function() print("from a task") end)
            end)
        )",
                 "Nested");
        stage.update();
        stage.update();
        stage.update();
        Lines said = stage.said();
        std::erase(said, "update");
        CHECK(said == Lines{"from a timer", "from a task"});

        CHECK(stage.scripts->tellContacts(stage.context(), {}).timers == 0);
    }

    TEST_CASE("a script has only so many timers and tasks waiting") {
        ScriptStage stage;
        // A thousand and twenty-four, and the next is one too many - also as a task.
        CHECK(stage.refusal("for count = 1, 1024 do cordite.time.after(60, print) end\n"
                            "cordite.time.after(60, print)") ==
              "the script has 1024 timers and tasks waiting, which is as many as it may: does it make one in every "
              "frame?");
        CHECK(stage.refusal("for count = 1, 1024 do cordite.time.every(60, print) end\n"
                            "cordite.time.run(print)") ==
              "the script has 1024 timers and tasks waiting, which is as many as it may: does it start one in every "
              "frame?");
        // Under the limit all is well, and what a script lets go of makes room again.
        CHECK(stage.line("local made = {}\n"
                         "for count = 1, 1024 do made[count] = cordite.time.after(60, print) end\n"
                         "for count = 1, 1024 do cordite.time.cancel(made[count]) end\n"
                         "for count = 1, 1024 do made[count] = cordite.time.after(60, print) end\n"
                         "print(#made)") == "1024");
        // What a script had waiting is let go of with it.
        CHECK(stage.scripts->update(stage.context(0.0)).timers == 0);
    }
}
