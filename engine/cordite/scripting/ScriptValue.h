#pragma once

/// @file
/// For the sources of the scripting module, and for tests of them: values on their way
/// between the engine, which describes its data as JSON (Reflection.h), and Lua.
///
/// @code
/// // In a function that a script calls, or inside ScriptRuntime::protect():
/// pushScriptValue(state, Json{{"name", "Lamp"}, {"color", {1.0, 0.5, 0.25}}});   // a table
/// CORDITE_TRY_ASSIGN(const Json back, readScriptValue(state, -1));
/// @endcode
///
/// How the two kinds of value correspond:
///
/// | JSON                | Lua                                                        |
/// |---------------------|------------------------------------------------------------|
/// | `null`              | `nil` - but `cordite.null` inside an array, see below      |
/// | `true`, `false`     | a boolean                                                  |
/// | a whole number      | an integer                                                 |
/// | any other number    | a float                                                    |
/// | a string            | a string                                                   |
/// | an array            | a table whose keys are 1 to n                              |
/// | an object           | a table whose keys are names                               |
///
/// A member of an object that is `null` is left out of the table, so that asking the
/// table for it gives `nil`, as a script expects of what is not there. Inside an array
/// that would not do - a `nil` ends a Lua list - so there `null` becomes the value scripts
/// know as `cordite.null`, and that value is read back as `null` wherever it stands.
///
/// On the way back a table has to be one or the other: one whose keys are exactly 1 to n
/// is an array, one whose keys are all strings an object (its members sorted by name, so
/// that the same table always gives the same JSON), and one without keys an empty object.
/// Metatables are not looked at.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <cstdint>

struct lua_State;

namespace cordite {

/// How deep the tables of a value that is read may be nested. One that holds itself has
/// no end, and is refused for being deeper.
inline constexpr uint32_t kMaxScriptValueDepth = 32;

/// Pushes the value that stands for JSON's `null` inside a list. Raises nothing.
void pushScriptNull(lua_State* state);

/// Whether the value at `index` is that one.
[[nodiscard]] bool isScriptNull(lua_State* state, int index);

/// Pushes `value` onto the stack, as the table above says. Raises a Lua error when the
/// scripts are out of memory or of stack, as most of Lua's own API does.
void pushScriptValue(lua_State* state, const Json& value);

/// Reads the value at `index` as JSON, leaving the stack as it is. Fails with
/// InvalidArgument for what JSON cannot hold: a function, a thread or an object of the
/// engine; a number that is not finite; a table with other keys than 1 to n or names, or
/// with tables in it more than kMaxScriptValueDepth deep. Raises a Lua error only when
/// the scripts are out of stack.
[[nodiscard]] Result<Json> readScriptValue(lua_State* state, int index);

} // namespace cordite
