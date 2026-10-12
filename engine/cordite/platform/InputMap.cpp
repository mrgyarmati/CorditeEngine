#include "cordite/platform/InputMap.h"

#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

namespace cordite {
namespace {

constexpr DocumentFormat kInputFormat{.name = "cordite.input", .version = 1, .kind = "map of controls"};

/// Fails for a member of `object` that is none of `known`, saying where: a name that
/// was mistyped must not be taken for a control that was left out.
Status checkMembers(const Json& object, std::initializer_list<std::string_view> known, std::string_view where) {
    const Status status = rejectUnknownMembers(object, std::span<const std::string_view>(known.begin(), known.size()));
    if (!status) {
        return makeError(status.error().code, "{}: {}", where, status.error().message);
    }
    return {};
}

/// A member that is a number, made of digits: not too large to be one, and not missing
/// where `fallback` says what it is then.
Result<float> readNumber(const Json& object, std::string_view key, float fallback, std::string_view where) {
    const auto member = object.find(key);
    if (member == object.end()) {
        return fallback;
    }
    if (!member->is_number()) {
        return makeError(ErrorCode::ParseError, "{}: '{}' is a number, not {}", where, key, jsonTypeName(*member));
    }
    const auto value = member->get<float>();
    if (!std::isfinite(value)) {
        return makeError(ErrorCode::ParseError, "{}: '{}' is not a number a control can have", where, key);
    }
    return value;
}

/// A member that names something, by a function that knows the names: a key, a button,
/// a stick. Nothing for a member that is not there.
template <typename Named, typename FromName>
Result<std::optional<Named>> readNamed(const Json& object, std::string_view key, FromName fromName,
                                       std::string_view kind, std::string_view where) {
    const auto member = object.find(key);
    if (member == object.end()) {
        return std::optional<Named>();
    }
    if (!member->is_string()) {
        return makeError(ErrorCode::ParseError, "{}: '{}' is the name of {}, not {}", where, key, kind,
                         jsonTypeName(*member));
    }
    const std::optional<Named> named = fromName(member->template get_ref<const std::string&>());
    if (!named) {
        return makeError(ErrorCode::ParseError, "{}: \"{}\" is not the name of {}", where,
                         member->template get_ref<const std::string&>(), kind);
    }
    return named;
}

std::optional<PointerAxis> pointerAxisFromName(std::string_view name) {
    if (name == "x" || name == "X") {
        return PointerAxis::X;
    }
    if (name == "y" || name == "Y") {
        return PointerAxis::Y;
    }
    return std::nullopt;
}

std::string_view nameOf(PointerAxis axis) {
    return axis == PointerAxis::X ? "x" : "y";
}

/// How many of the members that say what kind of binding one is `binding` has.
size_t countOf(const Json& binding, std::initializer_list<std::string_view> members) {
    return static_cast<size_t>(
        std::ranges::count_if(members, [&](std::string_view member) { return binding.contains(member); }));
}

Result<ActionBinding> parseActionBinding(const Json& json, std::string_view where) {
    if (!json.is_object()) {
        return makeError(ErrorCode::ParseError, "{} is an object, not {}", where, jsonTypeName(json));
    }
    CORDITE_TRY(checkMembers(json, {"key", "mouseButton", "gamepadButton", "gamepadAxis", "threshold"}, where));
    if (countOf(json, {"key", "mouseButton", "gamepadButton", "gamepadAxis"}) != 1) {
        return makeError(ErrorCode::ParseError,
                         "{} names one control: a 'key', a 'mouseButton', a 'gamepadButton' or a 'gamepadAxis'", where);
    }
    if (json.contains("threshold") && !json.contains("gamepadAxis")) {
        return makeError(ErrorCode::ParseError, "{}: only a 'gamepadAxis' has a 'threshold'", where);
    }

    CORDITE_TRY_ASSIGN(const std::optional<Key> key, readNamed<Key>(json, "key", keyFromName, "a key", where));
    if (key) {
        return ActionBinding(*key);
    }
    CORDITE_TRY_ASSIGN(const std::optional<MouseButton> mouseButton,
                       readNamed<MouseButton>(json, "mouseButton", mouseButtonFromName, "a mouse button", where));
    if (mouseButton) {
        return ActionBinding(*mouseButton);
    }
    CORDITE_TRY_ASSIGN(
        const std::optional<GamepadButton> gamepadButton,
        readNamed<GamepadButton>(json, "gamepadButton", gamepadButtonFromName, "a gamepad button", where));
    if (gamepadButton) {
        return ActionBinding(*gamepadButton);
    }
    CORDITE_TRY_ASSIGN(const std::optional<GamepadAxis> axis,
                       readNamed<GamepadAxis>(json, "gamepadAxis", gamepadAxisFromName, "a gamepad axis", where));
    CORDITE_TRY_ASSIGN(const float threshold, readNumber(json, "threshold", kDefaultActionThreshold, where));
    if (threshold == 0.0f) {
        return makeError(ErrorCode::ParseError,
                         "{}: a 'threshold' of 0 is passed by a stick at rest; it is above 0, or below", where);
    }
    // One of the four is there, and it was none of the other three.
    return ActionBinding(AxisThreshold{.axis = axis.value_or(GamepadAxis::LeftX), .threshold = threshold});
}

Result<AxisBinding> parseAxisBinding(const Json& json, std::string_view where) {
    if (!json.is_object()) {
        return makeError(ErrorCode::ParseError, "{} is an object, not {}", where, jsonTypeName(json));
    }
    CORDITE_TRY(checkMembers(json,
                             {"positiveKey", "negativeKey", "positiveButton", "negativeButton", "gamepadAxis",
                              "deadZone", "mouse", "scroll", "scale"},
                             where));
    const bool isKeys = countOf(json, {"positiveKey", "negativeKey"}) > 0;
    const bool isButtons = countOf(json, {"positiveButton", "negativeButton"}) > 0;
    const size_t kinds = (isKeys ? 1u : 0u) + (isButtons ? 1u : 0u) + countOf(json, {"gamepadAxis", "mouse", "scroll"});
    if (kinds != 1) {
        return makeError(ErrorCode::ParseError,
                         "{} names one control: a 'positiveKey' and a 'negativeKey', a 'positiveButton' and a "
                         "'negativeButton', a 'gamepadAxis', 'mouse' or 'scroll'",
                         where);
    }
    if (json.contains("deadZone") && !json.contains("gamepadAxis")) {
        return makeError(ErrorCode::ParseError, "{}: only a 'gamepadAxis' has a 'deadZone'", where);
    }

    AxisBinding binding;
    CORDITE_TRY_ASSIGN(binding.scale, readNumber(json, "scale", 1.0f, where));
    if (binding.scale == 0.0f) {
        return makeError(ErrorCode::ParseError, "{}: with a 'scale' of 0 it would never move anything", where);
    }
    if (isKeys) {
        CORDITE_TRY_ASSIGN(const std::optional<Key> positive,
                           readNamed<Key>(json, "positiveKey", keyFromName, "a key", where));
        CORDITE_TRY_ASSIGN(const std::optional<Key> negative,
                           readNamed<Key>(json, "negativeKey", keyFromName, "a key", where));
        binding.source =
            KeyPair{.positive = positive.value_or(Key::Unknown), .negative = negative.value_or(Key::Unknown)};
    } else if (isButtons) {
        CORDITE_TRY_ASSIGN(
            const std::optional<GamepadButton> positive,
            readNamed<GamepadButton>(json, "positiveButton", gamepadButtonFromName, "a gamepad button", where));
        CORDITE_TRY_ASSIGN(
            const std::optional<GamepadButton> negative,
            readNamed<GamepadButton>(json, "negativeButton", gamepadButtonFromName, "a gamepad button", where));
        binding.source = ButtonPair{.hasPositive = positive.has_value(),
                                    .positive = positive.value_or(GamepadButton::A),
                                    .hasNegative = negative.has_value(),
                                    .negative = negative.value_or(GamepadButton::A)};
    } else if (json.contains("gamepadAxis")) {
        CORDITE_TRY_ASSIGN(const std::optional<GamepadAxis> axis,
                           readNamed<GamepadAxis>(json, "gamepadAxis", gamepadAxisFromName, "a gamepad axis", where));
        CORDITE_TRY_ASSIGN(const float deadZone, readNumber(json, "deadZone", kDefaultDeadZone, where));
        if (deadZone < 0.0f || deadZone >= 1.0f) {
            return makeError(ErrorCode::ParseError, "{}: a 'deadZone' is from 0 to less than 1, not {}", where,
                             deadZone);
        }
        binding.source = StickAxis{.axis = axis.value_or(GamepadAxis::LeftX), .deadZone = deadZone};
    } else {
        const bool isMouse = json.contains("mouse");
        const std::string_view member = isMouse ? "mouse" : "scroll";
        CORDITE_TRY_ASSIGN(
            const std::optional<PointerAxis> axis,
            readNamed<PointerAxis>(json, member, pointerAxisFromName, "a direction, \"x\" or \"y\"", where));
        if (isMouse) {
            binding.source = MouseMotion{.axis = axis.value_or(PointerAxis::X)};
        } else {
            binding.source = MouseScroll{.axis = axis.value_or(PointerAxis::Y)};
        }
    }
    return binding;
}

/// Reads the entries of `kind` - "action" or "axis" - under `member`: each a name and
/// its bindings, read by `parseBinding`.
template <typename Entry, typename ParseBinding>
Result<std::vector<Entry>> parseEntries(const Json& json, std::string_view member, std::string_view kind,
                                        ParseBinding parseBinding) {
    std::vector<Entry> entries;
    const auto list = json.find(member);
    if (list == json.end()) {
        return entries;
    }
    if (!list->is_array()) {
        return makeError(ErrorCode::ParseError, "'{}' is a list, not {}", member, jsonTypeName(*list));
    }
    if (list->size() > kMaxInputMapEntries) {
        return makeError(ErrorCode::ParseError, "'{}' has {} entries, and at most {} are allowed", member, list->size(),
                         kMaxInputMapEntries);
    }
    for (const Json& item : *list) {
        if (!item.is_object()) {
            return makeError(ErrorCode::ParseError, "an entry of '{}' is an object, not {}", member,
                             jsonTypeName(item));
        }
        Entry entry;
        CORDITE_TRY_ASSIGN(entry.name, jsonGet<std::string>(item, "name"));
        if (entry.name.empty()) {
            return makeError(ErrorCode::ParseError, "an {} of '{}' has no name", kind, member);
        }
        const std::string where = fmt::format("{} '{}'", kind, entry.name);
        CORDITE_TRY(checkMembers(item, {"name", "bindings"}, where));
        if (std::ranges::any_of(entries, [&](const Entry& other) { return other.name == entry.name; })) {
            return makeError(ErrorCode::ParseError, "{} is there twice", where);
        }
        const auto bindings = item.find("bindings");
        if (bindings == item.end() || !bindings->is_array()) {
            return makeError(ErrorCode::ParseError, "{} has a list of 'bindings'", where);
        }
        if (bindings->size() > kMaxInputBindings) {
            return makeError(ErrorCode::ParseError, "{} has {} bindings, and at most {} are allowed", where,
                             bindings->size(), kMaxInputBindings);
        }
        for (size_t index = 0; index < bindings->size(); ++index) {
            CORDITE_TRY_ASSIGN(auto binding,
                               parseBinding((*bindings)[index], fmt::format("{}, binding {}", where, index + 1)));
            entry.bindings.push_back(std::move(binding));
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

Json serializeActionBinding(const ActionBinding& binding) {
    Json json = Json::object();
    if (const auto* key = std::get_if<Key>(&binding)) {
        json["key"] = std::string(nameOf(*key));
    } else if (const auto* mouseButton = std::get_if<MouseButton>(&binding)) {
        json["mouseButton"] = std::string(nameOf(*mouseButton));
    } else if (const auto* gamepadButton = std::get_if<GamepadButton>(&binding)) {
        json["gamepadButton"] = std::string(nameOf(*gamepadButton));
    } else if (const auto* threshold = std::get_if<AxisThreshold>(&binding)) {
        json["gamepadAxis"] = std::string(nameOf(threshold->axis));
        json["threshold"] = jsonFromFloat(threshold->threshold);
    }
    return json;
}

Json serializeAxisBinding(const AxisBinding& binding) {
    Json json = Json::object();
    if (const auto* keys = std::get_if<KeyPair>(&binding.source)) {
        if (keys->positive != Key::Unknown) {
            json["positiveKey"] = std::string(nameOf(keys->positive));
        }
        if (keys->negative != Key::Unknown) {
            json["negativeKey"] = std::string(nameOf(keys->negative));
        }
    } else if (const auto* buttons = std::get_if<ButtonPair>(&binding.source)) {
        if (buttons->hasPositive) {
            json["positiveButton"] = std::string(nameOf(buttons->positive));
        }
        if (buttons->hasNegative) {
            json["negativeButton"] = std::string(nameOf(buttons->negative));
        }
    } else if (const auto* stick = std::get_if<StickAxis>(&binding.source)) {
        json["gamepadAxis"] = std::string(nameOf(stick->axis));
        json["deadZone"] = jsonFromFloat(stick->deadZone);
    } else if (const auto* motion = std::get_if<MouseMotion>(&binding.source)) {
        json["mouse"] = std::string(nameOf(motion->axis));
    } else if (const auto* scroll = std::get_if<MouseScroll>(&binding.source)) {
        json["scroll"] = std::string(nameOf(scroll->axis));
    }
    json["scale"] = jsonFromFloat(binding.scale);
    return json;
}

/// Calls `visit(index)` for every gamepad that is connected.
template <typename Visit>
void forEachGamepad(const Input& input, Visit&& visit) {
    for (uint32_t index = 0; index < Input::kMaxGamepads; ++index) {
        if (input.isGamepadConnected(index)) {
            visit(index);
        }
    }
}

/// How far a stick or trigger is pushed on the gamepad that pushes it furthest; nothing
/// for a value that is no number.
float furthest(const Input& input, GamepadAxis axis) {
    float value = 0.0f;
    forEachGamepad(input, [&](uint32_t index) {
        const float pushed = input.gamepadAxis(index, axis);
        if (std::isfinite(pushed) && std::abs(pushed) > std::abs(value)) {
            value = pushed;
        }
    });
    return value;
}

bool isButtonDown(const Input& input, GamepadButton button) {
    bool down = false;
    forEachGamepad(input, [&](uint32_t index) { down = down || input.isGamepadButtonDown(index, button); });
    return down;
}

/// Whether a binding of an action is held, and whether it went down or came up during
/// the frame - which only a key and a button of the pointer say; of a gamepad there is
/// what it is like now.
struct Held {
    bool down = false;
    bool pressed = false;
    bool released = false;
};

Held heldOf(const Input& input, const ActionBinding& binding) {
    if (const auto* key = std::get_if<Key>(&binding)) {
        return {.down = input.isKeyDown(*key),
                .pressed = input.wasKeyPressed(*key),
                .released = input.wasKeyReleased(*key)};
    }
    if (const auto* mouseButton = std::get_if<MouseButton>(&binding)) {
        return {.down = input.isMouseButtonDown(*mouseButton),
                .pressed = input.wasMouseButtonPressed(*mouseButton),
                .released = input.wasMouseButtonReleased(*mouseButton)};
    }
    if (const auto* gamepadButton = std::get_if<GamepadButton>(&binding)) {
        return {.down = isButtonDown(input, *gamepadButton)};
    }
    const auto& threshold = std::get<AxisThreshold>(binding);
    const float pushed = furthest(input, threshold.axis);
    return {.down = threshold.threshold > 0.0f ? pushed >= threshold.threshold : pushed <= threshold.threshold};
}

float valueOf(const Input& input, const AxisBinding& binding) {
    float value = 0.0f;
    if (const auto* keys = std::get_if<KeyPair>(&binding.source)) {
        value = (input.isKeyDown(keys->positive) ? 1.0f : 0.0f) - (input.isKeyDown(keys->negative) ? 1.0f : 0.0f);
    } else if (const auto* buttons = std::get_if<ButtonPair>(&binding.source)) {
        value = (buttons->hasPositive && isButtonDown(input, buttons->positive) ? 1.0f : 0.0f) -
                (buttons->hasNegative && isButtonDown(input, buttons->negative) ? 1.0f : 0.0f);
    } else if (const auto* stick = std::get_if<StickAxis>(&binding.source)) {
        // Nothing within the dead zone, and from its edge on up to 1 at the end of the way.
        const float pushed = furthest(input, stick->axis);
        const float beyond = std::max(std::abs(pushed) - stick->deadZone, 0.0f) / (1.0f - stick->deadZone);
        value = std::copysign(std::min(beyond, 1.0f), pushed);
    } else if (const auto* motion = std::get_if<MouseMotion>(&binding.source)) {
        value = motion->axis == PointerAxis::X ? input.mouseDelta().x : input.mouseDelta().y;
    } else if (const auto* scroll = std::get_if<MouseScroll>(&binding.source)) {
        value = scroll->axis == PointerAxis::X ? input.scrollDelta().x : input.scrollDelta().y;
    }
    return std::isfinite(value) ? value * binding.scale : 0.0f;
}

/// The place of the entry called `name`, or the number of entries for none.
template <typename Entry>
size_t placeOf(const std::vector<Entry>& entries, std::string_view name) {
    const auto found = std::ranges::find(entries, name, &Entry::name);
    return static_cast<size_t>(found - entries.begin());
}

} // namespace

Result<InputMap> parseInputMap(const Json& json) {
    // There is one version, and so nothing that depends on which was read.
    CORDITE_TRY_ASSIGN([[maybe_unused]] const uint32_t version, readDocumentHeader(json, kInputFormat));
    CORDITE_TRY(checkMembers(json, {"format", "version", "actions", "axes"}, "a map of controls"));

    InputMap map;
    CORDITE_TRY_ASSIGN(map.actions, parseEntries<InputAction>(json, "actions", "action", parseActionBinding));
    CORDITE_TRY_ASSIGN(map.axes, parseEntries<InputAxis>(json, "axes", "axis", parseAxisBinding));
    return map;
}

Json serializeInputMap(const InputMap& map) {
    Json json = makeDocument(kInputFormat);
    Json actions = Json::array();
    for (const InputAction& action : map.actions) {
        Json bindings = Json::array();
        for (const ActionBinding& binding : action.bindings) {
            bindings.push_back(serializeActionBinding(binding));
        }
        Json entry = Json::object();
        entry["name"] = action.name;
        entry["bindings"] = std::move(bindings);
        actions.push_back(std::move(entry));
    }
    json["actions"] = std::move(actions);
    Json axes = Json::array();
    for (const InputAxis& axis : map.axes) {
        Json bindings = Json::array();
        for (const AxisBinding& binding : axis.bindings) {
            bindings.push_back(serializeAxisBinding(binding));
        }
        Json entry = Json::object();
        entry["name"] = axis.name;
        entry["bindings"] = std::move(bindings);
        axes.push_back(std::move(entry));
    }
    json["axes"] = std::move(axes);
    return json;
}

Result<InputMap> loadInputMap(const std::filesystem::path& path) {
    CORDITE_TRY_ASSIGN(const Json json, readJsonFile(path));
    Result<InputMap> map = parseInputMap(json);
    if (!map) {
        return makeError(map.error().code, "{}: {}", pathToUtf8(path), map.error().message);
    }
    return map;
}

Status saveInputMap(const std::filesystem::path& path, const InputMap& map) {
    return writeJsonFile(path, serializeInputMap(map));
}

void InputActions::setMap(InputMap map) {
    m_map = std::move(map);
    m_actions.assign(m_map.actions.size(), ActionState{});
    m_axes.assign(m_map.axes.size(), 0.0f);
    m_events.clear();
    m_fresh = true;
}

void InputActions::update(const Input& input) {
    m_events.clear();
    for (size_t index = 0; index < m_map.actions.size(); ++index) {
        const InputAction& action = m_map.actions[index];
        Held held;
        for (const ActionBinding& binding : action.bindings) {
            const Held one = heldOf(input, binding);
            held.down = held.down || one.down;
            held.pressed = held.pressed || one.pressed;
            held.released = held.released || one.released;
        }
        ActionState& state = m_actions[index];
        if (m_fresh) {
            // A map that was just taken does not know how things were before. A key and
            // a button of the pointer say themselves that they went down or came up in
            // this frame; of a gamepad there is only what it is like now, and that is
            // taken for how it was.
            state.pressed = held.pressed;
            state.released = held.released && !held.down;
        } else {
            // Down and up between two frames is a tap: up before, up now, and pressed on
            // the way.
            const bool tapped = !state.down && !held.down && held.pressed;
            state.pressed = (held.down && !state.down) || tapped;
            state.released = (!held.down && state.down) || tapped;
        }
        state.down = held.down;
        if (state.pressed) {
            m_events.push_back({.action = action.name, .pressed = true});
        }
        if (state.released) {
            m_events.push_back({.action = action.name, .pressed = false});
        }
    }
    for (size_t index = 0; index < m_map.axes.size(); ++index) {
        float furthestValue = 0.0f;
        for (const AxisBinding& binding : m_map.axes[index].bindings) {
            const float value = valueOf(input, binding);
            if (std::abs(value) > std::abs(furthestValue)) {
                furthestValue = value;
            }
        }
        m_axes[index] = furthestValue;
    }
    m_fresh = false;
}

bool InputActions::hasAction(std::string_view name) const {
    return placeOf(m_map.actions, name) < m_map.actions.size();
}

bool InputActions::hasAxis(std::string_view name) const {
    return placeOf(m_map.axes, name) < m_map.axes.size();
}

bool InputActions::isDown(std::string_view action) const {
    const size_t place = placeOf(m_map.actions, action);
    return place < m_actions.size() && m_actions[place].down;
}

bool InputActions::wasPressed(std::string_view action) const {
    const size_t place = placeOf(m_map.actions, action);
    return place < m_actions.size() && m_actions[place].pressed;
}

bool InputActions::wasReleased(std::string_view action) const {
    const size_t place = placeOf(m_map.actions, action);
    return place < m_actions.size() && m_actions[place].released;
}

float InputActions::axis(std::string_view axis) const {
    const size_t place = placeOf(m_map.axes, axis);
    return place < m_axes.size() ? m_axes[place] : 0.0f;
}

} // namespace cordite
