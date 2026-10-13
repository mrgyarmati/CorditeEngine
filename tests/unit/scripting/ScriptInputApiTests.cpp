#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/InputMap.h"
#include "cordite/platform/KeyCodes.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"

#include "unit/scripting/ScriptTestSupport.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// Two actions and two axes, bound to a little of everything.
constexpr const char* kControls = R"json({
    "format": "cordite.input",
    "version": 1,
    "actions": [
        {"name": "jump", "bindings": [{"key": "Space"}, {"gamepadButton": "A"}]},
        {"name": "fire", "bindings": [{"mouseButton": "Left"}]}
    ],
    "axes": [
        {"name": "moveX", "bindings": [{"positiveKey": "D", "negativeKey": "A"},
                                       {"gamepadAxis": "LeftX", "deadZone": 0.25}]},
        {"name": "zoom", "bindings": [{"scroll": "y", "scale": 0.1}]}
    ]
})json";

constexpr const char* kAskJump =
    "print(cordite.input.isDown('jump'), cordite.input.wasPressed('jump'), cordite.input.wasReleased('jump'))";

/// A script that says everything it is called with.
constexpr const char* kListener = R"(
    local Listener = {}
    function Listener:start() print("start " .. self.entity:name()) end
    function Listener:input(action, pressed)
        print(self.entity:name() .. " " .. action .. " " .. tostring(pressed) .. " " .. math.type(cordite.time.frame()))
    end
    function Listener:update() print("update " .. self.entity:name()) end
    return Listener
)";

} // namespace

TEST_SUITE("ScriptInputApi") {
    TEST_CASE("a script asks whether a control is held, and whether it was pressed or released in the frame") {
        ScriptStage stage;
        stage.setControls(kControls);
        CHECK(stage.line(kAskJump) == "false\tfalse\tfalse");

        // Every probe is a frame, and a second one in which it is taken away again.
        stage.input.injectKey(Key::Space, true);
        CHECK(stage.line(kAskJump) == "true\ttrue\tfalse");
        CHECK(stage.line(kAskJump) == "true\tfalse\tfalse");
        stage.input.injectKey(Key::Space, false);
        CHECK(stage.line(kAskJump) == "false\tfalse\ttrue");
        CHECK(stage.line(kAskJump) == "false\tfalse\tfalse");

        // What is tapped between two frames was pressed, and released, and is not held.
        stage.input.injectKey(Key::Space, true);
        stage.input.injectKey(Key::Space, false);
        CHECK(stage.line(kAskJump) == "false\ttrue\ttrue");

        // Any of its bindings holds an action; and one action is not another.
        GamepadState pad;
        pad.buttons.set(static_cast<size_t>(GamepadButton::A));
        stage.input.injectGamepad(0, &pad);
        CHECK(stage.line("print(cordite.input.isDown('jump'), cordite.input.isDown('fire'))") == "true\tfalse");
        stage.input.injectGamepad(0, nullptr);
        stage.input.injectMouseButton(MouseButton::Left, true);
        CHECK(stage.line("print(cordite.input.isDown('jump'), cordite.input.isDown('fire'))") == "false\ttrue");
    }

    TEST_CASE("a script asks how far an axis is pushed") {
        ScriptStage stage;
        stage.setControls(kControls);
        CHECK(stage.line("print(cordite.input.axis('moveX'), cordite.input.axis('zoom'))") == "0.0\t0.0");

        stage.input.injectKey(Key::D, true);
        CHECK(stage.line("print(cordite.input.axis('moveX'))") == "1.0");
        stage.input.injectKey(Key::D, false);
        stage.input.injectKey(Key::A, true);
        CHECK(stage.line("print(cordite.input.axis('moveX'))") == "-1.0");
        stage.input.injectKey(Key::A, false);

        // A stick: nothing within its dead zone of a quarter, and from there on up to 1
        // at the end of its way. Half of what is left is five eighths of the whole.
        GamepadState pad;
        pad.axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.125f;
        stage.input.injectGamepad(0, &pad);
        CHECK(stage.line("print(cordite.input.axis('moveX'))") == "0.0");
        pad.axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.625f;
        stage.input.injectGamepad(0, &pad);
        CHECK(stage.line("print(cordite.input.axis('moveX'), cordite.input.axis('moveX') == 0.5)") == "0.5\ttrue");
        stage.input.injectGamepad(0, nullptr);

        // The wheel gives how far it was turned in the frame, times the scale of the binding.
        stage.input.injectScroll(Vec2(0.0f, 3.0f));
        CHECK(stage.line("print(cordite.input.axis('zoom'))") == "0.3");
        CHECK(stage.line("print(cordite.input.axis('zoom'))") == "0.0");
    }

    TEST_CASE("a control that the input map does not have is a mistake, and said with the ones it has") {
        ScriptStage stage;
        stage.setControls(kControls);

        CHECK(stage.refusal("cordite.input.isDown('jmp')") ==
              "bad argument #1 to 'isDown' (the input map has no action called 'jmp'; it has: jump, fire)");
        CHECK(stage.refusal("cordite.input.wasPressed('Jump')") ==
              "bad argument #1 to 'wasPressed' (the input map has no action called 'Jump'; it has: jump, fire)");
        CHECK(stage.refusal("cordite.input.wasReleased('')") ==
              "bad argument #1 to 'wasReleased' (the input map has no action called ''; it has: jump, fire)");
        CHECK(stage.refusal("cordite.input.axis('moveZ')") ==
              "bad argument #1 to 'axis' (the input map has no axis called 'moveZ'; it has: moveX, zoom)");
        // An action is no axis, and an axis no action.
        CHECK(stage.refusal("cordite.input.axis('jump')") ==
              "bad argument #1 to 'axis' (the input map has no axis called 'jump'; it has: moveX, zoom)");
        CHECK(stage.refusal("cordite.input.isDown('moveX')") ==
              "bad argument #1 to 'isDown' (the input map has no action called 'moveX'; it has: jump, fire)");
        CHECK(stage.refusal("cordite.input.isDown()") == "bad argument #1 to 'isDown' (string expected, got no value)");
        CHECK(stage.refusal("cordite.input.axis({})") == "bad argument #1 to 'axis' (string expected, got table)");

        // A map of nothing has nothing to ask about.
        ScriptStage bare;
        CHECK(bare.refusal("cordite.input.isDown('jump')") ==
              "bad argument #1 to 'isDown' (the input map has no action called 'jump'; it has: none)");
        CHECK(bare.refusal("cordite.input.axis('moveX')") ==
              "bad argument #1 to 'axis' (the input map has no axis called 'moveX'; it has: none)");
    }

    TEST_CASE("a world that is given no controls has none to ask about, and tells its scripts of none") {
        ScriptStage stage;
        stage.setControls(kControls);
        stage.played = false;
        stage.input.injectKey(Key::Space, true);

        for (const char* asked : {"cordite.input.isDown('jump')", "cordite.input.wasPressed('jump')",
                                  "cordite.input.wasReleased('jump')", "cordite.input.axis('moveX')"}) {
            CAPTURE(asked);
            CHECK(stage.refusal(asked) == "there are no controls to ask: nothing gives this world any");
        }
        stage.add("Deaf", stage.write("scripts/listener.lua", kListener));
        stage.update();
        CHECK(stage.said() == Lines{"start Deaf", "update Deaf"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a script is told what was pressed and released, after the starts of the frame and before the updates") {
        ScriptStage stage;
        stage.setControls(kControls);
        const Uuid listener = stage.write("scripts/listener.lua", kListener);
        stage.add("First", listener);
        stage.add("Second", listener);
        // One that has no such function is told nothing.
        stage.add("Silent", stage.write("scripts/silent.lua", "return {update = function() end}"));

        // Jump goes down; fire is tapped. In the order of the map, and for one entity
        // after the other.
        stage.input.injectMouseButton(MouseButton::Left, true);
        stage.input.injectMouseButton(MouseButton::Left, false);
        stage.input.injectKey(Key::Space, true);
        ScriptStatistics statistics = stage.update();
        CHECK(stage.said() == Lines{"start First", "start Second", "First jump true integer", "First fire true integer",
                                    "First fire false integer", "Second jump true integer", "Second fire true integer",
                                    "Second fire false integer", "update First", "update Second"});
        // Two starts, twice three events, three updates.
        CHECK(statistics.calls == 11);

        // What is held is not pressed again.
        statistics = stage.update();
        CHECK(stage.said() == Lines{"update First", "update Second"});
        CHECK(statistics.calls == 3);

        stage.input.injectKey(Key::Space, false);
        stage.update();
        CHECK(stage.said() ==
              Lines{"First jump false integer", "Second jump false integer", "update First", "update Second"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("only what runs is told: not an entity that is switched off, nor a script that has stopped") {
        ScriptStage stage;
        stage.setControls(kControls);
        const Uuid listener = stage.write("scripts/listener.lua", kListener);
        const Entity off = stage.add("Off", listener);
        stage.world.setEnabled(off, false);
        // One that an event stops hears nothing after it, and is not updated either.
        stage.add("Fragile", stage.write("scripts/fragile.lua", R"(
            local Fragile = {}
            function Fragile:input(action, pressed)
                print("fragile " .. action)
                if action == "jump" then error("it broke") end
            end
            function Fragile:update() print("update Fragile") end
            return Fragile
        )"));
        // One that switches its entity off when it hears something hears no more of that
        // frame - and nothing at all until something switches it on again.
        const Entity shy = stage.add("Shy", stage.write("scripts/shy.lua", R"(
            local Shy = {}
            function Shy:input(action, pressed)
                print("shy " .. action .. " " .. tostring(pressed))
                self.entity:setEnabled(false)
            end
            function Shy:update() print("update Shy") end
            return Shy
        )"));
        stage.add("Last", listener);

        stage.input.injectKey(Key::Space, true);
        stage.input.injectMouseButton(MouseButton::Left, true);
        stage.update();
        CHECK(stage.said() == Lines{"start Last", "fragile jump", "shy jump true", "Last jump true integer",
                                    "Last fire true integer", "update Last"});
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("'Fragile' (scripts/fragile.lua): scripts/fragile.lua:5: it broke") == 0);
        CHECK_FALSE(stage.world.isEnabled(shy));
        static_cast<void>(stage.errors());

        // An entity that was off when something was pressed has missed it.
        stage.world.setEnabled(off, true);
        stage.world.setEnabled(shy, true);
        stage.update();
        CHECK(stage.said() == Lines{"start Off", "update Off", "update Shy", "update Last"});
    }

    TEST_CASE("an entity that is switched on in the middle of a frame starts with the next, and hears nothing before") {
        ScriptStage stage;
        stage.setControls(kControls);
        stage.add("Waker", stage.write("scripts/waker.lua", R"(
            local Waker = {}
            function Waker:input(action, pressed) cordite.world.find("Late"):setEnabled(true) end
            return Waker
        )"));
        const Entity late = stage.add("Late", stage.write("scripts/listener.lua", kListener));
        stage.world.setEnabled(late, false);
        stage.update();
        CHECK(stage.said().empty());

        // The waker hears the key and switches the other on - whose turn to hear comes
        // after, in the same walk. It has not started, and nothing is called before that.
        stage.input.injectKey(Key::Space, true);
        stage.update();
        CHECK(stage.world.isEnabled(late));
        CHECK(stage.said().empty());
        stage.update();
        CHECK(stage.said() == Lines{"start Late", "update Late"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a script that was made and has not started hears nothing either") {
        ScriptStage stage;
        stage.setControls(kControls);
        // The first puts the third to sleep as it starts - after the third was given its
        // instance, before its turn to start has come. The second wakes it when a key
        // goes down, which is in the same frame.
        stage.add("Sleeper", stage.write("scripts/sleeper.lua", R"(
            local Sleeper = {}
            function Sleeper:start() cordite.world.find("Late"):setEnabled(false) end
            return Sleeper
        )"));
        stage.add("Waker", stage.write("scripts/waker.lua", R"(
            local Waker = {}
            function Waker:input(action, pressed) cordite.world.find("Late"):setEnabled(true) end
            return Waker
        )"));
        const Entity late = stage.add("Late", stage.write("scripts/listener.lua", kListener));

        stage.input.injectKey(Key::Space, true);
        stage.update();
        CHECK(stage.world.isEnabled(late));
        CHECK(stage.said().empty());
        // Nothing is pressed in the next frame: it starts, and is updated.
        stage.update();
        CHECK(stage.said() == Lines{"start Late", "update Late"});
        CHECK(stage.problems().empty());
    }

    TEST_CASE("what a script calls `input` has to be a function") {
        ScriptStage stage;
        stage.add("Wrong", stage.write("scripts/wrong.lua", "return {input = 'jump'}"));
        stage.update();
        REQUIRE(stage.problems().size() == 2);
        CHECK(stage.problems()[0] == "scripts/wrong.lua: its 'input' is a string, not a function");
        static_cast<void>(stage.errors());
    }

    TEST_CASE("the functions of the controls are said in words") {
        const ScriptStage stage;
        Lines names;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            if (function.name.starts_with("cordite.input.")) {
                names.push_back(function.name);
                CHECK(function.signature.starts_with(function.name + "("));
            }
        }
        CHECK(names == Lines{"cordite.input.isDown", "cordite.input.wasPressed", "cordite.input.wasReleased",
                             "cordite.input.axis"});
    }
}
