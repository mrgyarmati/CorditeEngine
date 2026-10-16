#pragma once

/// @file
/// Keyboard, mouse and gamepad state for one window, and the files dropped onto it.
///
/// Input is updated once per frame: WindowSystem::pollEvents() calls beginFrame() and
/// then delivers the operating system's events. Between two polls the state is constant,
/// so every system in a frame sees the same answers.
///
/// @code
/// const Input& input = window.input();
/// if (input.wasKeyPressed(Key::Space)) { jump(); }
/// const Vec2 look = input.mouseDelta() * sensitivity;
/// const float throttle = input.gamepadAxis(0, GamepadAxis::RightTrigger);
/// @endcode
///
/// The inject functions feed events in directly. The window backend uses them, and so
/// do tests and editor automation, which makes scripted input indistinguishable from
/// real input.

#include "cordite/math/Math.h"
#include "cordite/platform/KeyCodes.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

enum class InputEventType : uint8_t {
    KeyDown,
    /// The operating system's key repeat while a key is held.
    KeyRepeat,
    KeyUp,
    MouseButtonDown,
    MouseButtonUp,
    MouseMove,
    MouseScroll,
    /// A Unicode character was typed.
    Text,
};

/// One input event, in the order it occurred. Which members are meaningful depends on
/// the type.
struct InputEvent {
    InputEventType type = InputEventType::KeyDown;
    /// KeyDown, KeyRepeat, KeyUp.
    Key key = Key::Unknown;
    /// MouseButtonDown, MouseButtonUp.
    MouseButton mouseButton = MouseButton::Left;
    /// Cursor position for mouse events; the scroll amount for MouseScroll.
    Vec2 position{0.0f};
    /// Text: the Unicode code point.
    uint32_t codepoint = 0;
    /// Modifiers held when the event happened.
    KeyModifiers modifiers = KeyModifiers::None;
};

/// The controls of one gamepad at an instant.
struct GamepadState {
    std::bitset<static_cast<size_t>(GamepadButton::Count)> buttons;
    std::array<float, static_cast<size_t>(GamepadAxis::Count)> axes{};
};

class Input {
public:
    static constexpr uint32_t kMaxGamepads = 4;

    /// Starts a new frame: forgets pressed/released transitions, movement deltas, typed
    /// text and the event list. Held keys and buttons stay held.
    void beginFrame();

    [[nodiscard]] bool isKeyDown(Key key) const;
    /// True during the one frame in which the key went down.
    [[nodiscard]] bool wasKeyPressed(Key key) const;
    /// True during the one frame in which the key went up.
    [[nodiscard]] bool wasKeyReleased(Key key) const;

    [[nodiscard]] bool isMouseButtonDown(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonPressed(MouseButton button) const;
    [[nodiscard]] bool wasMouseButtonReleased(MouseButton button) const;

    /// Cursor position in window pixels, origin top-left.
    [[nodiscard]] Vec2 mousePosition() const { return m_mousePosition; }

    /// Cursor movement during this frame.
    [[nodiscard]] Vec2 mouseDelta() const { return m_mouseDelta; }

    /// Scroll wheel movement during this frame; +Y scrolls away from the user.
    [[nodiscard]] Vec2 scrollDelta() const { return m_scrollDelta; }

    /// Modifier keys currently held, derived from the shift/control/alt/super key state.
    [[nodiscard]] KeyModifiers modifiers() const;

    /// Characters typed during this frame, as UTF-8.
    [[nodiscard]] std::string_view text() const { return m_text; }

    /// Files dropped onto the window from outside during this frame - from a file manager -
    /// as the operating system named them, in the order they came.
    [[nodiscard]] std::span<const std::filesystem::path> droppedFiles() const { return m_droppedFiles; }

    /// Every event of this frame, in order.
    [[nodiscard]] std::span<const InputEvent> events() const { return m_events; }

    /// Gamepad queries return false / 0 for an index that is out of range or not connected.
    [[nodiscard]] bool isGamepadConnected(uint32_t index) const;
    [[nodiscard]] bool isGamepadButtonDown(uint32_t index, GamepadButton button) const;
    [[nodiscard]] float gamepadAxis(uint32_t index, GamepadAxis axis) const;

    /// Sets a key's state. Injecting "down" for a key that is already down records a
    /// KeyRepeat event when `repeat` is set and is ignored otherwise.
    void injectKey(Key key, bool down, bool repeat = false);
    void injectMouseButton(MouseButton button, bool down);
    void injectMouseMove(Vec2 position);
    void injectScroll(Vec2 delta);
    /// Appends a typed character. Invalid code points (surrogates, above U+10FFFF) are ignored.
    void injectText(uint32_t codepoint);
    /// Connects a gamepad with the given state, or disconnects it with nullptr.
    void injectGamepad(uint32_t index, const GamepadState* state);
    /// Files dropped onto the window, added to what droppedFiles() lists this frame.
    void injectDrop(std::span<const std::filesystem::path> files);

    /// Releases every held key and mouse button, generating the matching up events. Call
    /// when the window loses focus so nothing stays stuck down.
    void releaseAll();

    /// Forgets the last cursor position so the next move produces no delta. Call when
    /// the cursor mode changes, which makes the position jump.
    void resetMouseTracking() { m_hasMousePosition = false; }

private:
    using KeySet = std::bitset<static_cast<size_t>(Key::Count)>;
    using MouseButtonSet = std::bitset<static_cast<size_t>(MouseButton::Count)>;

    KeySet m_keysDown;
    KeySet m_keysPressed;
    KeySet m_keysReleased;
    MouseButtonSet m_buttonsDown;
    MouseButtonSet m_buttonsPressed;
    MouseButtonSet m_buttonsReleased;

    Vec2 m_mousePosition{0.0f};
    Vec2 m_mouseDelta{0.0f};
    Vec2 m_scrollDelta{0.0f};
    bool m_hasMousePosition = false;

    std::string m_text;
    std::vector<std::filesystem::path> m_droppedFiles;
    std::vector<InputEvent> m_events;

    std::array<GamepadState, kMaxGamepads> m_gamepads{};
    std::bitset<kMaxGamepads> m_gamepadsConnected;
};

} // namespace cordite
