#pragma once

/// @file
/// Identifiers for keyboard keys, mouse buttons and gamepad controls.
///
/// Keys name physical positions on a US layout, so WASD stays under the left hand on
/// every keyboard layout. Use Input::text() for the characters a user typed.
///
/// Every identifier has a stable name for scripts, key bindings and settings files:
///
/// @code
/// const std::optional<Key> jump = keyFromName("Space");
/// CORDITE_LOG_INFO("Input", "jump is bound to {}", nameOf(*jump));
/// @endcode

#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

enum class Key : uint8_t {
    Unknown,

    Space,
    Apostrophe,
    Comma,
    Minus,
    Period,
    Slash,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Semicolon,
    Equal,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    LeftBracket,
    Backslash,
    RightBracket,
    GraveAccent,

    Escape,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp,
    PageDown,
    Home,
    End,
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,

    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    Keypad0,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
    KeypadDecimal,
    KeypadDivide,
    KeypadMultiply,
    KeypadSubtract,
    KeypadAdd,
    KeypadEnter,
    KeypadEqual,

    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,

    Count,
};

enum class MouseButton : uint8_t {
    Left,
    Right,
    Middle,
    Button4,
    Button5,

    Count,
};

/// Buttons of a standard (Xbox-style) gamepad.
enum class GamepadButton : uint8_t {
    A,
    B,
    X,
    Y,
    LeftBumper,
    RightBumper,
    Back,
    Start,
    Guide,
    LeftThumb,
    RightThumb,
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,

    Count,
};

/// Axes of a standard gamepad. Sticks range over [-1, 1] with +Y pointing down; triggers
/// range over [0, 1].
enum class GamepadAxis : uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,

    Count,
};

/// Modifier keys held while an input event happened. Combine with `|`.
enum class KeyModifiers : uint8_t {
    None = 0,
    Shift = 1 << 0,
    Control = 1 << 1,
    Alt = 1 << 2,
    Super = 1 << 3,
};

constexpr KeyModifiers operator|(KeyModifiers a, KeyModifiers b) noexcept {
    return static_cast<KeyModifiers>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// True when `value` contains every modifier in `flags`.
constexpr bool hasModifiers(KeyModifiers value, KeyModifiers flags) noexcept {
    return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flags)) == static_cast<uint8_t>(flags);
}

/// Stable identifier of each value, e.g. "LeftShift", "Middle", "DpadUp", "LeftTrigger".
std::string_view nameOf(Key key);
std::string_view nameOf(MouseButton button);
std::string_view nameOf(GamepadButton button);
std::string_view nameOf(GamepadAxis axis);

/// Inverse of nameOf, ignoring letter case. Returns nullopt for unknown names; "Unknown"
/// and "Count" are not valid names.
std::optional<Key> keyFromName(std::string_view name);
std::optional<MouseButton> mouseButtonFromName(std::string_view name);
std::optional<GamepadButton> gamepadButtonFromName(std::string_view name);
std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name);

} // namespace cordite
