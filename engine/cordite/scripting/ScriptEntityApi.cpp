#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/world/Entt.h"
#include "cordite/world/World.h"

#include <array>
#include <new>
#include <string>

namespace cordite {
namespace {

/// Where Lua keeps the metatable of entities, and the name they have in messages.
constexpr const char* kEntityType = "cordite.Entity";

/// Where Lua keeps the values that stand for entities, by the number of their handle:
/// a table that does not keep them alive.
constexpr const char* kEntityCache = "cordite.entities";

int entityIsValid(lua_State* state) {
    const Entity entity = checkEntityValue(state, 1);
    lua_pushboolean(state, checkContext(state).world.isValid(entity) ? 1 : 0);
    return 1;
}

int entityId(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const std::string id = checkContext(state).world.id(entity).toString();
    lua_pushlstring(state, id.data(), id.size());
    return 1;
}

int entityName(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const std::string& name = checkContext(state).world.name(entity);
    lua_pushlstring(state, name.data(), name.size());
    return 1;
}

int entitySetName(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    size_t length = 0;
    const char* name = luaL_checklstring(state, 2, &length);
    checkContext(state).world.setName(entity, std::string_view(name, length));
    return 0;
}

int entityIsEnabled(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    lua_pushboolean(state, checkContext(state).world.isEnabled(entity) ? 1 : 0);
    return 1;
}

int entitySetEnabled(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    checkContext(state).world.setEnabled(entity, lua_toboolean(state, 2) != 0);
    return 0;
}

int entityDestroy(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    ScriptSystem::of(state).destroyLater(entity);
    return 0;
}

/// What tostring() and print() make of an entity: its name, or that it is gone.
int entityToString(lua_State* state) {
    const Entity entity = checkEntityValue(state, 1);
    const World& world = checkContext(state).world;
    if (world.isValid(entity)) {
        lua_pushfstring(state, "Entity(%s)", world.name(entity).c_str());
    } else {
        lua_pushliteral(state, "Entity(gone)");
    }
    return 1;
}

constexpr std::array kEntityFunctions = {
    ScriptFunction{"isValid", "entity:isValid() -> boolean",
                   "Whether the entity is still there. Everything else that is asked of an entity which is gone is "
                   "an error.",
                   entityIsValid},
    ScriptFunction{"id", "entity:id() -> string",
                   "The id of the entity: the one thing about it that stays the same when the scene is saved and "
                   "loaded again.",
                   entityId},
    ScriptFunction{"name", "entity:name() -> string", "The name of the entity, which need not be its alone.",
                   entityName},
    ScriptFunction{"setName", "entity:setName(name)", "Gives the entity another name.", entitySetName},
    ScriptFunction{"isEnabled", "entity:isEnabled() -> boolean",
                   "Whether the entity itself is switched on. One that is may still be off because something it "
                   "hangs under is.",
                   entityIsEnabled},
    ScriptFunction{"setEnabled", "entity:setEnabled(enabled)",
                   "Switches the entity on or off, and with it everything below it: what is off is not drawn, not "
                   "simulated, and its scripts are not called.",
                   entitySetEnabled},
    ScriptFunction{"destroy", "entity:destroy()",
                   "Destroys the entity and everything below it once the scripts of this frame have run. Until "
                   "then it is as valid as ever; the scripts in there have their `destroy` called first.",
                   entityDestroy},
    ScriptFunction{"__tostring", "tostring(entity) -> string",
                   "What print() shows for an entity: its name, or that it is gone.", entityToString},
};

} // namespace

const ScriptContext& checkContext(lua_State* state) {
    const ScriptContext* context = ScriptSystem::of(state).context();
    if (context == nullptr) {
        luaL_error(state, "there is no world to reach from here: no update of the scripts is under way");
    }
    return *context;
}

void pushEntity(lua_State* state, Entity entity) {
    if (entity.isNull()) {
        lua_pushnil(state);
        return;
    }
    luaL_checkstack(state, 3, "the scripts are out of stack");
    lua_getfield(state, LUA_REGISTRYINDEX, kEntityCache);
    const auto key = static_cast<lua_Integer>(entt::to_integral(entity.handle));
    if (lua_rawgeti(state, -1, key) != LUA_TUSERDATA) {
        lua_pop(state, 1);
        new (lua_newuserdatauv(state, sizeof(Entity), 0)) Entity(entity);
        luaL_setmetatable(state, kEntityType);
        lua_pushvalue(state, -1);
        lua_rawseti(state, -3, key);
    }
    lua_remove(state, -2);
}

Entity checkEntityValue(lua_State* state, int index) {
    const void* entity = luaL_testudata(state, index, kEntityType);
    if (entity == nullptr) {
        luaL_typeerror(state, index, "Entity");
    }
    return *static_cast<const Entity*>(entity);
}

Entity checkEntity(lua_State* state, int index) {
    const Entity entity = checkEntityValue(state, index);
    if (!checkContext(state).world.isValid(entity)) {
        luaL_argerror(state, index, "the entity is gone: it was destroyed");
    }
    return entity;
}

Status registerEntityApi(ScriptRuntime& runtime) {
    CORDITE_TRY(runtime.registerLibrary(kEntityType, kEntityFunctions));
    return runtime.protect([](lua_State* state) {
        // The table of the functions is the metatable of every entity, and where their
        // methods are looked up.
        lua_getglobal(state, "cordite");
        lua_getfield(state, -1, "Entity");
        lua_pushvalue(state, -1);
        lua_setfield(state, -2, "__index");
        // What Lua calls the kind of value in what it says: "attempt to index a Entity
        // value".
        lua_pushliteral(state, "Entity");
        lua_setfield(state, -2, "__name");
        lua_setfield(state, LUA_REGISTRYINDEX, kEntityType);
        lua_pop(state, 1);

        // Values by handle; an entity nobody holds on to is not kept.
        lua_newtable(state);
        lua_newtable(state);
        lua_pushliteral(state, "v");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);
        lua_setfield(state, LUA_REGISTRYINDEX, kEntityCache);
    });
}

} // namespace cordite
