#include "cordite/platform/Input.h"

namespace cordite {
namespace {

/// Returns the index of a key or button, or the bitset size for out-of-range values so
/// callers can reject them with one comparison.
template <typename Enum>
size_t indexOf(Enum value) {
    return static_cast<size_t>(value);
}

bool isValidKey(Key key) {
    return key != Key::Unknown && indexOf(key) < indexOf(Key::Count);
}

bool isValidButton(MouseButton button) {
    return indexOf(button) < indexOf(MouseButton::Count);
}

void appendUtf8(std::string& text, uint32_t codepoint) {
    if (codepoint < 0x80) {
        text.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        text.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint < 0x10000) {
        text.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

} // namespace

void Input::beginFrame() {
    m_keysPressed.reset();
    m_keysReleased.reset();
    m_buttonsPressed.reset();
    m_buttonsReleased.reset();
    m_mouseDelta = Vec2(0.0f);
    m_scrollDelta = Vec2(0.0f);
    m_text.clear();
    m_events.clear();
}

bool Input::isKeyDown(Key key) const {
    return isValidKey(key) && m_keysDown.test(indexOf(key));
}

bool Input::wasKeyPressed(Key key) const {
    return isValidKey(key) && m_keysPressed.test(indexOf(key));
}

bool Input::wasKeyReleased(Key key) const {
    return isValidKey(key) && m_keysReleased.test(indexOf(key));
}

bool Input::isMouseButtonDown(MouseButton button) const {
    return isValidButton(button) && m_buttonsDown.test(indexOf(button));
}

bool Input::wasMouseButtonPressed(MouseButton button) const {
    return isValidButton(button) && m_buttonsPressed.test(indexOf(button));
}

bool Input::wasMouseButtonReleased(MouseButton button) const {
    return isValidButton(button) && m_buttonsReleased.test(indexOf(button));
}

KeyModifiers Input::modifiers() const {
    KeyModifiers result = KeyModifiers::None;
    if (isKeyDown(Key::LeftShift) || isKeyDown(Key::RightShift)) {
        result = result | KeyModifiers::Shift;
    }
    if (isKeyDown(Key::LeftControl) || isKeyDown(Key::RightControl)) {
        result = result | KeyModifiers::Control;
    }
    if (isKeyDown(Key::LeftAlt) || isKeyDown(Key::RightAlt)) {
        result = result | KeyModifiers::Alt;
    }
    if (isKeyDown(Key::LeftSuper) || isKeyDown(Key::RightSuper)) {
        result = result | KeyModifiers::Super;
    }
    return result;
}

bool Input::isGamepadConnected(uint32_t index) const {
    return index < kMaxGamepads && m_gamepadsConnected.test(index);
}

bool Input::isGamepadButtonDown(uint32_t index, GamepadButton button) const {
    return isGamepadConnected(index) && indexOf(button) < indexOf(GamepadButton::Count) &&
           m_gamepads[index].buttons.test(indexOf(button));
}

float Input::gamepadAxis(uint32_t index, GamepadAxis axis) const {
    if (!isGamepadConnected(index) || indexOf(axis) >= indexOf(GamepadAxis::Count)) {
        return 0.0f;
    }
    return m_gamepads[index].axes[indexOf(axis)];
}

void Input::injectKey(Key key, bool down, bool repeat) {
    if (!isValidKey(key)) {
        return;
    }

    const size_t index = indexOf(key);
    const bool wasDown = m_keysDown.test(index);

    InputEvent event;
    event.key = key;
    if (down) {
        if (wasDown && !repeat) {
            return;
        }
        if (!wasDown) {
            m_keysDown.set(index);
            m_keysPressed.set(index);
        }
        event.type = wasDown ? InputEventType::KeyRepeat : InputEventType::KeyDown;
    } else {
        if (!wasDown) {
            return;
        }
        m_keysDown.reset(index);
        m_keysReleased.set(index);
        event.type = InputEventType::KeyUp;
    }

    // Taken after the state update so that pressing Shift reports the Shift modifier.
    event.modifiers = modifiers();
    m_events.push_back(event);
}

void Input::injectMouseButton(MouseButton button, bool down) {
    if (!isValidButton(button)) {
        return;
    }

    const size_t index = indexOf(button);
    if (m_buttonsDown.test(index) == down) {
        return;
    }

    m_buttonsDown.set(index, down);
    if (down) {
        m_buttonsPressed.set(index);
    } else {
        m_buttonsReleased.set(index);
    }

    InputEvent event;
    event.type = down ? InputEventType::MouseButtonDown : InputEventType::MouseButtonUp;
    event.mouseButton = button;
    event.position = m_mousePosition;
    event.modifiers = modifiers();
    m_events.push_back(event);
}

void Input::injectMouseMove(Vec2 position) {
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
        return;
    }

    if (m_hasMousePosition) {
        m_mouseDelta += position - m_mousePosition;
    }
    m_mousePosition = position;
    m_hasMousePosition = true;

    InputEvent event;
    event.type = InputEventType::MouseMove;
    event.position = position;
    event.modifiers = modifiers();
    m_events.push_back(event);
}

void Input::injectScroll(Vec2 delta) {
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y)) {
        return;
    }

    m_scrollDelta += delta;

    InputEvent event;
    event.type = InputEventType::MouseScroll;
    event.position = delta;
    event.modifiers = modifiers();
    m_events.push_back(event);
}

void Input::injectText(uint32_t codepoint) {
    const bool isSurrogate = codepoint >= 0xd800 && codepoint <= 0xdfff;
    if (codepoint == 0 || codepoint > 0x10ffff || isSurrogate) {
        return;
    }

    appendUtf8(m_text, codepoint);

    InputEvent event;
    event.type = InputEventType::Text;
    event.codepoint = codepoint;
    event.modifiers = modifiers();
    m_events.push_back(event);
}

void Input::injectGamepad(uint32_t index, const GamepadState* state) {
    if (index >= kMaxGamepads) {
        return;
    }

    if (state == nullptr) {
        m_gamepadsConnected.reset(index);
        m_gamepads[index] = GamepadState{};
        return;
    }
    m_gamepadsConnected.set(index);
    m_gamepads[index] = *state;
}

void Input::releaseAll() {
    for (size_t index = 0; index < m_keysDown.size(); ++index) {
        if (m_keysDown.test(index)) {
            injectKey(static_cast<Key>(index), false);
        }
    }
    for (size_t index = 0; index < m_buttonsDown.size(); ++index) {
        if (m_buttonsDown.test(index)) {
            injectMouseButton(static_cast<MouseButton>(index), false);
        }
    }
}

} // namespace cordite
