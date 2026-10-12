#pragma once

/// @file
/// The editor commands for the controls of a game: what they are, what they are doing,
/// and pressing them without a hand.
///
/// | Command        | Does                                                                   |
/// |----------------|------------------------------------------------------------------------|
/// | `input.map`    | Returns the actions and axes of the project, and what moves them.      |
/// | `input.state`  | Says what every action and axis was doing in the last frame.           |
/// | `input.inject` | Presses a key or a button, moves the pointer or the wheel, or sets a gamepad. |
///
/// @code
/// CORDITE_TRY(registerInputCommands(commands, engine.input(), engine.actions(), project.inputMapPath()));
///
/// unwrap(commands.execute("input.inject", {{"key", "Space"}, {"down", true}}));
/// // After the next frame:
/// const Json state = unwrap(commands.execute("input.state", Json::object()));
/// // state["actions"][0] == {"name": "jump", "down": true, "pressed": true, "released": false}
/// @endcode
///
/// What is injected is what the next frame sees, exactly as if a person had done it:
/// the interface of the editor as much as the actions of the game.

#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/InputMap.h"

#include <filesystem>

namespace cordite {

/// Registers the commands above. `mapFile` is where the map of `actions` was read from,
/// for `input.map` to name; empty for none. The registry must not be used after the
/// input or the actions are gone: the handlers refer to both.
[[nodiscard]] Status registerInputCommands(CommandRegistry& commands, Input& input, InputActions& actions,
                                           std::filesystem::path mapFile);

} // namespace cordite
