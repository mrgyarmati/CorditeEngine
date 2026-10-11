#include "cordite/platform/KeyCodes.h"

#include "cordite/core/StringUtils.h"

#include <array>

namespace cordite {
namespace {

// Each table is indexed by the enum value. The static_asserts fail the build when an
// enumerator is added without a name.
constexpr std::array<std::string_view, static_cast<size_t>(Key::Count)> kKeyNames = {
    "Unknown",

    "Space",
    "Apostrophe",
    "Comma",
    "Minus",
    "Period",
    "Slash",
    "Num0",
    "Num1",
    "Num2",
    "Num3",
    "Num4",
    "Num5",
    "Num6",
    "Num7",
    "Num8",
    "Num9",
    "Semicolon",
    "Equal",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "LeftBracket",
    "Backslash",
    "RightBracket",
    "GraveAccent",

    "Escape",
    "Enter",
    "Tab",
    "Backspace",
    "Insert",
    "Delete",
    "Right",
    "Left",
    "Down",
    "Up",
    "PageUp",
    "PageDown",
    "Home",
    "End",
    "CapsLock",
    "ScrollLock",
    "NumLock",
    "PrintScreen",
    "Pause",

    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",

    "Keypad0",
    "Keypad1",
    "Keypad2",
    "Keypad3",
    "Keypad4",
    "Keypad5",
    "Keypad6",
    "Keypad7",
    "Keypad8",
    "Keypad9",
    "KeypadDecimal",
    "KeypadDivide",
    "KeypadMultiply",
    "KeypadSubtract",
    "KeypadAdd",
    "KeypadEnter",
    "KeypadEqual",

    "LeftShift",
    "LeftControl",
    "LeftAlt",
    "LeftSuper",
    "RightShift",
    "RightControl",
    "RightAlt",
    "RightSuper",
    "Menu",
};

constexpr std::array<std::string_view, static_cast<size_t>(MouseButton::Count)> kMouseButtonNames = {
    "Left", "Right", "Middle", "Button4", "Button5",
};

constexpr std::array<std::string_view, static_cast<size_t>(GamepadButton::Count)> kGamepadButtonNames = {
    "A",     "B",         "X",          "Y",      "LeftBumper", "RightBumper", "Back",     "Start",
    "Guide", "LeftThumb", "RightThumb", "DpadUp", "DpadRight",  "DpadDown",    "DpadLeft",
};

constexpr std::array<std::string_view, static_cast<size_t>(GamepadAxis::Count)> kGamepadAxisNames = {
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger",
};

// A trailing entry that was forgotten would be value-initialized to an empty view.
static_assert(!kKeyNames.back().empty());
static_assert(!kMouseButtonNames.back().empty());
static_assert(!kGamepadButtonNames.back().empty());
static_assert(!kGamepadAxisNames.back().empty());

template <typename Enum, size_t N>
std::string_view lookupName(const std::array<std::string_view, N>& names, Enum value) {
    const auto index = static_cast<size_t>(value);
    return index < N ? names[index] : std::string_view("Unknown");
}

template <typename Enum, size_t N>
std::optional<Enum> lookupValue(const std::array<std::string_view, N>& names, std::string_view name,
                                size_t firstValid) {
    for (size_t index = firstValid; index < N; ++index) {
        if (equalsIgnoreCase(names[index], name)) {
            return static_cast<Enum>(index);
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view nameOf(Key key) {
    return lookupName(kKeyNames, key);
}

std::string_view nameOf(MouseButton button) {
    return lookupName(kMouseButtonNames, button);
}

std::string_view nameOf(GamepadButton button) {
    return lookupName(kGamepadButtonNames, button);
}

std::string_view nameOf(GamepadAxis axis) {
    return lookupName(kGamepadAxisNames, axis);
}

std::optional<Key> keyFromName(std::string_view name) {
    // Index 0 is Key::Unknown, which is not a bindable key.
    return lookupValue<Key>(kKeyNames, name, 1);
}

std::optional<MouseButton> mouseButtonFromName(std::string_view name) {
    return lookupValue<MouseButton>(kMouseButtonNames, name, 0);
}

std::optional<GamepadButton> gamepadButtonFromName(std::string_view name) {
    return lookupValue<GamepadButton>(kGamepadButtonNames, name, 0);
}

std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name) {
    return lookupValue<GamepadAxis>(kGamepadAxisNames, name, 0);
}

} // namespace cordite
