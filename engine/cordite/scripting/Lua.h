#pragma once

/// @file
/// The one place that includes Lua, the language gameplay scripts are written in.
///
/// Lua is built as C++ here (see cmake/CorditeDependencies.cmake): an error in a script
/// then leaves the functions of the engine it passes through the way an exception does,
/// and their destructors run. Its headers have to be read as C++ as well, which `lua.hpp` -
/// the header Lua offers to C++ programs - would prevent: it declares everything a C
/// function, and nothing would link. Going through this header keeps that from happening;
/// the module_layering test rejects a direct include.
///
/// @code
/// #include "cordite/scripting/Lua.h"
///
/// int pushAnswer(lua_State* state) {
///     lua_pushinteger(state, 42);
///     return 1;
/// }
/// @endcode
///
/// Lua's API is used inside ScriptRuntime::protect() only, or in a function that a script
/// calls: an error raised anywhere else has nowhere to go, and ends the program.

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
