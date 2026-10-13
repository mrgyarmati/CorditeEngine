#pragma once

/// @file
/// For the sources of the scripting module alone: what the functions that scripts call
/// share - how they find the world, and how entities, vectors and rotations cross
/// between C++ and Lua.
///
/// @code
/// int entitySetPosition(lua_State* state) {
///     const Entity entity = checkEntity(state, 1);             // raises for anything else, or one that is gone
///     const Vec3 position = checkVec3(state, 2);               // raises for what is no vector of finite numbers
///     checkContext(state).world.transform(entity).position = position;
///     return 0;
/// }
/// @endcode
///
/// To a script an entity is a value of its own kind, with the functions of
/// `cordite.Entity` as its methods: `door:name()`. It is the same value every time for
/// the same entity, so two of them are equal exactly when they are the same entity, and
/// they serve as keys of tables. The value outlives its entity; what is done with it
/// then is an error that says so, and `isValid()` is how a script asks first.
///
/// A vector is a table with `x`, `y` and `z`, and a rotation one with `x`, `y`, `z` and
/// `w`: plain tables, which a script can read, change and make by hand. Those that the
/// engine makes have `cordite.Vec3` and `cordite.Quat` as their metatables, for the
/// arithmetic and the methods. The engine keeps single floats, and Lua numbers are
/// doubles: a float goes to Lua as the number that is written the shortest - 0.1, not
/// 0.10000000149011612 - which is the same number the reflection writes into files.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
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

/// Whether the value at `index` stands for an entity at all.
[[nodiscard]] bool isEntityValue(lua_State* state, int index);

/// A float as Lua is given it: the double that is written the shortest and is still
/// that float.
[[nodiscard]] double scriptNumber(float value);

/// Pushes a vector, or a rotation, as a table with the metatable that goes with it.
void pushVec3(lua_State* state, const Vec3& value);
void pushQuat(lua_State* state, const Quat& value);

/// The vector at `index`: a table with x, y and z - or with three numbers at the
/// positions 1 to 3, as a script writes one in passing: {0, 1, 0}. Raises a Lua error,
/// as a wrong argument, for anything else and for numbers that are not finite.
[[nodiscard]] Vec3 checkVec3(lua_State* state, int index);

/// The rotation at `index`: a table with x, y, z and w, or four numbers; of length one
/// when it is returned. Raises as checkVec3() does, and for one that has no length.
[[nodiscard]] Quat checkQuat(lua_State* state, int index);

/// Whether the table at `index` is shaped like a vector of `count` numbers - with the
/// names x, y, z, w or at the positions 1 to `count` - and if so, those numbers.
[[nodiscard]] bool readNumbers(lua_State* state, int index, int count, double* numbers);

/// Give scripts their libraries. Each fails as ScriptRuntime::registerLibrary() does.
[[nodiscard]] Status registerEntityApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerMathApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerWorldApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerComponentApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerInputApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerPhysicsApi(ScriptRuntime& runtime);
[[nodiscard]] Status registerAudioApi(ScriptRuntime& runtime);

} // namespace cordite
