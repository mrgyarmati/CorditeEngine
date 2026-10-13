#pragma once

/// @file
/// The editor commands about gameplay scripts: what a script can call, and what the engine
/// calls in a script.
///
/// | Command      | Does                                                                        |
/// |--------------|-----------------------------------------------------------------------------|
/// | `script.api` | Returns every function scripts are given and every function the engine calls in one. |
///
/// @code
/// CORDITE_TRY(registerScriptCommands(commands, engine.scripts()));
///
/// const Json api = unwrap(commands.execute("script.api", {{"prefix", "cordite.physics."}}));
/// // api["functions"][0] == {"name": "cordite.physics.raycast",
/// //                         "signature": "cordite.physics.raycast(origin, direction, reach) -> hit or nil",
/// //                         "description": "The first body a ray hits within ..."}
/// // api["callbacks"][2] == {"name": "update", "signature": "function Script:update(deltaSeconds)", ...}
/// @endcode
///
/// It is the catalogue the script runtime keeps (ScriptRuntime::functions(),
/// ScriptSystem::callbacks()): what is said here is what is there, and whoever writes a
/// script - a person, or an agent - need not read the engine to find out.

#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/scripting/ScriptSystem.h"

namespace cordite {

/// Registers the commands above. The registry must not be used after the script system
/// is gone: the handlers refer to it.
[[nodiscard]] Status registerScriptCommands(CommandRegistry& commands, ScriptSystem& scripts);

} // namespace cordite
