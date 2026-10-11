#include "cordite/platform/KeyCodes.h"

#include "support/TestSupport.h"

#include <set>
#include <string>

using namespace cordite;

TEST_SUITE("KeyCodes") {
    TEST_CASE("every key has a unique name that maps back to it") {
        std::set<std::string> names;
        for (size_t index = 1; index < static_cast<size_t>(Key::Count); ++index) {
            const Key key = static_cast<Key>(index);
            const std::string name(nameOf(key));
            REQUIRE_FALSE(name.empty());
            CHECK(names.insert(name).second);

            const std::optional<Key> parsed = keyFromName(name);
            REQUIRE(parsed.has_value());
            CHECK(*parsed == key);
        }
        CHECK(names.size() == static_cast<size_t>(Key::Count) - 1);
    }

    TEST_CASE("key names are the documented identifiers") {
        CHECK(nameOf(Key::Space) == "Space");
        CHECK(nameOf(Key::A) == "A");
        CHECK(nameOf(Key::Z) == "Z");
        CHECK(nameOf(Key::Num0) == "Num0");
        CHECK(nameOf(Key::Num9) == "Num9");
        CHECK(nameOf(Key::F1) == "F1");
        CHECK(nameOf(Key::F12) == "F12");
        CHECK(nameOf(Key::Keypad9) == "Keypad9");
        CHECK(nameOf(Key::KeypadEqual) == "KeypadEqual");
        CHECK(nameOf(Key::LeftShift) == "LeftShift");
        CHECK(nameOf(Key::Menu) == "Menu");
        CHECK(nameOf(Key::Unknown) == "Unknown");
    }

    TEST_CASE("names are matched without regard to case") {
        CHECK(keyFromName("space") == Key::Space);
        CHECK(keyFromName("LEFTSHIFT") == Key::LeftShift);
        CHECK(mouseButtonFromName("middle") == MouseButton::Middle);
        CHECK(gamepadButtonFromName("dpadup") == GamepadButton::DpadUp);
        CHECK(gamepadAxisFromName("LEFTTRIGGER") == GamepadAxis::LeftTrigger);
    }

    TEST_CASE("unknown names and the sentinel names are rejected") {
        CHECK_FALSE(keyFromName("").has_value());
        CHECK_FALSE(keyFromName("Unknown").has_value());
        CHECK_FALSE(keyFromName("Count").has_value());
        CHECK_FALSE(keyFromName("Spacebar").has_value());
        CHECK_FALSE(keyFromName(" A").has_value());
        CHECK_FALSE(mouseButtonFromName("Button6").has_value());
        CHECK_FALSE(gamepadButtonFromName("Z").has_value());
        CHECK_FALSE(gamepadAxisFromName("LeftZ").has_value());
    }

    TEST_CASE("out-of-range values have a safe name") {
        CHECK(nameOf(Key::Count) == "Unknown");
        CHECK(nameOf(static_cast<Key>(250)) == "Unknown");
        CHECK(nameOf(MouseButton::Count) == "Unknown");
    }

    TEST_CASE("mouse buttons, gamepad buttons and axes round-trip") {
        for (size_t index = 0; index < static_cast<size_t>(MouseButton::Count); ++index) {
            const auto button = static_cast<MouseButton>(index);
            CHECK(mouseButtonFromName(nameOf(button)) == button);
        }
        for (size_t index = 0; index < static_cast<size_t>(GamepadButton::Count); ++index) {
            const auto button = static_cast<GamepadButton>(index);
            CHECK(gamepadButtonFromName(nameOf(button)) == button);
        }
        for (size_t index = 0; index < static_cast<size_t>(GamepadAxis::Count); ++index) {
            const auto axis = static_cast<GamepadAxis>(index);
            CHECK(gamepadAxisFromName(nameOf(axis)) == axis);
        }
    }

    TEST_CASE("modifier flags combine and test as sets") {
        const KeyModifiers both = KeyModifiers::Shift | KeyModifiers::Control;
        CHECK(hasModifiers(both, KeyModifiers::Shift));
        CHECK(hasModifiers(both, KeyModifiers::Control));
        CHECK(hasModifiers(both, both));
        CHECK_FALSE(hasModifiers(both, KeyModifiers::Alt));
        CHECK_FALSE(hasModifiers(KeyModifiers::Shift, both));
        CHECK(hasModifiers(both, KeyModifiers::None));
    }
}
