#include "cordite/platform/InputMap.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {
namespace {

// A script asks about the controls of the game by the names the input map of the project
// gives them (platform/InputMap.h): whether "jump" is held, how far "moveX" is pushed.
// Which key that is, it never learns. A name the map does not have is a mistake - of the
// script or of the map - and is said, with the names there are, rather than answered
// with "not pressed".

/// The controls of the update that is under way. Raises where there are none.
const InputActions& checkControls(lua_State* state) {
    const InputActions* controls = checkContext(state).controls;
    if (controls == nullptr) {
        luaL_error(state, "there are no controls to ask: nothing gives this world any");
    }
    return *controls;
}

/// The names of what a map has, for a message: "jump, fire".
template <typename Entry>
std::string namesOf(const std::vector<Entry>& entries) {
    std::string names;
    for (const Entry& entry : entries) {
        names += names.empty() ? "" : ", ";
        names += entry.name;
    }
    return names.empty() ? "none" : names;
}

/// The name of an action at `index`. Raises for one the map does not have.
std::string_view checkAction(lua_State* state, int index, const InputActions& controls) {
    size_t length = 0;
    const char* text = luaL_checklstring(state, index, &length);
    const std::string_view name(text, length);
    if (!controls.hasAction(name)) {
        const std::string known = namesOf(controls.map().actions);
        luaL_argerror(
            state, index,
            lua_pushfstring(state, "the input map has no action called '%s'; it has: %s", text, known.c_str()));
    }
    return name;
}

int inputIsDown(lua_State* state) {
    const InputActions& controls = checkControls(state);
    lua_pushboolean(state, controls.isDown(checkAction(state, 1, controls)) ? 1 : 0);
    return 1;
}

int inputWasPressed(lua_State* state) {
    const InputActions& controls = checkControls(state);
    lua_pushboolean(state, controls.wasPressed(checkAction(state, 1, controls)) ? 1 : 0);
    return 1;
}

int inputWasReleased(lua_State* state) {
    const InputActions& controls = checkControls(state);
    lua_pushboolean(state, controls.wasReleased(checkAction(state, 1, controls)) ? 1 : 0);
    return 1;
}

int inputAxis(lua_State* state) {
    const InputActions& controls = checkControls(state);
    size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    const std::string_view name(text, length);
    if (!controls.hasAxis(name)) {
        const std::string known = namesOf(controls.map().axes);
        return luaL_argerror(
            state, 1, lua_pushfstring(state, "the input map has no axis called '%s'; it has: %s", text, known.c_str()));
    }
    lua_pushnumber(state, static_cast<lua_Number>(scriptNumber(controls.axis(name))));
    return 1;
}

constexpr std::array kInputFunctions = {
    ScriptFunction{"isDown", "cordite.input.isDown(action) -> boolean",
                   "Whether an action of the input map is held: any key, button or stick that is bound to it.",
                   inputIsDown},
    ScriptFunction{"wasPressed", "cordite.input.wasPressed(action) -> boolean",
                   "Whether the action went down in this frame - also when it has come up again already: what is "
                   "tapped between two frames was pressed, and released.",
                   inputWasPressed},
    ScriptFunction{"wasReleased", "cordite.input.wasReleased(action) -> boolean",
                   "Whether the action came up in this frame.", inputWasReleased},
    ScriptFunction{"axis", "cordite.input.axis(axis) -> number",
                   "How far an axis of the input map is pushed, and which way: what its binding that is moved "
                   "furthest gives, times the scale of that binding. Two keys give -1, 0 or 1.",
                   inputAxis},
};

} // namespace

Status registerInputApi(ScriptRuntime& runtime) {
    return runtime.registerLibrary("cordite.input", kInputFunctions);
}

} // namespace cordite
