#pragma once

/// @file
/// The commands through which scripts and AI agents edit a scene.
///
/// registerEditorCommands() fills a CommandRegistry with everything that can be done to
/// an EditorContext: creating, changing and deleting entities and components, selecting,
/// undoing, and loading and saving scenes. Each command is a thin JSON front for the
/// corresponding EditorActions function, so what a command does is exactly what the
/// editor's menus do, undo included.
///
/// @code
/// CommandRegistry commands;
/// CORDITE_TRY(registerEditorCommands(commands, context));
///
/// CORDITE_TRY_ASSIGN(const Json created, commands.execute("entity.create", {{"name", "Eye"}}));
/// CORDITE_TRY(commands.execute("component.add", {{"entity", created["id"]}, {"component", "Camera"}}));
/// CORDITE_TRY(commands.execute("editor.undo"));
/// @endcode
///
/// Commands refer to entities the way EditorContext::resolveEntity understands: by id,
/// by path ("Room/Table/Lamp") or by name. Results describe entities with their id and
/// path, so a caller can keep referring to what it just made. `editor.commands` returns
/// the catalogue of all commands, and `component.types` describes every component and
/// its properties.

#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/editor/EditorContext.h"

namespace cordite {

/// Adds the scene-editing commands to `commands`. Both arguments must outlive the
/// registered handlers. Fails when a command name is already taken.
[[nodiscard]] Status registerEditorCommands(CommandRegistry& commands, EditorContext& context);

} // namespace cordite
