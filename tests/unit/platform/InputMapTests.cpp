#include "cordite/platform/InputMap.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "support/TestSupport.h"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A map of the kind a game has: every kind of binding once at least.
constexpr const char* kControls = R"({
    "format": "cordite.input",
    "version": 1,
    "actions": [
        {"name": "jump", "bindings": [{"key": "Space"}, {"gamepadButton": "A"}]},
        {"name": "fire", "bindings": [{"mouseButton": "Left"}, {"gamepadAxis": "RightTrigger", "threshold": 0.25}]},
        {"name": "back", "bindings": [{"gamepadAxis": "LeftY", "threshold": -0.5}]},
        {"name": "idle", "bindings": []}
    ],
    "axes": [
        {"name": "moveX", "bindings": [{"positiveKey": "D", "negativeKey": "A"},
                                       {"positiveButton": "DpadRight", "negativeButton": "DpadLeft"},
                                       {"gamepadAxis": "LeftX", "deadZone": 0.2}]},
        {"name": "moveY", "bindings": [{"positiveKey": "W"}, {"gamepadAxis": "LeftY", "scale": -1}]},
        {"name": "lookX", "bindings": [{"mouse": "x", "scale": 0.1}, {"gamepadAxis": "RightX", "deadZone": 0, "scale": 2.5}]},
        {"name": "zoom", "bindings": [{"scroll": "y"}, {"negativeButton": "LeftBumper"}]}
    ]
})";

InputMap controls() {
    return unwrap(parseInputMap(unwrap(parseJson(kControls))));
}

/// What a map made of `text` is refused with.
std::string refusal(const std::string& text) {
    const Result<InputMap> map = parseInputMap(unwrap(parseJson(text)));
    REQUIRE_FALSE(map.has_value());
    CHECK(map.error().code == ErrorCode::ParseError);
    return map.error().message;
}

/// A map with these actions and nothing else.
std::string withActions(const std::string& actions) {
    return R"({"format": "cordite.input", "version": 1, "actions": )" + actions + "}";
}

std::string withAxes(const std::string& axes) {
    return R"({"format": "cordite.input", "version": 1, "axes": )" + axes + "}";
}

/// A gamepad with a stick pushed.
GamepadState pad(GamepadAxis axis, float value) {
    GamepadState state;
    state.axes[static_cast<size_t>(axis)] = value;
    return state;
}

} // namespace

TEST_SUITE("InputMap") {
    TEST_CASE("a map names actions and axes, and what moves them") {
        const InputMap map = controls();
        REQUIRE(map.actions.size() == 4);
        CHECK(map.actions[0].name == "jump");
        CHECK(map.actions[0].bindings ==
              std::vector<ActionBinding>{ActionBinding(Key::Space), ActionBinding(GamepadButton::A)});
        CHECK(map.actions[1].bindings ==
              std::vector<ActionBinding>{
                  ActionBinding(MouseButton::Left),
                  ActionBinding(AxisThreshold{.axis = GamepadAxis::RightTrigger, .threshold = 0.25f})});
        CHECK(map.actions[2].bindings ==
              std::vector<ActionBinding>{ActionBinding(AxisThreshold{.axis = GamepadAxis::LeftY, .threshold = -0.5f})});
        CHECK(map.actions[3].bindings.empty());

        REQUIRE(map.axes.size() == 4);
        CHECK(map.axes[0].name == "moveX");
        REQUIRE(map.axes[0].bindings.size() == 3);
        CHECK(map.axes[0].bindings[0] == AxisBinding{.source = KeyPair{.positive = Key::D, .negative = Key::A}});
        CHECK(map.axes[0].bindings[1] == AxisBinding{.source = ButtonPair{.hasPositive = true,
                                                                          .positive = GamepadButton::DpadRight,
                                                                          .hasNegative = true,
                                                                          .negative = GamepadButton::DpadLeft}});
        CHECK(map.axes[0].bindings[2] ==
              AxisBinding{.source = StickAxis{.axis = GamepadAxis::LeftX, .deadZone = 0.2f}});
        // A side that is left out is no key, and what a binding does not say is as given.
        CHECK(map.axes[1].bindings[0] == AxisBinding{.source = KeyPair{.positive = Key::W}});
        CHECK(
            map.axes[1].bindings[1] ==
            AxisBinding{.source = StickAxis{.axis = GamepadAxis::LeftY, .deadZone = kDefaultDeadZone}, .scale = -1.0f});
        CHECK(map.axes[2].bindings[0] == AxisBinding{.source = MouseMotion{.axis = PointerAxis::X}, .scale = 0.1f});
        CHECK(map.axes[3].bindings[0] == AxisBinding{.source = MouseScroll{.axis = PointerAxis::Y}});
        CHECK(map.axes[3].bindings[1] ==
              AxisBinding{.source = ButtonPair{.hasNegative = true, .negative = GamepadButton::LeftBumper}});

        // Names of keys are taken in any case, and a map may be empty.
        const InputMap lowered = unwrap(parseInputMap(unwrap(
            parseJson(withActions(R"([{"name": "jump", "bindings": [{"key": "space"}, {"gamepadButton": "a"}]}])")))));
        CHECK(lowered.actions[0].bindings == map.actions[0].bindings);
        CHECK(unwrap(parseInputMap(unwrap(parseJson(R"({"format": "cordite.input", "version": 1})")))) == InputMap{});
        // An action and an axis may have one name.
        CHECK(parseInputMap(unwrap(parseJson(R"({"format": "cordite.input", "version": 1,
                                                "actions": [{"name": "throttle", "bindings": []}],
                                                "axes": [{"name": "throttle", "bindings": []}]})")))
                  .has_value());
    }

    TEST_CASE("a map that is written is the file it was read from") {
        const InputMap map = controls();
        const Json written = serializeInputMap(map);
        // Everything is written out: also what the file left to be as given.
        CHECK(written == unwrap(parseJson(R"({
            "format": "cordite.input",
            "version": 1,
            "actions": [
                {"name": "jump", "bindings": [{"key": "Space"}, {"gamepadButton": "A"}]},
                {"name": "fire", "bindings": [{"mouseButton": "Left"}, {"gamepadAxis": "RightTrigger", "threshold": 0.25}]},
                {"name": "back", "bindings": [{"gamepadAxis": "LeftY", "threshold": -0.5}]},
                {"name": "idle", "bindings": []}
            ],
            "axes": [
                {"name": "moveX", "bindings": [{"positiveKey": "D", "negativeKey": "A", "scale": 1.0},
                                               {"positiveButton": "DpadRight", "negativeButton": "DpadLeft", "scale": 1.0},
                                               {"gamepadAxis": "LeftX", "deadZone": 0.2, "scale": 1.0}]},
                {"name": "moveY", "bindings": [{"positiveKey": "W", "scale": 1.0},
                                               {"gamepadAxis": "LeftY", "deadZone": 0.15, "scale": -1.0}]},
                {"name": "lookX", "bindings": [{"mouse": "x", "scale": 0.1},
                                               {"gamepadAxis": "RightX", "deadZone": 0.0, "scale": 2.5}]},
                {"name": "zoom", "bindings": [{"scroll": "y", "scale": 1.0}, {"negativeButton": "LeftBumper", "scale": 1.0}]}
            ]
        })")));
        CHECK(unwrap(parseInputMap(written)) == map);
        CHECK(dumpJson(serializeInputMap(unwrap(parseInputMap(written)))) == dumpJson(written));

        // To a file and back.
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / "controls.input.json";
        REQUIRE(saveInputMap(file, map).has_value());
        CHECK(unwrap(loadInputMap(file)) == map);
        // A file that is not there, and one that is no map, are named.
        const Result<InputMap> missing = loadInputMap(directory.path() / "none.input.json");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);
        REQUIRE(writeTextFile(file, R"({"format": "cordite.input", "version": 1, "actions": 5})").has_value());
        const Result<InputMap> broken = loadInputMap(file);
        REQUIRE_FALSE(broken.has_value());
        CHECK(broken.error().code == ErrorCode::ParseError);
        CHECK(broken.error().message == pathToUtf8(file) + ": 'actions' is a list, not number");
    }

    TEST_CASE("a map that says what cannot be is refused, and told why") {
        CHECK(refusal("[]") == "a map of controls must be a JSON object, found array");
        CHECK(refusal(R"({"version": 1})") == "not a Cordite map of controls: 'format' must be \"cordite.input\"");
        CHECK(refusal(R"({"format": "cordite.scene", "version": 1})") ==
              "not a Cordite map of controls: 'format' must be \"cordite.input\"");
        CHECK(refusal(R"({"format": "cordite.input"})") == "the map of controls has no 'version'");
        // A map of a version that this engine does not read is that, not a mistake in the file.
        const Result<InputMap> newer = parseInputMap(unwrap(parseJson(R"({"format": "cordite.input", "version": 2})")));
        REQUIRE_FALSE(newer.has_value());
        CHECK(newer.error().code == ErrorCode::VersionMismatch);
        CHECK(newer.error().message ==
              "the map of controls has format version 2, but this engine reads versions 1 to 1");
        CHECK(refusal(R"({"format": "cordite.input", "version": 1, "buttons": []})") ==
              "a map of controls: unknown member 'buttons'");
        CHECK(refusal(withActions("{}")) == "'actions' is a list, not object");
        CHECK(refusal(withActions("[5]")) == "an entry of 'actions' is an object, not number");
        CHECK(refusal(withActions(R"([{"bindings": []}])")) == "missing required field 'name'");
        CHECK(refusal(withActions(R"([{"name": "", "bindings": []}])")) == "an action of 'actions' has no name");
        CHECK(refusal(withActions(R"([{"name": "jump"}])")) == "action 'jump' has a list of 'bindings'");
        CHECK(refusal(withActions(R"([{"name": "jump", "bindings": {}}])")) ==
              "action 'jump' has a list of 'bindings'");
        CHECK(refusal(withActions(R"([{"name": "jump", "bindings": [], "hold": true}])")) ==
              "action 'jump': unknown member 'hold'");
        CHECK(refusal(withActions(R"([{"name": "jump", "bindings": []}, {"name": "jump", "bindings": []}])")) ==
              "action 'jump' is there twice");

        // Bindings of actions.
        const auto action = [](const std::string& binding) {
            return refusal(withActions(R"([{"name": "jump", "bindings": [{"key": "Space"}, )" + binding + "]}]"));
        };
        CHECK(action("5") == "action 'jump', binding 2 is an object, not number");
        CHECK(action("{}") == "action 'jump', binding 2 names one control: a 'key', a 'mouseButton', a "
                              "'gamepadButton' or a 'gamepadAxis'");
        CHECK(action(R"({"key": "Space", "gamepadButton": "A"})") ==
              "action 'jump', binding 2 names one control: a 'key', a 'mouseButton', a 'gamepadButton' or a "
              "'gamepadAxis'");
        CHECK(action(R"({"key": "Spacebar"})") == "action 'jump', binding 2: \"Spacebar\" is not the name of a key");
        CHECK(action(R"({"key": 32})") == "action 'jump', binding 2: 'key' is the name of a key, not number");
        CHECK(action(R"({"key": "Unknown"})").find("is not the name of") != std::string::npos);
        CHECK(action(R"({"mouseButton": "Thumb"})").find("is not the name of") != std::string::npos);
        CHECK(action(R"({"gamepadButton": "Circle"})").find("is not the name of") != std::string::npos);
        CHECK(action(R"({"gamepadAxis": "Twist"})").find("is not the name of") != std::string::npos);
        CHECK(action(R"({"key": "Space", "modifier": "Shift"})") ==
              "action 'jump', binding 2: unknown member 'modifier'");
        CHECK(action(R"({"key": "Space", "threshold": 0.5})") ==
              "action 'jump', binding 2: only a 'gamepadAxis' has a 'threshold'");
        CHECK(action(R"({"gamepadAxis": "LeftX", "threshold": 0})") ==
              "action 'jump', binding 2: a 'threshold' of 0 is passed by a stick at rest; it is above 0, or below");
        CHECK(action(R"({"gamepadAxis": "LeftX", "threshold": "far"})") ==
              "action 'jump', binding 2: 'threshold' is a number, not string");
        // A number that a file can hold and a control cannot: too large for the numbers the
        // engine works with.
        CHECK(action(R"({"gamepadAxis": "LeftX", "threshold": 1e300})") ==
              "action 'jump', binding 2: 'threshold' is not a number a control can have");

        // Bindings of axes.
        const auto axis = [](const std::string& binding) {
            return refusal(withAxes(R"([{"name": "moveX", "bindings": [)" + binding + "]}]"));
        };
        CHECK(axis("{}") == "axis 'moveX', binding 1 names one control: a 'positiveKey' and a 'negativeKey', a "
                            "'positiveButton' and a 'negativeButton', a 'gamepadAxis', 'mouse' or 'scroll'");
        CHECK(axis(R"({"positiveKey": "D", "gamepadAxis": "LeftX"})").find("names one control") != std::string::npos);
        CHECK(axis(R"({"positiveKey": "D", "negativeButton": "A"})").find("names one control") != std::string::npos);
        CHECK(axis(R"({"mouse": "x", "scroll": "y"})").find("names one control") != std::string::npos);
        CHECK(axis(R"({"key": "D"})") == "axis 'moveX', binding 1: unknown member 'key'");
        CHECK(axis(R"({"positiveKey": "Right arrow"})").find("is not the name of") != std::string::npos);
        CHECK(axis(R"({"negativeButton": "Select"})").find("is not the name of") != std::string::npos);
        CHECK(axis(R"({"mouse": "z"})").find("is not the name of") != std::string::npos);
        CHECK(axis(R"({"scroll": 1})").find("is the name of") != std::string::npos);
        CHECK(axis(R"({"positiveKey": "D", "deadZone": 0.2})") ==
              "axis 'moveX', binding 1: only a 'gamepadAxis' has a 'deadZone'");
        CHECK(axis(R"({"gamepadAxis": "LeftX", "deadZone": 1})") ==
              "axis 'moveX', binding 1: a 'deadZone' is from 0 to less than 1, not 1");
        CHECK(axis(R"({"gamepadAxis": "LeftX", "deadZone": -0.1})").find("a 'deadZone' is from 0") !=
              std::string::npos);
        CHECK(axis(R"({"gamepadAxis": "LeftX", "scale": 0})") ==
              "axis 'moveX', binding 1: with a 'scale' of 0 it would never move anything");
        CHECK(axis(R"({"gamepadAxis": "LeftX", "scale": 1e300})").find("is not a number") != std::string::npos);
        CHECK(refusal(withAxes(R"([{"name": "moveX", "bindings": []}, {"name": "moveX", "bindings": []}])")) ==
              "axis 'moveX' is there twice");
    }

    TEST_CASE("a map of more entries or bindings than a game has controls is refused") {
        std::string many = "[";
        for (size_t index = 0; index <= kMaxInputMapEntries; ++index) {
            many += (index == 0 ? "" : ", ") + std::string(R"({"name": "a)") + std::to_string(index) +
                    R"(", "bindings": []})";
        }
        many += "]";
        CHECK(refusal(withActions(many)) == "'actions' has 1025 entries, and at most 1024 are allowed");
        CHECK(refusal(withAxes(many)) == "'axes' has 1025 entries, and at most 1024 are allowed");

        std::string bindings = "[";
        for (size_t index = 0; index <= kMaxInputBindings; ++index) {
            bindings += (index == 0 ? "" : ", ") + std::string(R"({"key": "Space"})");
        }
        bindings += "]";
        CHECK(refusal(withActions(R"([{"name": "jump", "bindings": )" + bindings + "}]")) ==
              "action 'jump' has 65 bindings, and at most 64 are allowed");
    }
}

TEST_SUITE("InputActions") {
    TEST_CASE("an action is down while one of its bindings is, and pressed and released once") {
        Input input;
        InputActions actions;
        actions.setMap(controls());
        CHECK(actions.hasAction("jump"));
        CHECK_FALSE(actions.hasAction("moveX"));
        CHECK_FALSE(actions.hasAction("crouch"));

        input.beginFrame();
        actions.update(input);
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.events().empty());

        input.beginFrame();
        input.injectKey(Key::Space, true);
        actions.update(input);
        CHECK(actions.isDown("jump"));
        CHECK(actions.wasPressed("jump"));
        CHECK_FALSE(actions.wasReleased("jump"));
        CHECK_FALSE(actions.isDown("fire"));
        REQUIRE(actions.events().size() == 1);
        CHECK(actions.events()[0] == ActionEvent{.action = "jump", .pressed = true});

        // Held: down, and neither pressed nor released again. Another binding going down
        // as well presses nothing.
        input.beginFrame();
        GamepadState held;
        held.buttons.set(static_cast<size_t>(GamepadButton::A));
        input.injectGamepad(2, &held);
        actions.update(input);
        CHECK(actions.isDown("jump"));
        CHECK_FALSE(actions.wasPressed("jump"));
        CHECK(actions.events().empty());

        // One of the two comes up: still down. The other as well: released.
        input.beginFrame();
        input.injectKey(Key::Space, false);
        actions.update(input);
        CHECK(actions.isDown("jump"));
        CHECK_FALSE(actions.wasReleased("jump"));
        input.beginFrame();
        input.injectGamepad(2, nullptr);
        actions.update(input);
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.wasReleased("jump"));
        REQUIRE(actions.events().size() == 1);
        CHECK(actions.events()[0] == ActionEvent{.action = "jump", .pressed = false});
        input.beginFrame();
        actions.update(input);
        CHECK_FALSE(actions.wasReleased("jump"));
    }

    TEST_CASE("a key that is tapped between two frames was pressed, and released") {
        Input input;
        InputActions actions;
        actions.setMap(controls());
        input.beginFrame();
        actions.update(input);

        input.beginFrame();
        input.injectKey(Key::Space, true);
        input.injectKey(Key::Space, false);
        input.injectMouseButton(MouseButton::Left, true);
        actions.update(input);
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.wasPressed("jump"));
        CHECK(actions.wasReleased("jump"));
        CHECK(actions.isDown("fire"));
        // In the order of the map; what was tapped is pressed, then released.
        REQUIRE(actions.events().size() == 3);
        CHECK(actions.events()[0] == ActionEvent{.action = "jump", .pressed = true});
        CHECK(actions.events()[1] == ActionEvent{.action = "jump", .pressed = false});
        CHECK(actions.events()[2] == ActionEvent{.action = "fire", .pressed = true});

        input.beginFrame();
        actions.update(input);
        CHECK_FALSE(actions.wasPressed("jump"));
        CHECK_FALSE(actions.wasReleased("jump"));
        CHECK(actions.isDown("fire"));
        CHECK(actions.events().empty());
    }

    TEST_CASE("a trigger or a stick presses an action when it is pushed as far as the threshold") {
        Input input;
        InputActions actions;
        actions.setMap(controls());
        input.beginFrame();
        actions.update(input);

        // The trigger: a quarter of its way, or further.
        GamepadState state = pad(GamepadAxis::RightTrigger, 0.2f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK_FALSE(actions.isDown("fire"));
        state = pad(GamepadAxis::RightTrigger, 0.25f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK(actions.isDown("fire"));
        CHECK(actions.wasPressed("fire"));

        // A threshold below nothing is passed the other way: the stick pulled back.
        state = pad(GamepadAxis::LeftY, 0.9f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK_FALSE(actions.isDown("back"));
        CHECK(actions.wasReleased("fire"));
        state = pad(GamepadAxis::LeftY, -0.5f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK(actions.isDown("back"));

        // Any gamepad will do; and a value that is no number pushes nothing.
        state = pad(GamepadAxis::LeftY, kNotANumber);
        GamepadState other = pad(GamepadAxis::RightTrigger, 1.0f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        input.injectGamepad(3, &other);
        actions.update(input);
        CHECK_FALSE(actions.isDown("back"));
        CHECK(actions.isDown("fire"));
        // An action without bindings is never down.
        CHECK_FALSE(actions.isDown("idle"));
    }

    TEST_CASE("an axis has the value of whichever of its bindings is moved furthest") {
        Input input;
        InputActions actions;
        actions.setMap(controls());
        CHECK(actions.hasAxis("moveX"));
        CHECK_FALSE(actions.hasAxis("jump"));
        input.beginFrame();
        actions.update(input);
        CHECK(actions.axis("moveX") == 0.0f);

        // Two keys: one way, the other, and nothing when both are held.
        input.beginFrame();
        input.injectKey(Key::D, true);
        actions.update(input);
        CHECK(actions.axis("moveX") == 1.0f);
        input.beginFrame();
        input.injectKey(Key::A, true);
        actions.update(input);
        CHECK(actions.axis("moveX") == 0.0f);
        input.beginFrame();
        input.injectKey(Key::D, false);
        actions.update(input);
        CHECK(actions.axis("moveX") == -1.0f);
        input.injectKey(Key::A, false);

        // A stick: nothing within its dead zone, and from its edge up to 1 at the end.
        GamepadState state = pad(GamepadAxis::LeftX, 0.2f);
        input.beginFrame();
        input.injectGamepad(1, &state);
        actions.update(input);
        CHECK(actions.axis("moveX") == 0.0f);
        state = pad(GamepadAxis::LeftX, 0.6f);
        input.beginFrame();
        input.injectGamepad(1, &state);
        actions.update(input);
        CHECK(actions.axis("moveX") == doctest::Approx(0.5f));
        state = pad(GamepadAxis::LeftX, -1.0f);
        input.beginFrame();
        input.injectGamepad(1, &state);
        actions.update(input);
        CHECK(actions.axis("moveX") == -1.0f);
        // Further than its end it does not go, whatever a driver says.
        state = pad(GamepadAxis::LeftX, 3.0f);
        input.beginFrame();
        input.injectGamepad(1, &state);
        actions.update(input);
        CHECK(actions.axis("moveX") == 1.0f);

        // The furthest wins: half a stick against a key, and the buttons of another gamepad.
        state = pad(GamepadAxis::LeftX, 0.6f);
        GamepadState buttons;
        buttons.buttons.set(static_cast<size_t>(GamepadButton::DpadLeft));
        input.beginFrame();
        input.injectGamepad(1, &state);
        input.injectGamepad(0, &buttons);
        actions.update(input);
        CHECK(actions.axis("moveX") == -1.0f);
        input.injectGamepad(0, nullptr);
        input.injectGamepad(1, nullptr);

        // A scale turns a binding round: the stick's Y points down, and forward is up.
        state = pad(GamepadAxis::LeftY, -1.0f);
        input.beginFrame();
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK(actions.axis("moveY") == 1.0f);
        // One key alone goes one way only.
        input.beginFrame();
        input.injectGamepad(0, nullptr);
        input.injectKey(Key::W, true);
        actions.update(input);
        CHECK(actions.axis("moveY") == 1.0f);
        input.injectKey(Key::W, false);
        // One button alone as well: the other way, here.
        buttons = GamepadState{};
        buttons.buttons.set(static_cast<size_t>(GamepadButton::LeftBumper));
        input.beginFrame();
        input.injectGamepad(0, &buttons);
        actions.update(input);
        CHECK(actions.axis("zoom") == -1.0f);
        input.injectGamepad(0, nullptr);
    }

    TEST_CASE("the pointer and the wheel give how far they went in the frame") {
        Input input;
        InputActions actions;
        actions.setMap(controls());
        input.beginFrame();
        input.injectMouseMove(Vec2(100.0f, 100.0f));
        actions.update(input);

        input.beginFrame();
        input.injectMouseMove(Vec2(130.0f, 90.0f));
        input.injectScroll(Vec2(0.0f, 2.0f));
        actions.update(input);
        // Thirty pixels, a tenth of which counts.
        CHECK(actions.axis("lookX") == doctest::Approx(3.0f));
        CHECK(actions.axis("zoom") == 2.0f);

        // A frame in which neither moves: nothing.
        input.beginFrame();
        actions.update(input);
        CHECK(actions.axis("lookX") == 0.0f);
        CHECK(actions.axis("zoom") == 0.0f);
        // A stick is scaled the same way, and wins where it gives more.
        GamepadState state = pad(GamepadAxis::RightX, -0.8f);
        input.beginFrame();
        input.injectMouseMove(Vec2(140.0f, 90.0f));
        input.injectGamepad(0, &state);
        actions.update(input);
        CHECK(actions.axis("lookX") == doctest::Approx(-2.0f));
    }

    TEST_CASE("names that a map does not have are never pressed and never moved") {
        Input input;
        InputActions actions;
        // Without a map there is nothing.
        input.beginFrame();
        input.injectKey(Key::Space, true);
        actions.update(input);
        CHECK(actions.map() == InputMap{});
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.axis("moveX") == 0.0f);
        CHECK(actions.events().empty());

        actions.setMap(controls());
        input.beginFrame();
        actions.update(input);
        CHECK_FALSE(actions.isDown("crouch"));
        CHECK_FALSE(actions.wasPressed("crouch"));
        CHECK_FALSE(actions.wasReleased("crouch"));
        CHECK(actions.axis("strafe") == 0.0f);
        CHECK_FALSE(actions.isDown(""));
        // An action is no axis, and an axis no action.
        CHECK(actions.axis("jump") == 0.0f);
        CHECK_FALSE(actions.isDown("moveX"));
    }

    TEST_CASE("what is held when a map is taken was not pressed for it") {
        Input input;
        InputActions actions;
        // A key, a stick and a button of a gamepad that are held since an earlier frame.
        input.beginFrame();
        input.injectKey(Key::Space, true);
        input.injectKey(Key::D, true);
        GamepadState pulled = pad(GamepadAxis::LeftY, -1.0f);
        input.injectGamepad(0, &pulled);
        input.beginFrame();
        actions.setMap(controls());
        // Nothing is anything until an update has looked.
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.axis("moveX") == 0.0f);

        actions.update(input);
        CHECK(actions.isDown("jump"));
        CHECK_FALSE(actions.wasPressed("jump"));
        CHECK(actions.isDown("back"));
        CHECK_FALSE(actions.wasPressed("back"));
        CHECK(actions.events().empty());
        CHECK(actions.axis("moveX") == 1.0f);

        // Released, it is released: that happened under this map.
        input.beginFrame();
        input.injectKey(Key::Space, false);
        actions.update(input);
        CHECK(actions.wasReleased("jump"));

        // What goes down in the very frame a map first looks at was pressed for it: a
        // key says so itself.
        InputActions prompt;
        prompt.setMap(controls());
        input.beginFrame();
        input.injectMouseButton(MouseButton::Left, true);
        prompt.update(input);
        CHECK(prompt.wasPressed("fire"));
        CHECK_FALSE(prompt.wasPressed("jump"));
        REQUIRE(prompt.events().size() == 1);
        CHECK(prompt.events()[0] == ActionEvent{.action = "fire", .pressed = true});
        // And what comes up in it was released, and a tap is both.
        InputActions late;
        late.setMap(controls());
        input.beginFrame();
        input.injectMouseButton(MouseButton::Left, false);
        input.injectKey(Key::Space, false);
        input.injectKey(Key::Space, true);
        input.injectKey(Key::Space, false);
        late.update(input);
        CHECK(late.wasReleased("fire"));
        CHECK_FALSE(late.wasPressed("fire"));
        CHECK(late.wasPressed("jump"));
        CHECK(late.wasReleased("jump"));

        // Another map: what the one before knew is gone.
        input.beginFrame();
        input.injectKey(Key::Space, true);
        actions.update(input);
        REQUIRE(actions.isDown("jump"));
        InputMap other;
        other.actions.push_back({.name = "leap", .bindings = {ActionBinding(Key::Space)}});
        actions.setMap(std::move(other));
        CHECK_FALSE(actions.isDown("jump"));
        CHECK(actions.events().empty());
        input.beginFrame();
        actions.update(input);
        CHECK(actions.isDown("leap"));
        CHECK_FALSE(actions.wasPressed("leap"));
        CHECK_FALSE(actions.hasAction("jump"));
    }
}
