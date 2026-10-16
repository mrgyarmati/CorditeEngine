#pragma once

/// @file
/// The reference pages of the user guide (docs/reference), written by the engine itself:
/// every component with every property, every function scripts can call and every one the
/// engine calls in a script, every command of the editor, and the names of the controls an
/// input map binds. A page that is made of what the engine has cannot leave anything out; a
/// test writes each again and compares it with the page in the repository, so neither can
/// fall behind the other.
///
/// Each page is Markdown, the same on every system and every run: what is listed is in a
/// fixed order, and numbers are written as files write them.
///
/// @code
/// CORDITE_TRY(writeTextFile(directory / "components.md", componentReference(engine.components())));
/// CORDITE_TRY(writeTextFile(directory / "script-api.md", scriptReference(engine.scripts())));
/// CORDITE_TRY(writeTextFile(directory / "editor-commands.md", commandReference(editor.commands())));
/// CORDITE_TRY(writeTextFile(directory / "input.md", inputReference()));
/// @endcode

#include <string>

namespace cordite {

class CommandRegistry;
class ComponentRegistry;
class ScriptSystem;

/// Every component of `components`, by name, with what it is for and a table of its
/// properties: type, default, the values it takes and what it means.
[[nodiscard]] std::string componentReference(const ComponentRegistry& components);

/// The functions the engine calls in a script, and every function scripts can call,
/// library by library, in the order of the catalogue (ScriptRuntime::functions()).
[[nodiscard]] std::string scriptReference(const ScriptSystem& scripts);

/// Every command of `commands`, by name, with what it does and its parameters.
[[nodiscard]] std::string commandReference(const CommandRegistry& commands);

/// The names an input map binds controls by: keys, mouse buttons, gamepad buttons and
/// sticks, and the directions of the pointer.
[[nodiscard]] std::string inputReference();

} // namespace cordite
