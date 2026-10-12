#include "cordite/editor/InputCommands.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/math/Math.h"
#include "cordite/platform/KeyCodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

using Type = CommandParameterType;

/// What a call of input.inject is about: one of these, and no more.
constexpr std::array<std::string_view, 5> kInjected = {"key", "mouseButton", "mouseMove", "scroll", "gamepad"};
/// The furthest a pointer is put, and the most a wheel is turned in one call: beyond
/// any screen, and a number that the arithmetic of an interface survives.
constexpr double kFurthestPointer = 1000000.0;

/// A parameter that is two numbers, neither further from nothing than `furthest`.
Result<Vec2> pairParameter(const Json& parameters, const char* name, double furthest) {
    const Json& pair = parameters.at(name);
    const bool isPair = pair.is_array() && pair.size() == 2 && pair[0].is_number() && pair[1].is_number();
    if (!isPair) {
        return makeError(ErrorCode::InvalidArgument, "'{}' must be an array of 2 numbers", name);
    }
    const double x = pair[0].get<double>();
    const double y = pair[1].get<double>();
    if (!(std::abs(x) <= furthest) || !(std::abs(y) <= furthest)) {
        return makeError(ErrorCode::OutOfRange, "'{}' must be within {} of nothing either way, got [{}, {}]", name,
                         furthest, x, y);
    }
    return Vec2(static_cast<float>(x), static_cast<float>(y));
}

/// Whether the key or the button of a call goes down or comes up: it has to say.
Result<bool> downParameter(const Json& parameters, std::string_view what) {
    CORDITE_TRY_ASSIGN(const std::optional<bool> down, jsonGetOptional<bool>(parameters, "down"));
    if (!down) {
        return makeError(ErrorCode::InvalidArgument, "'down' says whether the {} goes down or comes up: true or false",
                         what);
    }
    return *down;
}

/// What a gamepad is like, as a call says it: the buttons that are held, by name, and
/// how far its sticks and triggers are pushed.
Result<GamepadState> gamepadParameter(const Json& parameters) {
    GamepadState state;
    if (const auto buttons = parameters.find("buttons"); buttons != parameters.end()) {
        for (const Json& name : *buttons) {
            const std::optional<GamepadButton> button =
                name.is_string() ? gamepadButtonFromName(name.get_ref<const std::string&>()) : std::nullopt;
            if (!button) {
                return makeError(ErrorCode::InvalidArgument, "'buttons' lists the names of gamepad buttons; {} is none",
                                 name.dump());
            }
            state.buttons.set(static_cast<size_t>(*button));
        }
    }
    if (const auto axes = parameters.find("axes"); axes != parameters.end()) {
        for (const auto& [name, value] : axes->items()) {
            const std::optional<GamepadAxis> axis = gamepadAxisFromName(name);
            if (!axis) {
                return makeError(ErrorCode::InvalidArgument, "'axes' has the names of gamepad axes; \"{}\" is none",
                                 name);
            }
            const double pushed = value.is_number() ? value.get<double>() : 2.0;
            if (!(std::abs(pushed) <= 1.0)) {
                return makeError(ErrorCode::OutOfRange, "the axis \"{}\" is pushed from -1 to 1, not {}", name,
                                 value.dump());
            }
            state.axes[static_cast<size_t>(*axis)] = static_cast<float>(pushed);
        }
    }
    return state;
}

Result<Json> inject(Input& input, const Json& parameters) {
    const auto given =
        std::ranges::count_if(kInjected, [&](std::string_view name) { return parameters.contains(name); });
    if (given != 1) {
        return makeError(ErrorCode::InvalidArgument,
                         "one thing is injected at a time: a 'key', a 'mouseButton', a 'mouseMove', a 'scroll' or a "
                         "'gamepad'");
    }
    if (parameters.contains("key")) {
        const auto& name = parameters.at("key").get_ref<const std::string&>();
        const std::optional<Key> key = keyFromName(name);
        if (!key) {
            return makeError(ErrorCode::InvalidArgument, "\"{}\" is not the name of a key", name);
        }
        CORDITE_TRY_ASSIGN(const bool down, downParameter(parameters, "key"));
        input.injectKey(*key, down);
    } else if (parameters.contains("mouseButton")) {
        const auto& name = parameters.at("mouseButton").get_ref<const std::string&>();
        const std::optional<MouseButton> button = mouseButtonFromName(name);
        if (!button) {
            return makeError(ErrorCode::InvalidArgument, "\"{}\" is not the name of a mouse button", name);
        }
        CORDITE_TRY_ASSIGN(const bool down, downParameter(parameters, "mouse button"));
        input.injectMouseButton(*button, down);
    } else if (parameters.contains("mouseMove")) {
        CORDITE_TRY_ASSIGN(const Vec2 position, pairParameter(parameters, "mouseMove", kFurthestPointer));
        input.injectMouseMove(position);
    } else if (parameters.contains("scroll")) {
        CORDITE_TRY_ASSIGN(const Vec2 turned, pairParameter(parameters, "scroll", kFurthestPointer));
        input.injectScroll(turned);
    } else {
        const auto index = parameters.at("gamepad").get<int64_t>();
        if (index < 0 || index >= static_cast<int64_t>(Input::kMaxGamepads)) {
            return makeError(ErrorCode::OutOfRange, "'gamepad' is from 0 to {}, not {}", Input::kMaxGamepads - 1,
                             index);
        }
        if (!jsonGetOr<bool>(parameters, "connected", true)) {
            input.injectGamepad(static_cast<uint32_t>(index), nullptr);
        } else {
            CORDITE_TRY_ASSIGN(const GamepadState state, gamepadParameter(parameters));
            input.injectGamepad(static_cast<uint32_t>(index), &state);
        }
    }
    return Json::object();
}

Json describeState(const InputActions& actions) {
    Json listed = Json::array();
    for (const InputAction& action : actions.map().actions) {
        Json entry = Json::object();
        entry["name"] = action.name;
        entry["down"] = actions.isDown(action.name);
        entry["pressed"] = actions.wasPressed(action.name);
        entry["released"] = actions.wasReleased(action.name);
        listed.push_back(std::move(entry));
    }
    Json axes = Json::array();
    for (const InputAxis& axis : actions.map().axes) {
        Json entry = Json::object();
        entry["name"] = axis.name;
        entry["value"] = jsonFromFloat(actions.axis(axis.name));
        axes.push_back(std::move(entry));
    }
    Json state = Json::object();
    state["actions"] = std::move(listed);
    state["axes"] = std::move(axes);
    return state;
}

} // namespace

Status registerInputCommands(CommandRegistry& commands, Input& input, InputActions& actions,
                             std::filesystem::path mapFile) {
    CORDITE_TRY(commands.add({
        .name = "input.map",
        .description = "Returns the controls of the game: its actions and axes by name, and the keys, buttons and "
                       "sticks that move them, as the input map of the project has them (`map`, in the format of "
                       "an input file), and the file they were read from (`file`, or null).",
        .handler = [&actions, mapFile = std::move(mapFile)](const Json& /*parameters*/) -> Result<Json> {
            Json result = Json::object();
            result["file"] = mapFile.empty() ? Json(nullptr) : Json(pathToUtf8(mapFile));
            result["map"] = serializeInputMap(actions.map());
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "input.state",
        .description = "Says what the controls were doing in the last frame: of every action whether it is `down`, "
                       "and whether it was `pressed` or `released` in that frame; of every axis its `value`.",
        .handler = [&actions](const Json& /*parameters*/) -> Result<Json> { return describeState(actions); },
    }));

    return commands.add({
        .name = "input.inject",
        .description = "Does what a hand does, one thing at a time: presses or lets go of a key or a mouse button, "
                       "moves the pointer, turns the wheel, or sets what a gamepad is like. The next frame sees it "
                       "- the interface of the editor as well as the actions of the game. A key or a button stays "
                       "down until it is let go of.",
        .parameters =
            {{"key", Type::String, R"(The name of a key, such as "Space" or "W"; with `down`.)", false},
             {"mouseButton", Type::String, R"(The name of a mouse button, such as "Left"; with `down`.)", false},
             {"down", Type::Boolean, "Whether the key or the button goes down (true) or comes up (false).", false},
             {"mouseMove", Type::Array, "Where the pointer goes: [x, y] in pixels from the top left.", false},
             {"scroll", Type::Array, "How far the wheel is turned: [x, y] in notches.", false},
             {"gamepad", Type::Integer,
              "Which gamepad is set, from 0 to 3: to the `buttons` and `axes` given, and connected.", false},
             {"buttons", Type::Array,
              R"(The names of the gamepad buttons that are held, such as ["A"]; none when left out.)", false},
             {"axes", Type::Object,
              R"(How far sticks and triggers are pushed, by name: {"LeftX": 0.5}; at rest when left out.)", false},
             {"connected", Type::Boolean, "False unplugs the gamepad. Defaults to true.", false}},
        .handler = [&input](const Json& parameters) -> Result<Json> { return inject(input, parameters); },
    });
}

} // namespace cordite
