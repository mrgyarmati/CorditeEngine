#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/scripting/ScriptValue.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

// Components are what the reflection says they are (core/Reflection.h): a script reads
// and writes them as the scene files and the editor do, by the names of their
// properties, and what it writes is checked by the same rules - types, ranges, what is
// read-only. On the way, values take the forms a script works with: a vector is a Vec3
// and a rotation a Quat rather than lists of numbers, and a reference to an entity is the
// entity rather than its id.

// --- From the engine to Lua --------------------------------------------------------------

void pushValueOf(lua_State* state, PropertyType type, const PropertyInfo& property, const Json& value);

/// A list of numbers as a table with the names x, y, z and w.
void pushNamedNumbers(lua_State* state, const Json& numbers) {
    constexpr std::array<const char*, 4> kNames = {"x", "y", "z", "w"};
    lua_createtable(state, 0, 4);
    for (size_t index = 0; index < numbers.size() && index < kNames.size(); ++index) {
        pushScriptValue(state, numbers[index]);
        lua_setfield(state, -2, kNames[index]);
    }
}

void pushObjectOf(lua_State* state, const TypeInfo& type, const Json& object) {
    lua_createtable(state, 0, static_cast<int>(type.properties().size()));
    for (const PropertyInfo& property : type.properties()) {
        const auto member = object.find(property.name());
        if (member == object.end()) {
            continue;
        }
        // What holds nothing is pushed as nil, and a table that is given nil for a
        // name does not have the name: asking for it gives nil, as for what is not there.
        pushValueOf(state, property.type(), property, *member);
        lua_setfield(state, -2, property.name().c_str());
    }
}

void pushValueOf(lua_State* state, PropertyType type, const PropertyInfo& property, const Json& value) {
    luaL_checkstack(state, 4, "a value is nested too deeply");
    const bool isNumbers = value.is_array() && !value.empty() && value[0].is_number();
    switch (type) {
        case PropertyType::Vec3:
            if (isNumbers && value.size() == 3) {
                pushVec3(state, Vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>()));
                return;
            }
            break;
        case PropertyType::Quat:
            if (isNumbers && value.size() == 4) {
                // Stored as [x, y, z, w]; glm's constructor takes w first.
                pushQuat(state, Quat(value[3].get<float>(), value[0].get<float>(), value[1].get<float>(),
                                     value[2].get<float>()));
                return;
            }
            break;
        case PropertyType::Vec2:
        case PropertyType::Vec4:
            if (isNumbers) {
                pushNamedNumbers(state, value);
                return;
            }
            break;
        case PropertyType::Uuid:
            if (property.attributes().semantic == PropertySemantic::EntityReference) {
                const std::optional<Uuid> id = value.is_string() ? Uuid::parse(value.get<std::string>()) : std::nullopt;
                pushEntity(state, id ? checkContext(state).world.findById(*id) : Entity{});
                return;
            }
            break;
        case PropertyType::Object:
            if (property.objectType() != nullptr && value.is_object()) {
                pushObjectOf(state, *property.objectType(), value);
                return;
            }
            break;
        case PropertyType::List:
            if (value.is_array()) {
                lua_createtable(state, static_cast<int>(value.size()), 0);
                lua_Integer position = 1;
                for (const Json& element : value) {
                    pushValueOf(state, property.elementType(), property, element);
                    // A place in a list that holds nothing still is a place.
                    if (lua_isnil(state, -1)) {
                        lua_pop(state, 1);
                        pushScriptNull(state);
                    }
                    lua_rawseti(state, -2, position++);
                }
                return;
            }
            break;
        case PropertyType::Bool:
        case PropertyType::Int:
        case PropertyType::Float:
        case PropertyType::String:
        case PropertyType::Enum:
        case PropertyType::Json:
            break;
    }
    pushScriptValue(state, value);
}

// --- From Lua to the engine ---------------------------------------------------------------

Result<Json> readValueOf(lua_State* state, int index, PropertyType type, const PropertyInfo& property, uint32_t depth);

/// A table of names as the JSON object of a reflected type. What is no property of the
/// type is passed on as it is, for the reflection to refuse by name.
Result<Json> readObjectOf(lua_State* state, int index, const TypeInfo& type, uint32_t depth) {
    if (lua_type(state, index) != LUA_TTABLE) {
        return makeError(ErrorCode::InvalidArgument, "a table of the properties of {} expected, not a {}", type.name(),
                         luaL_typename(state, index));
    }
    index = lua_absindex(state, index);
    luaL_checkstack(state, 4, "a value is nested too deeply");
    Json object = Json::object();
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            return makeError(ErrorCode::InvalidArgument,
                             "the properties of {} have names; a table of them has no other keys", type.name());
        }
        const std::string name = lua_tostring(state, -2);
        const PropertyInfo* property = type.findProperty(name);
        Result<Json> value = property != nullptr ? readValueOf(state, -1, property->type(), *property, depth + 1)
                                                 : readScriptValue(state, -1);
        lua_pop(state, 1);
        if (!value) {
            lua_pop(state, 1);
            return tl::unexpected(value.error().withContext(fmt::format("'{}'", name)));
        }
        object[name] = std::move(*value);
    }
    return object;
}

Result<Json> readValueOf(lua_State* state, int index, PropertyType type, const PropertyInfo& property, uint32_t depth) {
    index = lua_absindex(state, index);
    // What stands for nothing in a list is nothing here too: a reference that is cleared.
    if (lua_isnil(state, index) || isScriptNull(state, index)) {
        return Json(nullptr);
    }
    // The types say how deep a value goes, and a list of lists says it of every depth:
    // a table that holds itself would be followed for ever.
    if (depth >= kMaxScriptValueDepth && lua_type(state, index) == LUA_TTABLE) {
        return makeError(ErrorCode::InvalidArgument, "tables are nested more than {} deep, or one of them holds itself",
                         kMaxScriptValueDepth);
    }
    const auto numbers = [&](int count) -> std::optional<Json> {
        std::array<double, 4> read{};
        if (!readNumbers(state, index, count, read.data())) {
            return std::nullopt;
        }
        Json list = Json::array();
        for (int item = 0; item < count; ++item) {
            list.push_back(read[static_cast<size_t>(item)]);
        }
        return list;
    };
    switch (type) {
        case PropertyType::Vec2:
            if (std::optional<Json> list = numbers(2)) {
                return std::move(*list);
            }
            break;
        case PropertyType::Vec3:
            if (std::optional<Json> list = numbers(3)) {
                return std::move(*list);
            }
            break;
        case PropertyType::Vec4:
        case PropertyType::Quat:
            if (std::optional<Json> list = numbers(4)) {
                return std::move(*list);
            }
            break;
        case PropertyType::Uuid:
            if (isEntityValue(state, index)) {
                if (property.attributes().semantic != PropertySemantic::EntityReference) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "an entity was given to what does not refer to an entity");
                }
                const World& world = checkContext(state).world;
                const Entity entity = checkEntityValue(state, index);
                if (!world.isValid(entity)) {
                    return makeError(ErrorCode::InvalidArgument, "the entity is gone: it was destroyed");
                }
                return Json(world.id(entity).toString());
            }
            break;
        case PropertyType::Object:
            if (property.objectType() != nullptr) {
                return readObjectOf(state, index, *property.objectType(), depth);
            }
            break;
        case PropertyType::List:
            // A table that is no list - one with names in it - is left for what follows
            // to refuse, rather than taken for the list of what stands at its positions.
            if (const std::optional<size_t> length = scriptListLength(state, index)) {
                luaL_checkstack(state, 2, "a value is nested too deeply");
                Json list = Json::array();
                for (size_t position = 1; position <= *length; ++position) {
                    lua_rawgeti(state, index, static_cast<lua_Integer>(position));
                    Result<Json> element = readValueOf(state, -1, property.elementType(), property, depth + 1);
                    lua_pop(state, 1);
                    if (!element) {
                        return tl::unexpected(element.error().withContext(fmt::format("element {}", position)));
                    }
                    list.push_back(std::move(*element));
                }
                return list;
            }
            break;
        case PropertyType::Bool:
        case PropertyType::Int:
        case PropertyType::Float:
        case PropertyType::String:
        case PropertyType::Enum:
        case PropertyType::Json:
            break;
    }
    return readScriptValue(state, index);
}

// --- cordite.Entity: its components -----------------------------------------------------------

/// The component that the argument at `index` names. Raises for a name that is none.
const ComponentDescriptor& checkComponent(lua_State* state, int index) {
    size_t length = 0;
    const char* name = luaL_checklstring(state, index, &length);
    const ComponentDescriptor* descriptor = checkContext(state).components.find(std::string_view(name, length));
    if (descriptor == nullptr) {
        luaL_argerror(state, index, lua_pushfstring(state, "there is no component called '%s'", name));
    }
    return *descriptor;
}

/// The property of a component that the argument at `index` names. Raises for a name
/// that is none.
const PropertyInfo& checkProperty(lua_State* state, int index, const ComponentDescriptor& descriptor) {
    size_t length = 0;
    const char* name = luaL_checklstring(state, index, &length);
    const PropertyInfo* property = descriptor.type.findProperty(std::string_view(name, length));
    if (property == nullptr) {
        luaL_argerror(state, index,
                      lua_pushfstring(state, "%s has no property '%s'", descriptor.type.name().c_str(), name));
    }
    return *property;
}

int entityHas(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const ComponentDescriptor& descriptor = checkComponent(state, 2);
    lua_pushboolean(state, descriptor.has(checkContext(state).world.registry(), entity.handle) ? 1 : 0);
    return 1;
}

int entityGet(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const ComponentDescriptor& descriptor = checkComponent(state, 2);
    const PropertyInfo* only = lua_isnoneornil(state, 3) ? nullptr : &checkProperty(state, 3, descriptor);

    // What is read is copied out before anything is made of it: making tables may
    // run out of memory, and must not do so half way through a component.
    const World& world = checkContext(state).world;
    const void* instance = descriptor.getConst(world.registry(), entity.handle);
    if (instance == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (only != nullptr) {
        const Json value = only->get(instance);
        pushValueOf(state, only->type(), *only, value);
        return 1;
    }
    Json values = Json::object();
    for (const PropertyInfo& property : descriptor.type.properties()) {
        values[property.name()] = property.get(instance);
    }
    pushObjectOf(state, descriptor.type, values);
    return 1;
}

/// Raises what the reflection has to say against a change; it names the component
/// itself. Does not return; the int is what lets a function end with `return ...`.
int raiseRefusal(lua_State* state, const Error& error) {
    return luaL_error(state, "%s", error.message.c_str());
}

/// Raises what kept a value from being read at all, which was not said with the name
/// of the component yet.
int raiseUnreadable(lua_State* state, const ComponentDescriptor& descriptor, const Error& error) {
    return luaL_error(state, "%s: %s", descriptor.type.name().c_str(), error.message.c_str());
}

/// What a script says a component is to be, at `index`: a table of properties - or, with
/// a name at `index` and a value after it, that one property.
Json checkValues(lua_State* state, int index, const ComponentDescriptor& descriptor) {
    if (lua_type(state, index) == LUA_TSTRING) {
        const PropertyInfo& property = checkProperty(state, index, descriptor);
        luaL_checkany(state, index + 1);
        Result<Json> value = readValueOf(state, index + 1, property.type(), property, 0);
        if (!value) {
            raiseUnreadable(state, descriptor, value.error().withContext(fmt::format("'{}'", property.name())));
            return Json();
        }
        Json values = Json::object();
        values[property.name()] = std::move(*value);
        return values;
    }
    Result<Json> values = readObjectOf(state, index, descriptor.type, 0);
    if (!values) {
        raiseUnreadable(state, descriptor, values.error());
        return Json();
    }
    return std::move(*values);
}

/// `value` as a script hands it back when it has changed nothing: without the members
/// that hold nothing, which are not in the table a script is given, and with the
/// members of every object in the order of their names - a table has no order, and
/// the engine's JSON keeps the one of the file. Two values that are the same to a
/// script are equal in this form, and only then.
Json asScriptsHandBack(const Json& value) {
    if (value.is_object()) {
        std::vector<std::string> names;
        for (const auto& [name, member] : value.items()) {
            if (!member.is_null()) {
                names.push_back(name);
            }
        }
        std::ranges::sort(names);
        Json object = Json::object();
        for (const std::string& name : names) {
            object[name] = asScriptsHandBack(value[name]);
        }
        return object;
    }
    if (value.is_array()) {
        Json array = Json::array();
        for (const Json& element : value) {
            array.push_back(asScriptsHandBack(element));
        }
        return array;
    }
    return value;
}

/// Takes out of `values` what gives a property the value it has, `held` being what the
/// properties hold now. A script that reads a component, changes something of it and
/// writes the table back has changed nothing of the rest: that is no attempt at what
/// may not be changed, and it does not take what a table cannot hold - a member that
/// holds nothing, the order of the members - out of data the script never touched.
void dropUnchanged(Json& values, const TypeInfo& type, const Json& held) {
    for (const PropertyInfo& property : type.properties()) {
        const auto given = values.find(property.name());
        const auto now = held.find(property.name());
        if (given == values.end() || now == held.end()) {
            continue;
        }
        if (asScriptsHandBack(*given) == asScriptsHandBack(*now)) {
            values.erase(given);
        } else if (property.objectType() != nullptr && given->is_object() && now->is_object()) {
            dropUnchanged(*given, *property.objectType(), *now);
        }
    }
}

int entitySet(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const ComponentDescriptor& descriptor = checkComponent(state, 2);
    Json values = checkValues(state, 3, descriptor);

    void* instance = descriptor.get(checkContext(state).world.registry(), entity.handle);
    if (instance == nullptr) {
        return luaL_error(state, "the entity has no %s; add() gives it one", descriptor.type.name().c_str());
    }
    Json held = Json::object();
    for (const auto& [name, value] : values.items()) {
        if (const PropertyInfo* property = descriptor.type.findProperty(name); property != nullptr) {
            held[name] = property->get(instance);
        }
    }
    dropUnchanged(values, descriptor.type, held);

    // Checked as a whole before any of it is written: a change that is refused changes nothing.
    const Result<PropertyPatch> patch = descriptor.type.parseEdit(values);
    if (!patch) {
        return raiseRefusal(state, patch.error());
    }
    patch->applyTo(instance);
    return 0;
}

int entityAdd(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const ComponentDescriptor& descriptor = checkComponent(state, 2);
    World& world = checkContext(state).world;
    if (!descriptor.traits.optional) {
        return luaL_error(state, "every entity has a %s; none is added", descriptor.type.name().c_str());
    }
    if (descriptor.has(world.registry(), entity.handle)) {
        return luaL_error(state, "the entity has a %s already; set() changes it", descriptor.type.name().c_str());
    }
    const Json values = lua_isnoneornil(state, 3) ? Json::object() : checkValues(state, 3, descriptor);
    const Result<PropertyPatch> patch = descriptor.type.parseEdit(values);
    if (!patch) {
        return raiseRefusal(state, patch.error());
    }
    descriptor.emplace(world.registry(), entity.handle, *patch);
    return 0;
}

int entityRemove(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const ComponentDescriptor& descriptor = checkComponent(state, 2);
    if (!descriptor.traits.optional) {
        return luaL_error(state, "every entity has a %s; it is not removed", descriptor.type.name().c_str());
    }
    descriptor.remove(checkContext(state).world.registry(), entity.handle);
    return 0;
}

constexpr std::array kComponentFunctions = {
    ScriptFunction{"has", "entity:has(component) -> boolean",
                   R"(Whether the entity has a component of that name: "Light", "RigidBody".)", entityHas},
    ScriptFunction{"get", "entity:get(component, property) -> table, value or nil",
                   "What a component of the entity holds: all its properties as a table, or the one that is named. "
                   "A copy - changing it changes nothing until it is given to set(). Vectors are Vec3, rotations "
                   "Quat, and an entity that a property refers to is that entity. nil when the entity has no such "
                   "component.",
                   entityGet},
    ScriptFunction{"set", "entity:set(component, values) or entity:set(component, property, value)",
                   "Changes properties of a component of the entity: those in a table of names, or the one that is "
                   "named. What is written is checked as the editor checks it - kind, range, whether it may be "
                   "changed at all - and a change that is refused changes nothing. A table that get() gave can be "
                   "given back with changes: what has the value it had is no change, also where it may not be "
                   "changed.",
                   entitySet},
    ScriptFunction{"add", "entity:add(component, values)",
                   "Gives the entity a component of that name, with the properties in `values` and the rest as a "
                   "new component has them. An error when it has one already.",
                   entityAdd},
    ScriptFunction{"remove", "entity:remove(component)",
                   "Takes the component of that name off the entity, if it has one. What every entity has - its "
                   "Transform - stays.",
                   entityRemove},
};

} // namespace

Status registerComponentApi(ScriptRuntime& runtime) {
    return runtime.registerLibrary("cordite.Entity", kComponentFunctions);
}

} // namespace cordite
