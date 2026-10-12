#pragma once

/// @file
/// What a game calls its controls: actions and axes with names, and the keys, buttons
/// and sticks that move them.
///
/// A game asks whether "jump" was pressed and how far "moveX" is pushed, not whether the
/// space bar is down: which key that is, and that a gamepad does the same, is written
/// down once, in a file of the project - and can be changed there without touching the
/// game.
///
/// @code
/// CORDITE_TRY_ASSIGN(InputMap map, loadInputMap(project.assetDirectory() / "input/controls.input.json"));
/// InputActions actions;
/// actions.setMap(std::move(map));
///
/// // Every frame, after the events of the frame have been delivered to `input`:
/// actions.update(input);
/// if (actions.wasPressed("jump")) {
///     controller.jump = 4.0f;
/// }
/// controller.movement = Vec3(actions.axis("moveX"), 0.0f, -actions.axis("moveY")) * walkingSpeed;
/// @endcode
///
/// The file:
///
/// @code{.json}
/// {
///     "format": "cordite.input",
///     "version": 1,
///     "actions": [
///         // An action is down while any of its bindings is.
///         {"name": "jump", "bindings": [{"key": "Space"}, {"gamepadButton": "A"}]},
///         // A trigger or a stick is down when it is pushed as far as `threshold`: past
///         // 0.5 as given, and the other way for a threshold below zero.
///         {"name": "fire", "bindings": [{"mouseButton": "Left"}, {"gamepadAxis": "RightTrigger", "threshold": 0.3}]}
///     ],
///     "axes": [
///         // An axis has the value of whichever of its bindings is moved furthest. Two
///         // keys, or two buttons, give -1, 0 or 1; either may be left out.
///         {"name": "moveX", "bindings": [{"positiveKey": "D", "negativeKey": "A"},
///                                        {"positiveButton": "DpadRight", "negativeButton": "DpadLeft"},
///                                        {"gamepadAxis": "LeftX", "deadZone": 0.2}]},
///         // `scale` multiplies what a binding gives: -1 turns a stick round, whose Y
///         // points down. The pointer and the wheel give how far they went in the frame,
///         // in pixels and in notches, which a scale makes a speed of.
///         {"name": "moveY", "bindings": [{"gamepadAxis": "LeftY", "scale": -1}]},
///         {"name": "lookX", "bindings": [{"mouse": "x", "scale": 0.1}, {"gamepadAxis": "RightX", "scale": 2.5}]},
///         {"name": "zoom", "bindings": [{"scroll": "y"}]}
///     ]
/// }
/// @endcode
///
/// Keys, buttons and sticks have the names of KeyCodes.h. A gamepad binding is moved by
/// any gamepad that is connected: by the one that pushes furthest.

#include "cordite/core/Base.h"
#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/KeyCodes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cordite {

/// How far a trigger or a stick has to be pushed to press an action, where a binding
/// does not say.
inline constexpr float kDefaultActionThreshold = 0.5f;
/// How far a stick may be off its middle and still count as resting there, where a
/// binding does not say: no stick comes to rest at exactly nothing.
inline constexpr float kDefaultDeadZone = 0.15f;

/// A stick or a trigger that presses an action when it is pushed as far as `threshold`:
/// at or beyond it for one above zero, at or below it for one below. Never zero.
struct AxisThreshold {
    GamepadAxis axis = GamepadAxis::LeftX;
    float threshold = kDefaultActionThreshold;

    bool operator==(const AxisThreshold&) const = default;
};

/// What presses an action: a key, a button of the pointer, a button of a gamepad, or a
/// stick or trigger that is pushed far enough.
using ActionBinding = std::variant<Key, MouseButton, GamepadButton, AxisThreshold>;

/// Two keys that push an axis one way and the other. Key::Unknown for a side that has none.
struct KeyPair {
    Key positive = Key::Unknown;
    Key negative = Key::Unknown;

    bool operator==(const KeyPair&) const = default;
};

/// Two buttons of a gamepad that push an axis one way and the other; either may be
/// missing.
struct ButtonPair {
    bool hasPositive = false;
    GamepadButton positive = GamepadButton::A;
    bool hasNegative = false;
    GamepadButton negative = GamepadButton::A;

    bool operator==(const ButtonPair&) const = default;
};

/// A stick or a trigger: nothing within `deadZone` of its middle, and from there on up
/// to 1 at its end.
struct StickAxis {
    GamepadAxis axis = GamepadAxis::LeftX;
    /// From 0 to less than 1.
    float deadZone = kDefaultDeadZone;

    bool operator==(const StickAxis&) const = default;
};

/// Which of the two directions of the pointer or of the wheel.
enum class PointerAxis : uint8_t {
    X,
    Y,
};

/// How far the pointer went in the frame, in pixels: +X to the right, +Y down.
struct MouseMotion {
    PointerAxis axis = PointerAxis::X;

    bool operator==(const MouseMotion&) const = default;
};

/// How far the wheel was turned in the frame, in notches.
struct MouseScroll {
    PointerAxis axis = PointerAxis::Y;

    bool operator==(const MouseScroll&) const = default;
};

/// What moves an axis, and what that is multiplied by.
struct AxisBinding {
    std::variant<KeyPair, ButtonPair, StickAxis, MouseMotion, MouseScroll> source;
    /// Any number but none; negative to turn the binding round.
    float scale = 1.0f;

    bool operator==(const AxisBinding&) const = default;
};

/// Something a game can ask about by name that is pressed, held and released.
struct InputAction {
    std::string name;
    std::vector<ActionBinding> bindings;

    bool operator==(const InputAction&) const = default;
};

/// Something a game can ask about by name that has a value: how far, and which way.
struct InputAxis {
    std::string name;
    std::vector<AxisBinding> bindings;

    bool operator==(const InputAxis&) const = default;
};

/// The most actions, the most axes, and the most bindings of one of them that a map
/// holds: far more than a game has controls, and few enough that a file cannot ask for
/// memory without end.
inline constexpr size_t kMaxInputMapEntries = 1024;
inline constexpr size_t kMaxInputBindings = 64;

/// The controls of a game. Every name is there once among the actions and once among
/// the axes at most; an action and an axis may share one.
struct InputMap {
    std::vector<InputAction> actions;
    std::vector<InputAxis> axes;

    bool operator==(const InputMap&) const = default;
};

/// Reads a map from what an input file holds. Strict, as every file is read: fails as
/// readDocumentHeader() does for what is no input map of a version this engine reads,
/// and with ParseError - naming the action or axis, and the binding - for a member that is not
/// known, a name that is empty or there twice, a key, button or stick that has no such
/// name, a binding that names no control or more than one kind, a threshold of zero, a
/// dead zone that is not from 0 to less than 1, a scale of zero, numbers that are
/// none, and more entries than kMaxInputMapEntries or bindings than kMaxInputBindings.
[[nodiscard]] Result<InputMap> parseInputMap(const Json& json);

/// What parseInputMap() reads, with everything written out: a map that is read and
/// written again is the same file.
[[nodiscard]] Json serializeInputMap(const InputMap& map);

/// Reads an input file. Fails as readJsonFile() and parseInputMap() do, naming the file.
[[nodiscard]] Result<InputMap> loadInputMap(const std::filesystem::path& path);

/// Writes an input file.
[[nodiscard]] Status saveInputMap(const std::filesystem::path& path, const InputMap& map);

/// An action that was pressed or released in a frame.
struct ActionEvent {
    /// The name of the action, which lives as long as the map does.
    std::string_view action;
    bool pressed = false;

    bool operator==(const ActionEvent&) const = default;
};

/// What the actions and axes of a map are doing in the frame.
class InputActions {
public:
    InputActions() = default;
    ~InputActions() = default;
    /// What events() returns names the actions of the map in place.
    CORDITE_NON_MOVABLE(InputActions);

    /// Takes other controls. Everything is at rest until the next update() - in which
    /// what was held before does not count as pressed: only a key or a button of the
    /// pointer that goes down in that very frame does.
    void setMap(InputMap map);

    [[nodiscard]] const InputMap& map() const { return m_map; }

    /// Works out every action and axis from `input`: once per frame, after the events of
    /// the frame have been delivered.
    void update(const Input& input);

    [[nodiscard]] bool hasAction(std::string_view name) const;
    [[nodiscard]] bool hasAxis(std::string_view name) const;

    // What follows answers for a name that the map does not have as for an action that
    // is never pressed, and an axis that is never moved.

    /// Whether any binding of the action is held.
    [[nodiscard]] bool isDown(std::string_view action) const;
    /// Whether the action went down in this frame - also when it has come up again
    /// already: a key that is tapped between two frames was pressed, and released.
    [[nodiscard]] bool wasPressed(std::string_view action) const;
    [[nodiscard]] bool wasReleased(std::string_view action) const;

    /// What the binding of the axis that is moved furthest gives.
    [[nodiscard]] float axis(std::string_view axis) const;

    /// The actions that were pressed and released in this frame, in the order of the
    /// map; one that was tapped is pressed, then released.
    [[nodiscard]] std::span<const ActionEvent> events() const { return m_events; }

private:
    struct ActionState {
        bool down = false;
        bool pressed = false;
        bool released = false;
    };

    InputMap m_map;
    /// What each action and each axis of the map is doing, in the order of the map.
    std::vector<ActionState> m_actions;
    std::vector<float> m_axes;
    std::vector<ActionEvent> m_events;
    /// No update() has looked since setMap(): how things were before is not known, and
    /// what is held was not pressed for this map.
    bool m_fresh = true;
};

} // namespace cordite
