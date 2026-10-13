#include "cordite/core/Json.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace cordite {
namespace {

/// Where Lua keeps the metatables of vectors and rotations, which are the tables of
/// their functions.
constexpr const char* kVec3Type = "cordite.Vec3";
constexpr const char* kQuatType = "cordite.Quat";

/// Scripts compute in doubles, as Lua does: a vector that is only ever added to
/// another never passes through a float, and comes out as exact as it went in.
using DVec3 = glm::dvec3;
using DQuat = glm::dquat;

constexpr std::array<const char*, 4> kNames = {"x", "y", "z", "w"};

void pushNumbers(lua_State* state, std::span<const double> numbers, const char* type) {
    luaL_checkstack(state, 3, "the scripts are out of stack");
    lua_createtable(state, 0, static_cast<int>(numbers.size()));
    for (size_t index = 0; index < numbers.size(); ++index) {
        // Nothing is nothing, whichever side it was reached from: the vector that
        // points the other way from nowhere is not shown as (-0.0, -0.0, -0.0).
        const double number = numbers[index] == 0.0 ? 0.0 : numbers[index];
        lua_pushnumber(state, static_cast<lua_Number>(number));
        lua_setfield(state, -2, kNames[index]);
    }
    luaL_setmetatable(state, type);
}

void pushVector(lua_State* state, const DVec3& value) {
    pushNumbers(state, std::array{value.x, value.y, value.z}, kVec3Type);
}

void pushRotation(lua_State* state, const DQuat& value) {
    pushNumbers(state, std::array{value.x, value.y, value.z, value.w}, kQuatType);
}

/// The vector at `index`, as exact as Lua has it. Raises for what is none.
DVec3 checkVector(lua_State* state, int index) {
    std::array<double, 3> numbers{};
    if (!readNumbers(state, index, 3, numbers.data())) {
        luaL_typeerror(state, index, "Vec3");
    }
    return {numbers[0], numbers[1], numbers[2]};
}

/// The rotation at `index`, of length one. Raises for what is none.
DQuat checkRotation(lua_State* state, int index) {
    std::array<double, 4> numbers{};
    if (!readNumbers(state, index, 4, numbers.data())) {
        luaL_typeerror(state, index, "Quat");
    }
    const DQuat rotation(numbers[3], numbers[0], numbers[1], numbers[2]);
    const double length = glm::length(rotation);
    if (!std::isfinite(length) || length < 1.0e-12) {
        luaL_argerror(state, index, "a rotation has a length, and numbers that are finite");
    }
    return rotation / length;
}

double checkFinite(lua_State* state, int index) {
    const auto number = static_cast<double>(luaL_checknumber(state, index));
    if (!std::isfinite(number)) {
        luaL_argerror(state, index, "a number that is finite expected");
    }
    return number;
}

int makeVec3(lua_State* state) {
    pushVector(state,
               {static_cast<double>(luaL_optnumber(state, 1, 0.0)), static_cast<double>(luaL_optnumber(state, 2, 0.0)),
                static_cast<double>(luaL_optnumber(state, 3, 0.0))});
    return 1;
}

int vec3Add(lua_State* state) {
    pushVector(state, checkVector(state, 1) + checkVector(state, 2));
    return 1;
}

int vec3Subtract(lua_State* state) {
    pushVector(state, checkVector(state, 1) - checkVector(state, 2));
    return 1;
}

/// One of the two is a vector, or Lua would not have come here; the other may be a
/// number, which stands for a vector of three of it.
DVec3 vectorOrNumber(lua_State* state, int index) {
    if (lua_type(state, index) == LUA_TNUMBER) {
        return DVec3(static_cast<double>(lua_tonumber(state, index)));
    }
    return checkVector(state, index);
}

int vec3Multiply(lua_State* state) {
    pushVector(state, vectorOrNumber(state, 1) * vectorOrNumber(state, 2));
    return 1;
}

int vec3Divide(lua_State* state) {
    pushVector(state, vectorOrNumber(state, 1) / vectorOrNumber(state, 2));
    return 1;
}

int vec3Negate(lua_State* state) {
    pushVector(state, -checkVector(state, 1));
    return 1;
}

int vec3Equals(lua_State* state) {
    std::array<double, 3> first{};
    std::array<double, 3> second{};
    const bool same =
        readNumbers(state, 1, 3, first.data()) && readNumbers(state, 2, 3, second.data()) && first == second;
    lua_pushboolean(state, same ? 1 : 0);
    return 1;
}

int vec3ToString(lua_State* state) {
    const DVec3 value = checkVector(state, 1);
    lua_pushfstring(state, "(%f, %f, %f)", static_cast<lua_Number>(value.x), static_cast<lua_Number>(value.y),
                    static_cast<lua_Number>(value.z));
    return 1;
}

int vec3Length(lua_State* state) {
    lua_pushnumber(state, static_cast<lua_Number>(glm::length(checkVector(state, 1))));
    return 1;
}

int vec3Normalized(lua_State* state) {
    const DVec3 value = checkVector(state, 1);
    const double length = glm::length(value);
    // What has no length has no direction: it stays what it is, nothing.
    pushVector(state, std::isfinite(length) && length > 0.0 ? value / length : DVec3(0.0));
    return 1;
}

int vec3Dot(lua_State* state) {
    lua_pushnumber(state, static_cast<lua_Number>(glm::dot(checkVector(state, 1), checkVector(state, 2))));
    return 1;
}

int vec3Cross(lua_State* state) {
    pushVector(state, glm::cross(checkVector(state, 1), checkVector(state, 2)));
    return 1;
}

int vec3Distance(lua_State* state) {
    lua_pushnumber(state, static_cast<lua_Number>(glm::distance(checkVector(state, 1), checkVector(state, 2))));
    return 1;
}

int vec3Lerp(lua_State* state) {
    const DVec3 from = checkVector(state, 1);
    const DVec3 to = checkVector(state, 2);
    const auto amount = static_cast<double>(luaL_checknumber(state, 3));
    pushVector(state, from + (to - from) * amount);
    return 1;
}

int makeQuat(lua_State* state) {
    const DQuat rotation(
        static_cast<double>(luaL_optnumber(state, 4, 1.0)), static_cast<double>(luaL_optnumber(state, 1, 0.0)),
        static_cast<double>(luaL_optnumber(state, 2, 0.0)), static_cast<double>(luaL_optnumber(state, 3, 0.0)));
    const double length = glm::length(rotation);
    if (!std::isfinite(length) || length < 1.0e-12) {
        return luaL_error(state, "a rotation has a length, and numbers that are finite");
    }
    pushRotation(state, rotation / length);
    return 1;
}

int makeEuler(lua_State* state) {
    // Three numbers, or a vector of them: the angles about X, Y and Z, in degrees.
    Vec3 degrees(0.0f);
    if (lua_type(state, 1) == LUA_TTABLE) {
        degrees = checkVec3(state, 1);
    } else {
        degrees = Vec3(static_cast<float>(checkFinite(state, 1)), static_cast<float>(checkFinite(state, 2)),
                       static_cast<float>(checkFinite(state, 3)));
        // Finite as Lua has them is not yet finite as the engine keeps them.
        if (!isFinite(degrees)) {
            return luaL_error(state, "three angles that are finite, and not too large, expected");
        }
    }
    pushQuat(state, quatFromEulerDegrees(degrees));
    return 1;
}

int makeAxisAngle(lua_State* state) {
    const DVec3 axis = checkVector(state, 1);
    const double degrees = checkFinite(state, 2);
    const double length = glm::length(axis);
    if (!std::isfinite(length) || length < 1.0e-12) {
        return luaL_argerror(state, 1, "an axis has a direction: a vector of some length");
    }
    pushRotation(state, glm::angleAxis(glm::radians(degrees), axis / length));
    return 1;
}

int makeLookRotation(lua_State* state) {
    const Vec3 direction = checkVec3(state, 1);
    const Vec3 up = lua_isnoneornil(state, 2) ? kWorldUp : checkVec3(state, 2);
    if (glm::length(direction) < 1.0e-6f) {
        return luaL_argerror(state, 1, "a direction to look in: a vector of some length");
    }
    pushQuat(state, lookRotation(direction, up));
    return 1;
}

int quatMultiply(lua_State* state) {
    const DQuat rotation = checkRotation(state, 1);
    std::array<double, 4> other{};
    if (readNumbers(state, 2, 4, other.data())) {
        pushRotation(state, glm::normalize(rotation * checkRotation(state, 2)));
    } else {
        pushVector(state, rotation * checkVector(state, 2));
    }
    return 1;
}

int quatEquals(lua_State* state) {
    std::array<double, 4> first{};
    std::array<double, 4> second{};
    const bool same =
        readNumbers(state, 1, 4, first.data()) && readNumbers(state, 2, 4, second.data()) && first == second;
    lua_pushboolean(state, same ? 1 : 0);
    return 1;
}

int quatToString(lua_State* state) {
    std::array<double, 4> numbers{};
    if (!readNumbers(state, 1, 4, numbers.data())) {
        return luaL_typeerror(state, 1, "Quat");
    }
    lua_pushfstring(state, "(%f, %f, %f, %f)", static_cast<lua_Number>(numbers[0]), static_cast<lua_Number>(numbers[1]),
                    static_cast<lua_Number>(numbers[2]), static_cast<lua_Number>(numbers[3]));
    return 1;
}

int quatInverse(lua_State* state) {
    pushRotation(state, glm::conjugate(checkRotation(state, 1)));
    return 1;
}

int quatSlerp(lua_State* state) {
    const DQuat from = checkRotation(state, 1);
    const DQuat to = checkRotation(state, 2);
    const auto amount = static_cast<double>(luaL_checknumber(state, 3));
    pushRotation(state, glm::normalize(glm::slerp(from, to, amount)));
    return 1;
}

int quatEuler(lua_State* state) {
    pushVec3(state, eulerDegreesFromQuat(checkQuat(state, 1)));
    return 1;
}

int quatForward(lua_State* state) {
    pushVector(state, checkRotation(state, 1) * DVec3(kWorldForward));
    return 1;
}

int quatRight(lua_State* state) {
    pushVector(state, checkRotation(state, 1) * DVec3(kWorldRight));
    return 1;
}

int quatUp(lua_State* state) {
    pushVector(state, checkRotation(state, 1) * DVec3(kWorldUp));
    return 1;
}

constexpr std::array kMakers = {
    ScriptFunction{"vec3", "cordite.vec3(x, y, z) -> Vec3",
                   "A vector: a table with x, y and z, which add, subtract, scale and compare like numbers. What "
                   "is left out is 0.",
                   makeVec3},
    ScriptFunction{"quat", "cordite.quat(x, y, z, w) -> Quat",
                   "A rotation from its four numbers, brought to length one. Without numbers, the rotation that "
                   "turns nothing.",
                   makeQuat},
    ScriptFunction{"euler", "cordite.euler(x, y, z) -> Quat",
                   "The rotation of three angles in degrees - about X, about Y, about Z - as the inspector of the "
                   "editor shows them: Y is applied first, then X, then Z. Takes a vector of the three as well.",
                   makeEuler},
    ScriptFunction{"axisAngle", "cordite.axisAngle(axis, degrees) -> Quat",
                   "The rotation by an angle in degrees about an axis, counter-clockwise for whoever looks "
                   "against the axis.",
                   makeAxisAngle},
    ScriptFunction{"lookRotation", "cordite.lookRotation(direction, up) -> Quat",
                   "The rotation that makes forward (-Z) point along a direction, with up (+Y) as near to `up` as "
                   "that allows; `up` is the world's when left out.",
                   makeLookRotation},
};

constexpr std::array kVec3Functions = {
    ScriptFunction{"__add", "a + b -> Vec3", "The sum of two vectors.", vec3Add},
    ScriptFunction{"__sub", "a - b -> Vec3", "The difference of two vectors.", vec3Subtract},
    ScriptFunction{"__mul", "a * b -> Vec3",
                   "A vector scaled by a number, or two vectors multiplied component by component.", vec3Multiply},
    ScriptFunction{"__div", "a / b -> Vec3",
                   "A vector divided by a number, or two vectors divided component by component.", vec3Divide},
    ScriptFunction{"__unm", "-a -> Vec3", "The vector that points the other way.", vec3Negate},
    ScriptFunction{"__eq", "a == b -> boolean", "Whether two vectors have the same three numbers, exactly.",
                   vec3Equals},
    ScriptFunction{"__tostring", "tostring(a) -> string", "What print() shows for a vector: (x, y, z).", vec3ToString},
    ScriptFunction{"length", "a:length() -> number", "How long the vector is.", vec3Length},
    ScriptFunction{"normalized", "a:normalized() -> Vec3",
                   "The vector of length one that points the same way; a vector of no length stays one.",
                   vec3Normalized},
    ScriptFunction{"dot", "a:dot(b) -> number",
                   "The dot product: how far one vector reaches along the other, times the length of that one.",
                   vec3Dot},
    ScriptFunction{"cross", "a:cross(b) -> Vec3",
                   "The cross product: a vector at right angles to both, by the rule of the right hand.", vec3Cross},
    ScriptFunction{"distance", "a:distance(b) -> number", "How far it is from one point to the other.", vec3Distance},
    ScriptFunction{"lerp", "a:lerp(b, amount) -> Vec3",
                   "The point on the way from a to b: a at 0, b at 1, and beyond both outside of that.", vec3Lerp},
};

constexpr std::array kQuatFunctions = {
    ScriptFunction{"__mul", "q * r -> Quat, q * v -> Vec3",
                   "Two rotations in one - r first, then q - or a vector turned by the rotation.", quatMultiply},
    ScriptFunction{"__eq", "q == r -> boolean", "Whether two rotations have the same four numbers, exactly.",
                   quatEquals},
    ScriptFunction{"__tostring", "tostring(q) -> string", "What print() shows for a rotation: (x, y, z, w).",
                   quatToString},
    ScriptFunction{"inverse", "q:inverse() -> Quat", "The rotation that undoes this one.", quatInverse},
    ScriptFunction{"slerp", "q:slerp(r, amount) -> Quat",
                   "The rotation on the shortest way from q to r: q at 0, r at 1, at an even speed between.",
                   quatSlerp},
    ScriptFunction{"euler", "q:euler() -> Vec3",
                   "The three angles in degrees that cordite.euler() makes this rotation of.", quatEuler},
    ScriptFunction{"forward", "q:forward() -> Vec3", "Where forward (-Z) points once turned by the rotation.",
                   quatForward},
    ScriptFunction{"right", "q:right() -> Vec3", "Where right (+X) points once turned by the rotation.", quatRight},
    ScriptFunction{"up", "q:up() -> Vec3", "Where up (+Y) points once turned by the rotation.", quatUp},
};

/// Makes the library `name` of `cordite` the metatable of its kind of value, kept
/// where Lua finds it by `type`.
void makeMetatable(lua_State* state, const char* name, const char* type) {
    lua_getglobal(state, "cordite");
    lua_getfield(state, -1, name);
    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "__index");
    lua_pushstring(state, name);
    lua_setfield(state, -2, "__name");
    lua_setfield(state, LUA_REGISTRYINDEX, type);
    lua_pop(state, 1);
}

} // namespace

double scriptNumber(float value) {
    // Nothing is nothing, whichever side it was reached from: a script that prints an
    // angle is not shown "-0.0".
    if (value == 0.0f) {
        return 0.0;
    }
    const Json shortest = jsonFromFloat(value);
    return shortest.is_number() ? shortest.get<double>() : static_cast<double>(value);
}

void pushVec3(lua_State* state, const Vec3& value) {
    pushVector(state, {scriptNumber(value.x), scriptNumber(value.y), scriptNumber(value.z)});
}

void pushQuat(lua_State* state, const Quat& value) {
    pushNumbers(state,
                std::array{scriptNumber(value.x), scriptNumber(value.y), scriptNumber(value.z), scriptNumber(value.w)},
                kQuatType);
}

bool readNumbers(lua_State* state, int index, int count, double* numbers) {
    if (lua_type(state, index) != LUA_TTABLE) {
        return false;
    }
    index = lua_absindex(state, index);
    luaL_checkstack(state, 2, "the scripts are out of stack");
    // By name, or - for one written in passing, {0, 1, 0} - by position; and no number
    // more than was asked for: a rotation is not a vector that has something extra.
    // Looked up without asking a metatable: nothing of a script runs in here.
    lua_pushstring(state, kNames[0]);
    lua_rawget(state, index);
    const bool named = lua_type(state, -1) == LUA_TNUMBER;
    lua_pop(state, 1);
    for (int item = 0; item <= count && item < 4; ++item) {
        if (named) {
            lua_pushstring(state, kNames[static_cast<size_t>(item)]);
            lua_rawget(state, index);
        } else {
            lua_rawgeti(state, index, item + 1);
        }
        const bool isNumber = lua_type(state, -1) == LUA_TNUMBER;
        if (item < count && isNumber) {
            numbers[item] = static_cast<double>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
        if (isNumber != (item < count)) {
            return false;
        }
    }
    return true;
}

Vec3 checkVec3(lua_State* state, int index) {
    std::array<double, 3> numbers{};
    if (!readNumbers(state, index, 3, numbers.data())) {
        luaL_typeerror(state, index, "Vec3");
    }
    const Vec3 value(static_cast<float>(numbers[0]), static_cast<float>(numbers[1]), static_cast<float>(numbers[2]));
    if (!isFinite(value)) {
        luaL_argerror(state, index, "a vector of numbers that are finite, and not too large, expected");
    }
    return value;
}

Quat checkQuat(lua_State* state, int index) {
    const DQuat rotation = checkRotation(state, index);
    return {static_cast<float>(rotation.w), static_cast<float>(rotation.x), static_cast<float>(rotation.y),
            static_cast<float>(rotation.z)};
}

Status registerMathApi(ScriptRuntime& runtime) {
    CORDITE_TRY(runtime.registerLibrary("cordite", kMakers));
    CORDITE_TRY(runtime.registerLibrary(kVec3Type, kVec3Functions));
    CORDITE_TRY(runtime.registerLibrary(kQuatType, kQuatFunctions));
    return runtime.protect([](lua_State* state) {
        makeMetatable(state, "Vec3", kVec3Type);
        makeMetatable(state, "Quat", kQuatType);
    });
}

} // namespace cordite
