#include "cordite/scripting/ScriptValue.h"

#include "cordite/scripting/Lua.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Its address is the value that stands for null: a light userdata, which is equal to
/// itself and to nothing else, and which nothing has to keep alive.
const char kNullTag = 0;

void pushValue(lua_State* state, const Json& value, bool inList, uint32_t depth);

void pushArray(lua_State* state, const Json& array, uint32_t depth) {
    const size_t size = array.size();
    lua_createtable(state, static_cast<int>(std::min<size_t>(size, std::numeric_limits<int>::max())), 0);
    lua_Integer position = 1;
    for (const Json& element : array) {
        pushValue(state, element, true, depth + 1);
        lua_rawseti(state, -2, position);
        ++position;
    }
}

void pushObject(lua_State* state, const Json& object, uint32_t depth) {
    lua_createtable(state, 0, static_cast<int>(std::min<size_t>(object.size(), std::numeric_limits<int>::max())));
    for (auto member = object.begin(); member != object.end(); ++member) {
        if (member.value().is_null()) {
            continue;
        }
        const std::string& name = member.key();
        lua_pushlstring(state, name.data(), name.size());
        pushValue(state, member.value(), false, depth + 1);
        lua_rawset(state, -3);
    }
}

void pushValue(lua_State* state, const Json& value, bool inList, uint32_t depth) {
    if (value.is_structured() && depth >= kMaxScriptValueDepth) {
        luaL_error(state, "a value for a script is nested more than %d deep", static_cast<int>(kMaxScriptValueDepth));
    }
    luaL_checkstack(state, 4, "a value is nested too deeply");
    switch (value.type()) {
        case Json::value_t::boolean:
            lua_pushboolean(state, value.get<bool>() ? 1 : 0);
            return;
        case Json::value_t::number_integer:
            lua_pushinteger(state, static_cast<lua_Integer>(value.get<int64_t>()));
            return;
        case Json::value_t::number_unsigned: {
            // Lua's integers are signed; what does not fit becomes the nearest float.
            const auto number = value.get<uint64_t>();
            if (number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                lua_pushinteger(state, static_cast<lua_Integer>(number));
            } else {
                lua_pushnumber(state, static_cast<lua_Number>(number));
            }
            return;
        }
        case Json::value_t::number_float:
            lua_pushnumber(state, static_cast<lua_Number>(value.get<double>()));
            return;
        case Json::value_t::string: {
            const auto& text = value.get_ref<const std::string&>();
            lua_pushlstring(state, text.data(), text.size());
            return;
        }
        case Json::value_t::array:
            pushArray(state, value, depth);
            return;
        case Json::value_t::object:
            pushObject(state, value, depth);
            return;
        case Json::value_t::null:
        case Json::value_t::binary:
        case Json::value_t::discarded:
            break;
    }
    // Null - and what the engine's JSON never holds, which is nothing to a script either.
    if (inList) {
        pushScriptNull(state);
    } else {
        lua_pushnil(state);
    }
}

/// What to call the key on top of the stack, which is no string, in a message. The key
/// is not converted: a number that became a string under lua_next() would lose the
/// traversal its place.
std::string describeKey(lua_State* state) {
    if (lua_type(state, -1) != LUA_TNUMBER) {
        return fmt::format("that is a {}", luaL_typename(state, -1));
    }
    if (lua_isinteger(state, -1) != 0) {
        return fmt::format("{}", static_cast<int64_t>(lua_tointeger(state, -1)));
    }
    return fmt::format("{}", static_cast<double>(lua_tonumber(state, -1)));
}

Result<Json> readValue(lua_State* state, int index, uint32_t depth);

/// The table at `index`, which has the keys 1 to `length` and no others.
Result<Json> readArray(lua_State* state, int index, lua_Unsigned length, uint32_t depth) {
    Json array = Json::array();
    for (lua_Unsigned position = 1; position <= length; ++position) {
        lua_rawgeti(state, index, static_cast<lua_Integer>(position));
        Result<Json> element = readValue(state, -1, depth + 1);
        lua_pop(state, 1);
        if (!element) {
            return tl::unexpected(element.error().withContext(fmt::format("element {}", position)));
        }
        array.push_back(std::move(*element));
    }
    return array;
}

/// The table at `index`, whose keys are all strings.
Result<Json> readObject(lua_State* state, int index, uint32_t depth) {
    std::vector<std::pair<std::string, Json>> members;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        std::string name(text, length);
        Result<Json> member = readValue(state, -1, depth + 1);
        lua_pop(state, 1);
        if (!member) {
            lua_pop(state, 1);
            return tl::unexpected(member.error().withContext(fmt::format("'{}'", name)));
        }
        members.emplace_back(std::move(name), std::move(*member));
    }
    // A table hands its members out in an order of its own; by name, the same table is
    // the same JSON every time.
    std::ranges::sort(members, {}, &std::pair<std::string, Json>::first);
    Json object = Json::object();
    for (auto& [name, member] : members) {
        object[name] = std::move(member);
    }
    return object;
}

/// The keys of a table, counted by what they are.
struct TableKeys {
    /// What Lua takes for the length of the table: a position that holds something
    /// and is followed by one that holds nothing.
    lua_Unsigned length = 0;
    /// Keys that are whole numbers from 1 to `length`.
    lua_Unsigned positions = 0;
    /// Keys that are strings.
    lua_Unsigned names = 0;
    /// The first key that is neither, as a message would call it; empty when there is
    /// none. The count stops at it.
    std::string other;
};

/// Counts the keys of the table at `index`, which is absolute. Needs two places on the
/// stack and leaves it as it was.
TableKeys countKeys(lua_State* state, int index) {
    TableKeys keys{.length = lua_rawlen(state, index)};
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        lua_pop(state, 1);
        if (lua_type(state, -1) == LUA_TSTRING) {
            ++keys.names;
        } else if (lua_isinteger(state, -1) != 0 && lua_tointeger(state, -1) >= 1 &&
                   static_cast<lua_Unsigned>(lua_tointeger(state, -1)) <= keys.length) {
            ++keys.positions;
        } else {
            keys.other = describeKey(state);
            lua_pop(state, 1);
            return keys;
        }
    }
    return keys;
}

Result<Json> readTable(lua_State* state, int index, uint32_t depth) {
    if (depth >= kMaxScriptValueDepth) {
        return makeError(ErrorCode::InvalidArgument, "tables are nested more than {} deep, or one of them holds itself",
                         kMaxScriptValueDepth);
    }
    luaL_checkstack(state, 4, "a value is nested too deeply");

    const TableKeys keys = countKeys(state, index);
    if (!keys.other.empty()) {
        return makeError(ErrorCode::InvalidArgument,
                         "a table can be stored when its keys are 1 to n, or names; this one has a key {}", keys.other);
    }
    if (keys.names == 0 && keys.positions == 0) {
        return Json::object();
    }
    if (keys.positions == 0) {
        return readObject(state, index, depth);
    }
    if (keys.names != 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "a table can be stored when its keys are 1 to n, or names; this one has both");
    }
    if (keys.positions != keys.length) {
        return makeError(ErrorCode::InvalidArgument,
                         "a table can be stored when its keys are 1 to n; this one has nothing at some of the "
                         "positions up to {}",
                         keys.length);
    }
    return readArray(state, index, keys.length, depth);
}

Result<Json> readValue(lua_State* state, int index, uint32_t depth) {
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            return Json(nullptr);
        case LUA_TBOOLEAN:
            return Json(lua_toboolean(state, index) != 0);
        case LUA_TNUMBER: {
            if (lua_isinteger(state, index) != 0) {
                return Json(static_cast<int64_t>(lua_tointeger(state, index)));
            }
            const auto number = static_cast<double>(lua_tonumber(state, index));
            if (!std::isfinite(number)) {
                return makeError(ErrorCode::InvalidArgument, "a number that is not finite cannot be stored");
            }
            return Json(number);
        }
        case LUA_TSTRING: {
            size_t length = 0;
            const char* text = lua_tolstring(state, index, &length);
            return Json(std::string(text, length));
        }
        case LUA_TTABLE:
            return readTable(state, lua_absindex(state, index), depth);
        case LUA_TLIGHTUSERDATA:
            if (isScriptNull(state, index)) {
                return Json(nullptr);
            }
            break;
        default:
            break;
    }
    return makeError(ErrorCode::InvalidArgument, "a {} cannot be stored", luaL_typename(state, index));
}

} // namespace

void pushScriptNull(lua_State* state) {
    // Lua never writes through the pointer; its API merely has no way to say so.
    lua_pushlightuserdata(state, const_cast<char*>(&kNullTag));
}

bool isScriptNull(lua_State* state, int index) {
    return lua_type(state, index) == LUA_TLIGHTUSERDATA && lua_touserdata(state, index) == &kNullTag;
}

void pushScriptValue(lua_State* state, const Json& value) {
    pushValue(state, value, false, 0);
}

Result<Json> readScriptValue(lua_State* state, int index) {
    return readValue(state, index, 0);
}

std::optional<size_t> scriptListLength(lua_State* state, int index) {
    if (lua_type(state, index) != LUA_TTABLE) {
        return std::nullopt;
    }
    luaL_checkstack(state, 4, "a value is nested too deeply");
    const TableKeys keys = countKeys(state, lua_absindex(state, index));
    if (!keys.other.empty() || keys.names != 0 || keys.positions != keys.length) {
        return std::nullopt;
    }
    return static_cast<size_t>(keys.length);
}

} // namespace cordite
