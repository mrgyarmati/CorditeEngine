#include "cordite/platform/Input.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <iterator>
#include <limits>
#include <string>

using namespace cordite;

TEST_SUITE("Input") {
    TEST_CASE("a key press is an edge for one frame and a level until released") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::W, true);

        CHECK(input.isKeyDown(Key::W));
        CHECK(input.wasKeyPressed(Key::W));
        CHECK_FALSE(input.wasKeyReleased(Key::W));
        CHECK_FALSE(input.isKeyDown(Key::S));

        input.beginFrame();
        CHECK(input.isKeyDown(Key::W));
        CHECK_FALSE(input.wasKeyPressed(Key::W));

        input.injectKey(Key::W, false);
        CHECK_FALSE(input.isKeyDown(Key::W));
        CHECK(input.wasKeyReleased(Key::W));

        input.beginFrame();
        CHECK_FALSE(input.wasKeyReleased(Key::W));
    }

    TEST_CASE("a press and release within one frame reports both edges") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::Space, true);
        input.injectKey(Key::Space, false);

        CHECK(input.wasKeyPressed(Key::Space));
        CHECK(input.wasKeyReleased(Key::Space));
        CHECK_FALSE(input.isKeyDown(Key::Space));
    }

    TEST_CASE("redundant key events are ignored unless marked as repeats") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::A, true);
        input.injectKey(Key::A, true);
        input.injectKey(Key::B, false);
        REQUIRE(input.events().size() == 1);
        CHECK(input.events()[0].type == InputEventType::KeyDown);

        input.injectKey(Key::A, true, true);
        REQUIRE(input.events().size() == 2);
        CHECK(input.events()[1].type == InputEventType::KeyRepeat);

        // A repeat is not a new press.
        input.beginFrame();
        input.injectKey(Key::A, true, true);
        CHECK_FALSE(input.wasKeyPressed(Key::A));
        CHECK(input.isKeyDown(Key::A));
    }

    TEST_CASE("invalid keys and buttons are ignored") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::Unknown, true);
        input.injectKey(Key::Count, true);
        input.injectKey(static_cast<Key>(255), true);
        input.injectMouseButton(MouseButton::Count, true);

        CHECK(input.events().empty());
        CHECK_FALSE(input.isKeyDown(Key::Unknown));
        CHECK_FALSE(input.isKeyDown(Key::Count));
        CHECK_FALSE(input.isKeyDown(static_cast<Key>(255)));
        CHECK_FALSE(input.isMouseButtonDown(MouseButton::Count));
    }

    TEST_CASE("modifiers follow the modifier keys and are recorded on events") {
        Input input;
        input.beginFrame();
        CHECK(input.modifiers() == KeyModifiers::None);

        input.injectKey(Key::LeftShift, true);
        input.injectKey(Key::RightControl, true);
        CHECK(hasModifiers(input.modifiers(), KeyModifiers::Shift | KeyModifiers::Control));
        CHECK_FALSE(hasModifiers(input.modifiers(), KeyModifiers::Alt));

        input.injectKey(Key::S, true);
        const InputEvent& event = input.events().back();
        CHECK(event.key == Key::S);
        CHECK(hasModifiers(event.modifiers, KeyModifiers::Shift | KeyModifiers::Control));

        // The Shift key's own event already carries Shift.
        CHECK(hasModifiers(input.events()[0].modifiers, KeyModifiers::Shift));

        input.injectKey(Key::LeftShift, false);
        CHECK_FALSE(hasModifiers(input.modifiers(), KeyModifiers::Shift));
    }

    TEST_CASE("mouse buttons report edges and the cursor position") {
        Input input;
        input.beginFrame();
        input.injectMouseMove(Vec2(100.0f, 50.0f));
        input.injectMouseButton(MouseButton::Left, true);

        CHECK(input.isMouseButtonDown(MouseButton::Left));
        CHECK(input.wasMouseButtonPressed(MouseButton::Left));
        CHECK_FALSE(input.isMouseButtonDown(MouseButton::Right));
        CHECK_NEAR(input.events().back().position, Vec2(100.0f, 50.0f), 0.0f);

        input.beginFrame();
        CHECK_FALSE(input.wasMouseButtonPressed(MouseButton::Left));
        input.injectMouseButton(MouseButton::Left, false);
        CHECK(input.wasMouseButtonReleased(MouseButton::Left));
        CHECK_FALSE(input.isMouseButtonDown(MouseButton::Left));
    }

    TEST_CASE("mouse movement accumulates into a per-frame delta") {
        Input input;
        input.beginFrame();
        // The first position has nothing to be relative to.
        input.injectMouseMove(Vec2(10.0f, 10.0f));
        CHECK_NEAR(input.mouseDelta(), Vec2(0.0f), 0.0f);

        input.injectMouseMove(Vec2(15.0f, 12.0f));
        input.injectMouseMove(Vec2(18.0f, 7.0f));
        CHECK_NEAR(input.mousePosition(), Vec2(18.0f, 7.0f), 0.0f);
        CHECK_NEAR(input.mouseDelta(), Vec2(8.0f, -3.0f), 1e-5f);

        input.beginFrame();
        CHECK_NEAR(input.mouseDelta(), Vec2(0.0f), 0.0f);
        CHECK_NEAR(input.mousePosition(), Vec2(18.0f, 7.0f), 0.0f);
    }

    TEST_CASE("resetMouseTracking suppresses the jump after a cursor mode change") {
        Input input;
        input.beginFrame();
        input.injectMouseMove(Vec2(10.0f, 10.0f));
        input.resetMouseTracking();
        input.injectMouseMove(Vec2(500.0f, 400.0f));
        CHECK_NEAR(input.mouseDelta(), Vec2(0.0f), 0.0f);

        input.injectMouseMove(Vec2(501.0f, 400.0f));
        CHECK_NEAR(input.mouseDelta(), Vec2(1.0f, 0.0f), 1e-5f);
    }

    TEST_CASE("scroll accumulates within a frame") {
        Input input;
        input.beginFrame();
        input.injectScroll(Vec2(0.0f, 1.0f));
        input.injectScroll(Vec2(0.5f, 2.0f));
        CHECK_NEAR(input.scrollDelta(), Vec2(0.5f, 3.0f), 1e-6f);

        input.beginFrame();
        CHECK_NEAR(input.scrollDelta(), Vec2(0.0f), 0.0f);
    }

    TEST_CASE("non-finite mouse input is ignored") {
        Input input;
        input.beginFrame();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        input.injectMouseMove(Vec2(nan, 0.0f));
        input.injectMouseMove(Vec2(0.0f, infinity));
        input.injectScroll(Vec2(nan, nan));

        CHECK(input.events().empty());
        CHECK_NEAR(input.mousePosition(), Vec2(0.0f), 0.0f);
        CHECK_NEAR(input.scrollDelta(), Vec2(0.0f), 0.0f);
    }

    TEST_CASE("typed text is collected as UTF-8") {
        Input input;
        input.beginFrame();
        input.injectText('H');
        input.injectText('i');
        input.injectText(0xe9);    // e with acute accent: two bytes
        input.injectText(0x20ac);  // euro sign: three bytes
        input.injectText(0x1f600); // an emoji: four bytes

        CHECK(input.text() == "Hi\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
        CHECK(input.events().size() == 5);
        CHECK(input.events()[2].type == InputEventType::Text);
        CHECK(input.events()[2].codepoint == 0xe9);

        input.beginFrame();
        CHECK(input.text().empty());
    }

    TEST_CASE("invalid code points are not turned into text") {
        Input input;
        input.beginFrame();
        input.injectText(0);
        input.injectText(0xd800); // a lone surrogate
        input.injectText(0xdfff);
        input.injectText(0x110000); // beyond the last code point

        CHECK(input.text().empty());
        CHECK(input.events().empty());

        input.injectText(0x10ffff);
        CHECK(input.text().size() == 4);
    }

    TEST_CASE("events are recorded in the order they happened") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::E, true);
        input.injectMouseMove(Vec2(1.0f, 2.0f));
        input.injectMouseButton(MouseButton::Right, true);
        input.injectScroll(Vec2(0.0f, -1.0f));
        input.injectText('e');
        input.injectKey(Key::E, false);

        const InputEventType expected[] = {
            InputEventType::KeyDown,     InputEventType::MouseMove, InputEventType::MouseButtonDown,
            InputEventType::MouseScroll, InputEventType::Text,      InputEventType::KeyUp,
        };
        REQUIRE(input.events().size() == std::size(expected));
        for (size_t index = 0; index < std::size(expected); ++index) {
            CHECK(input.events()[index].type == expected[index]);
        }
        CHECK(input.events()[2].mouseButton == MouseButton::Right);
        CHECK_NEAR(input.events()[3].position, Vec2(0.0f, -1.0f), 0.0f);
    }

    TEST_CASE("releaseAll lifts every held key and button") {
        Input input;
        input.beginFrame();
        input.injectKey(Key::W, true);
        input.injectKey(Key::LeftShift, true);
        input.injectMouseButton(MouseButton::Left, true);

        input.beginFrame();
        input.releaseAll();

        CHECK_FALSE(input.isKeyDown(Key::W));
        CHECK_FALSE(input.isKeyDown(Key::LeftShift));
        CHECK_FALSE(input.isMouseButtonDown(MouseButton::Left));
        CHECK(input.wasKeyReleased(Key::W));
        CHECK(input.wasMouseButtonReleased(MouseButton::Left));
        CHECK(input.events().size() == 3);
        CHECK(input.modifiers() == KeyModifiers::None);

        // Nothing is held, so a second call does nothing.
        input.beginFrame();
        input.releaseAll();
        CHECK(input.events().empty());
    }

    TEST_CASE("gamepads report state only while connected") {
        Input input;
        CHECK_FALSE(input.isGamepadConnected(0));
        CHECK_FALSE(input.isGamepadButtonDown(0, GamepadButton::A));
        CHECK(input.gamepadAxis(0, GamepadAxis::LeftX) == 0.0f);

        GamepadState state;
        state.buttons.set(static_cast<size_t>(GamepadButton::A));
        state.axes[static_cast<size_t>(GamepadAxis::LeftX)] = -0.75f;
        state.axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = 1.0f;
        input.injectGamepad(1, &state);

        CHECK(input.isGamepadConnected(1));
        CHECK_FALSE(input.isGamepadConnected(0));
        CHECK(input.isGamepadButtonDown(1, GamepadButton::A));
        CHECK_FALSE(input.isGamepadButtonDown(1, GamepadButton::B));
        CHECK(input.gamepadAxis(1, GamepadAxis::LeftX) == -0.75f);
        CHECK(input.gamepadAxis(1, GamepadAxis::RightTrigger) == 1.0f);

        input.injectGamepad(1, nullptr);
        CHECK_FALSE(input.isGamepadConnected(1));
        CHECK_FALSE(input.isGamepadButtonDown(1, GamepadButton::A));
        CHECK(input.gamepadAxis(1, GamepadAxis::LeftX) == 0.0f);
    }

    TEST_CASE("out-of-range gamepad queries are safe") {
        Input input;
        GamepadState state;
        state.buttons.set();
        input.injectGamepad(Input::kMaxGamepads, &state);
        input.injectGamepad(0, &state);

        CHECK_FALSE(input.isGamepadConnected(Input::kMaxGamepads));
        CHECK_FALSE(input.isGamepadButtonDown(Input::kMaxGamepads, GamepadButton::A));
        CHECK_FALSE(input.isGamepadButtonDown(0, GamepadButton::Count));
        CHECK(input.gamepadAxis(0, GamepadAxis::Count) == 0.0f);
        CHECK(input.gamepadAxis(99, GamepadAxis::LeftX) == 0.0f);
    }
}
