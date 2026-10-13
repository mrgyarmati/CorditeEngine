#pragma once

/// @file
/// For the sources of the scripting module alone: what the functions that scripts call
/// share - how they find the world, and how an entity crosses between C++ and Lua.
///
/// @code
/// int entityName(lua_State* state) {
///     const Entity entity = checkEntity(state, 1);             // raises for anything else, or one that is gone
///     const std::string& name = checkContext(state).world.name(entity);
///     lua_pushlstring(state, name.data(), name.size());
///     return 1;
/// }
/// @endcode
///
/// To a script an entity is a value of its own kind, with the functions of
/// `cordite.Entity` as its methods: `door:name()`. It is the same value every time for
/// the same entity, so two of them are equal exactly when they are the same entity, and
/// they serve as keys of tables. The value outlives its entity; what is done with it
/// then is an error that says so, and `isValid()` is how a script asks first.

#include "cordite/core/Result.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"

struct lua_State;

namespace cordite {

/// The world of the update that is under way. Raises a Lua error when none is: a script
/// that runs outside one - a piece of Lua run for its result - has no world to reach.
[[nodiscard]] const ScriptContext& checkContext(lua_State* state);

/// Pushes the value that stands for `entity`; nil for the null entity. Raises a Lua
/// error only when the scripts are out of memory or stack.
void pushEntity(lua_State* state, Entity entity);

/// The entity that the value at `index` stands for, whether it is still there or not.
/// Raises a Lua error, as a wrong argument, for a value that is no entity.
[[nodiscard]] Entity checkEntityValue(lua_State* state, int index);

/// The same, for an entity that has to be there: raises for one that is gone as well.
[[nodiscard]] Entity checkEntity(lua_State* state, int index);

/// Gives scripts `cordite.Entity`. Fails as ScriptRuntime::registerLibrary() does.
[[nodiscard]] Status registerEntityApi(ScriptRuntime& runtime);

} // namespace cordite
