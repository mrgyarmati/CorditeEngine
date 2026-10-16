#include "cordite/editor/InputCommands.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "unit/editor/EditorTestSupport.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

using namespace cordite;
using cordite::test::json;
using cordite::test::unwrap;

namespace {

/// The controls of a small game, the input they are worked out of, and the commands.
struct InputFixture {
    Input input;
    InputActions actions;
    CommandRegistry commands;

    explicit InputFixture(std::filesystem::path mapFile = "controls.input.json") {
        actions.setMap(unwrap(parseInputMap(json(R"({
            "format": "cordite.input",
            "version": 1,
            "actions": [{"name": "jump", "bindings": [{"key": "Space"}, {"gamepadButton": "A"}]},
                        {"name": "fire", "bindings": [{"mouseButton": "Left"}]}],
            "axes": [{"name": "moveX", "bindings": [{"gamepadAxis": "LeftX", "deadZone": 0.0}]},
                     {"name": "lookX", "bindings": [{"mouse": "x"}]},
                     {"name": "zoom", "bindings": [{"scroll": "y"}]}]
        })"))));
        REQUIRE(registerInputCommands(commands, input, actions, std::move(mapFile)).has_value());
        frame();
    }

    /// A frame goes by: what was injected since the last is what this one sees.
    void frame() {
        actions.update(input);
        input.beginFrame();
    }

    Json run(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));
        return *result;
    }

    Error fail(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_FALSE(result.has_value());
        return result.error();
    }
};

} // namespace

TEST_SUITE("InputCommands") {
    TEST_CASE("input.map returns the controls and the file they came from") {
        InputFixture fixture(std::filesystem::path("assets") / "input" / "controls.input.json");
        const Json result = fixture.run("input.map");
        CHECK(result["file"] == pathToUtf8(std::filesystem::path("assets") / "input" / "controls.input.json"));
        // In the format of an input file: what is returned can be read as one.
        CHECK(unwrap(parseInputMap(result["map"])) == fixture.actions.map());
        CHECK(result["map"]["actions"][0]["name"] == "jump");
        CHECK(result["map"]["actions"][0]["bindings"][0] == json(R"({"key": "Space"})"));

        // Without a file there is none to name, and without controls an empty map.
        Input input;
        InputActions none;
        CommandRegistry commands;
        REQUIRE(registerInputCommands(commands, input, none, {}).has_value());
        const Json empty = unwrap(commands.execute("input.map", Json::object()));
        CHECK(empty["file"].is_null());
        CHECK(empty["map"] == json(R"({"format": "cordite.input", "version": 1, "actions": [], "axes": []})"));
    }

    TEST_CASE("input.inject presses keys and buttons, and input.state says what the controls did") {
        InputFixture fixture;
        CHECK(fixture.run("input.state") ==
              json(R"({"actions": [{"name": "jump", "down": false, "pressed": false, "released": false},
                                   {"name": "fire", "down": false, "pressed": false, "released": false}],
                       "axes": [{"name": "moveX", "value": 0.0}, {"name": "lookX", "value": 0.0},
                                {"name": "zoom", "value": 0.0}]})"));

        // A key goes down, and stays down until it is let go of.
        CHECK(fixture.run("input.inject", R"({"key": "Space", "down": true})") == Json::object());
        CHECK(fixture.input.isKeyDown(Key::Space));
        // The controls are worked out by the frame, not by the command.
        CHECK(fixture.run("input.state")["actions"][0]["down"] == false);
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][0] ==
              json(R"({"name": "jump", "down": true, "pressed": true, "released": false})"));
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][0] ==
              json(R"({"name": "jump", "down": true, "pressed": false, "released": false})"));
        fixture.run("input.inject", R"({"key": "space", "down": false})");
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][0] ==
              json(R"({"name": "jump", "down": false, "pressed": false, "released": true})"));

        fixture.run("input.inject", R"({"mouseButton": "Left", "down": true})");
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][1]["down"] == true);
        fixture.run("input.inject", R"({"mouseButton": "Left", "down": false})");
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][1]["released"] == true);
    }

    TEST_CASE("input.inject moves the pointer, turns the wheel and sets a gamepad") {
        InputFixture fixture;
        fixture.run("input.inject", R"({"mouseMove": [100, 50]})");
        fixture.frame();
        fixture.run("input.inject", R"({"mouseMove": [112.5, 50]})");
        fixture.run("input.inject", R"({"scroll": [0, -2]})");
        CHECK(fixture.input.mousePosition() == Vec2(112.5f, 50.0f));
        fixture.frame();
        Json state = fixture.run("input.state");
        CHECK(state["axes"][1] == json(R"({"name": "lookX", "value": 12.5})"));
        CHECK(state["axes"][2] == json(R"({"name": "zoom", "value": -2.0})"));

        // A gamepad is as the call says it is: what is not named is at rest.
        fixture.run("input.inject", R"({"gamepad": 1, "buttons": ["A", "DpadUp"], "axes": {"LeftX": -0.5}})");
        CHECK(fixture.input.isGamepadConnected(1));
        CHECK(fixture.input.isGamepadButtonDown(1, GamepadButton::DpadUp));
        fixture.frame();
        state = fixture.run("input.state");
        CHECK(state["actions"][0]["pressed"] == true);
        CHECK(state["axes"][0] == json(R"({"name": "moveX", "value": -0.5})"));
        fixture.run("input.inject", R"({"gamepad": 1})");
        fixture.frame();
        state = fixture.run("input.state");
        CHECK(state["actions"][0]["released"] == true);
        CHECK(state["axes"][0]["value"] == 0.0);
        CHECK(fixture.input.isGamepadConnected(1));
        // Unplugged, it is not there.
        fixture.run("input.inject", R"({"gamepad": 1, "connected": false})");
        CHECK_FALSE(fixture.input.isGamepadConnected(1));

        // Files are dropped onto the window, as a file manager drops them.
        fixture.run("input.inject", R"({"drop": ["C:/Downloads/crate.glb", "rust.png"]})");
        REQUIRE(fixture.input.droppedFiles().size() == 2);
        CHECK(fixture.input.droppedFiles()[0] == pathFromUtf8("C:/Downloads/crate.glb"));
        CHECK(fixture.input.droppedFiles()[1] == pathFromUtf8("rust.png"));
    }

    TEST_CASE("input.inject refuses what no hand can do") {
        InputFixture fixture;
        CHECK(fixture.fail("input.inject").message == "input.inject: one thing is injected at a time: a 'key', a "
                                                      "'mouseButton', a 'mouseMove', a 'scroll', a 'gamepad' or a "
                                                      "'drop'");
        CHECK(fixture.fail("input.inject", R"({"drop": []})").message ==
              "input.inject: 'drop' names from 1 to 1024 files, not 0");
        CHECK(fixture.fail("input.inject", R"({"drop": ["a.png", 3]})").message ==
              "input.inject: 'drop' is a list of the paths of files, and 3 is none");
        CHECK(fixture.fail("input.inject", R"({"drop": [""]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"drop": ["a.png"], "scroll": [0, 1]})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"key": "Space", "mouseButton": "Left", "down": true})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"down": true})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"key": "Spacebar", "down": true})").message ==
              "input.inject: \"Spacebar\" is not the name of a key");
        CHECK(fixture.fail("input.inject", R"({"key": "Space"})").message ==
              "input.inject: 'down' says whether the key goes down or comes up: true or false");
        CHECK(fixture.fail("input.inject", R"({"mouseButton": "Thumb", "down": true})").message ==
              "input.inject: \"Thumb\" is not the name of a mouse button");
        CHECK(fixture.fail("input.inject", R"({"mouseButton": "Left"})").message ==
              "input.inject: 'down' says whether the mouse button goes down or comes up: true or false");
        CHECK(fixture.fail("input.inject", R"({"mouseMove": [1]})").message ==
              "input.inject: 'mouseMove' must be an array of 2 numbers");
        CHECK(fixture.fail("input.inject", R"({"mouseMove": ["a", "b"]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"mouseMove": [1e12, 0]})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("input.inject", R"({"scroll": [0, 0, 0]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"gamepad": 4})").message ==
              "input.inject: 'gamepad' is from 0 to 3, not 4");
        CHECK(fixture.fail("input.inject", R"({"gamepad": -1})").code == ErrorCode::OutOfRange);
        CHECK(fixture.fail("input.inject", R"({"gamepad": 0, "buttons": ["Circle"]})").message ==
              "input.inject: 'buttons' lists the names of gamepad buttons; \"Circle\" is none");
        CHECK(fixture.fail("input.inject", R"({"gamepad": 0, "buttons": [3]})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("input.inject", R"({"gamepad": 0, "axes": {"Twist": 1}})").message ==
              "input.inject: 'axes' has the names of gamepad axes; \"Twist\" is none");
        CHECK(fixture.fail("input.inject", R"({"gamepad": 0, "axes": {"LeftX": 1.5}})").message ==
              "input.inject: the axis \"LeftX\" is pushed from -1 to 1, not 1.5");
        CHECK(fixture.fail("input.inject", R"({"gamepad": 0, "axes": {"LeftX": "far"}})").code ==
              ErrorCode::OutOfRange);
        // What is of the wrong kind altogether is refused before it gets here.
        CHECK_FALSE(fixture.commands.execute("input.inject", json(R"({"key": 32, "down": true})")).has_value());
        CHECK_FALSE(fixture.commands.execute("input.inject", json(R"({"key": "Space", "down": "yes"})")).has_value());
        // Nothing was pressed by any of it.
        fixture.frame();
        CHECK(fixture.run("input.state")["actions"][0]["down"] == false);
        CHECK_FALSE(fixture.input.isGamepadConnected(0));
    }

    TEST_CASE("the input commands are described for whoever reads the catalogue") {
        const InputFixture fixture;
        for (const char* name : {"input.map", "input.state", "input.inject"}) {
            const CommandDesc* command = fixture.commands.find(name);
            REQUIRE(command != nullptr);
            CHECK_FALSE(command->description.empty());
            for (const CommandParameter& parameter : command->parameters) {
                CAPTURE(parameter.name);
                CHECK_FALSE(parameter.description.empty());
                CHECK_FALSE(parameter.required);
            }
        }
    }
}
