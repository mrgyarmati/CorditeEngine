#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {
namespace {

std::string_view checkText(lua_State* state, int index) {
    size_t length = 0;
    const char* text = luaL_checklstring(state, index, &length);
    return {text, length};
}

/// Pushes a list of entities, in the order given.
void pushEntities(lua_State* state, const std::vector<Entity>& entities) {
    lua_createtable(state, static_cast<int>(entities.size()), 0);
    lua_Integer position = 1;
    for (const Entity entity : entities) {
        pushEntity(state, entity);
        lua_rawseti(state, -2, position++);
    }
}

/// The entity at `index`, or the null entity for nil and for nothing.
Entity checkEntityOrNone(lua_State* state, int index) {
    return lua_isnoneornil(state, index) ? Entity{} : checkEntity(state, index);
}

// --- cordite.world ---------------------------------------------------------------------

int worldFind(lua_State* state) {
    pushEntity(state, checkContext(state).world.findByName(checkText(state, 1)));
    return 1;
}

int worldFindById(lua_State* state) {
    const std::string_view text = checkText(state, 1);
    const std::optional<Uuid> id = Uuid::parse(text);
    if (!id) {
        return luaL_argerror(state, 1, "the id of an entity expected, as entity:id() gives it");
    }
    pushEntity(state, checkContext(state).world.findById(*id));
    return 1;
}

int worldCreate(lua_State* state) {
    World& world = checkContext(state).world;
    const std::string name(lua_isnoneornil(state, 1) ? std::string_view("Entity") : checkText(state, 1));
    const Entity parent = checkEntityOrNone(state, 2);
    const Entity entity = world.createEntity(name);
    if (!parent.isNull()) {
        // A new entity is where its parent is: its own transform is none yet.
        if (const Status attached = world.setParent(entity, parent, false); !attached) {
            world.destroyEntity(entity);
            return luaL_error(state, "%s", attached.error().message.c_str());
        }
    }
    pushEntity(state, entity);
    return 1;
}

int worldRoots(lua_State* state) {
    pushEntities(state, checkContext(state).world.roots());
    return 1;
}

int worldWithComponent(lua_State* state) {
    const ScriptContext& context = checkContext(state);
    const std::string name(checkText(state, 1));
    const ComponentDescriptor* descriptor = context.components.find(name);
    if (descriptor == nullptr) {
        return luaL_argerror(state, 1, R"(the name of a component expected, such as "Light")");
    }
    // In the order of the hierarchy, as everything is that a script is given a list of.
    std::vector<Entity> entities;
    context.world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (descriptor->has(context.world.registry(), entity.handle)) {
            entities.push_back(entity);
        }
    });
    pushEntities(state, entities);
    return 1;
}

int worldComponentTypes(lua_State* state) {
    const auto components = checkContext(state).components.components();
    lua_createtable(state, static_cast<int>(components.size()), 0);
    lua_Integer position = 1;
    for (const ComponentDescriptor* descriptor : components) {
        const std::string& name = descriptor->type.name();
        lua_pushlstring(state, name.data(), name.size());
        lua_rawseti(state, -2, position++);
    }
    return 1;
}

constexpr std::array kWorldFunctions = {
    ScriptFunction{"find", "cordite.world.find(name) -> Entity or nil",
                   "The first entity of that name, in the order of the hierarchy; nil when there is none.", worldFind},
    ScriptFunction{"findById", "cordite.world.findById(id) -> Entity or nil",
                   "The entity with that id, as entity:id() gives it; nil when there is none.", worldFindById},
    ScriptFunction{"create", "cordite.world.create(name, parent) -> Entity",
                   "Makes an entity, at once. With a parent it hangs under that, where the parent is; without, it "
                   "is a root.",
                   worldCreate},
    ScriptFunction{"roots", "cordite.world.roots() -> {Entity}",
                   "The entities that hang under nothing, in their order.", worldRoots},
    ScriptFunction{"withComponent", "cordite.world.withComponent(component) -> {Entity}",
                   "Every entity that has a component of that name - \"Light\", \"Script\" - in the order of the "
                   "hierarchy.",
                   worldWithComponent},
    ScriptFunction{"componentTypes", "cordite.world.componentTypes() -> {string}",
                   "The name of every kind of component there is, in the order the engine knows them: what has(), "
                   "get() and withComponent() are asked with.",
                   worldComponentTypes},
};

// --- cordite.Entity: the hierarchy ---------------------------------------------------------

int entityParent(lua_State* state) {
    pushEntity(state, checkContext(state).world.parent(checkEntity(state, 1)));
    return 1;
}

int entitySetParent(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const Entity parent = checkEntityOrNone(state, 2);
    const bool keepWorldTransform = lua_isnoneornil(state, 3) || lua_toboolean(state, 3) != 0;
    if (const Status moved = checkContext(state).world.setParent(entity, parent, keepWorldTransform); !moved) {
        return luaL_error(state, "%s", moved.error().message.c_str());
    }
    return 0;
}

int entityChildren(lua_State* state) {
    pushEntities(state, checkContext(state).world.children(checkEntity(state, 1)));
    return 1;
}

// --- cordite.Entity: where it is -----------------------------------------------------------

Transform& transformOf(lua_State* state, Entity entity) {
    return checkContext(state).world.transform(entity);
}

/// What takes a point of the entity's parent to the world, as the transforms are now.
Mat4 parentToWorld(lua_State* state, Entity entity) {
    const World& world = checkContext(state).world;
    const Entity parent = world.parent(entity);
    return parent.isNull() ? Mat4(1.0f) : world.computeWorldMatrix(parent);
}

/// How the entity is turned in the world, as the transforms are now.
Quat worldRotationOf(lua_State* state, Entity entity) {
    return Transform::fromMatrix(checkContext(state).world.computeWorldMatrix(entity)).rotation;
}

int entityPosition(lua_State* state) {
    pushVec3(state, transformOf(state, checkEntity(state, 1)).position);
    return 1;
}

int entitySetPosition(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    transformOf(state, entity).position = checkVec3(state, 2);
    return 0;
}

int entityRotation(lua_State* state) {
    pushQuat(state, transformOf(state, checkEntity(state, 1)).rotation);
    return 1;
}

int entitySetRotation(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    transformOf(state, entity).rotation = checkQuat(state, 2);
    return 0;
}

int entityScale(lua_State* state) {
    pushVec3(state, transformOf(state, checkEntity(state, 1)).scale);
    return 1;
}

int entitySetScale(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    transformOf(state, entity).scale = checkVec3(state, 2);
    return 0;
}

int entityWorldPosition(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    pushVec3(state, Vec3(checkContext(state).world.computeWorldMatrix(entity)[3]));
    return 1;
}

int entitySetWorldPosition(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const Vec3 position = checkVec3(state, 2);
    const Mat4 toWorld = parentToWorld(state, entity);
    // What it hangs under may have no size along some axis, and then no place in it
    // leads to the one that is asked for. Judged against how large it is: what is
    // small all round is not flat.
    const Mat3 axes(toWorld);
    const float volume = glm::length(axes[0]) * glm::length(axes[1]) * glm::length(axes[2]);
    if (!(std::abs(glm::determinant(axes)) > 1.0e-6f * volume)) {
        return luaL_error(state, "the entity cannot be put there: what it hangs under has no size");
    }
    const Vec3 local = Vec3(glm::inverse(toWorld) * Vec4(position, 1.0f));
    if (!isFinite(local)) {
        return luaL_error(state, "the entity cannot be put there: the place is too far for what it hangs under");
    }
    transformOf(state, entity).position = local;
    return 0;
}

int entityWorldRotation(lua_State* state) {
    pushQuat(state, worldRotationOf(state, checkEntity(state, 1)));
    return 1;
}

int entityForward(lua_State* state) {
    pushVec3(state, worldRotationOf(state, checkEntity(state, 1)) * kWorldForward);
    return 1;
}

int entityRight(lua_State* state) {
    pushVec3(state, worldRotationOf(state, checkEntity(state, 1)) * kWorldRight);
    return 1;
}

int entityUp(lua_State* state) {
    pushVec3(state, worldRotationOf(state, checkEntity(state, 1)) * kWorldUp);
    return 1;
}

int entityTranslate(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const Vec3 moved = transformOf(state, entity).position + checkVec3(state, 2);
    if (!isFinite(moved)) {
        return luaL_argerror(state, 2, "the entity would be moved further than there are numbers for");
    }
    transformOf(state, entity).position = moved;
    return 0;
}

int entityRotate(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const Quat by = checkQuat(state, 2);
    Transform& transform = transformOf(state, entity);
    transform.rotation = glm::normalize(transform.rotation * by);
    return 0;
}

int entityLookAt(lua_State* state) {
    const Entity entity = checkEntity(state, 1);
    const Vec3 target = checkVec3(state, 2);
    const Vec3 up = lua_isnoneornil(state, 3) ? kWorldUp : checkVec3(state, 3);
    const World& world = checkContext(state).world;
    const Vec3 direction = target - Vec3(world.computeWorldMatrix(entity)[3]);
    if (!isFinite(direction) || glm::length(direction) < 1.0e-6f) {
        // Where it is, is where it is to look: any way of looking is as good as the one it has.
        return 0;
    }
    // Turned in the world as asked, which under a parent that is turned itself is
    // another turn of its own.
    const Entity parent = world.parent(entity);
    const Quat parentRotation = parent.isNull() ? kIdentityQuat : worldRotationOf(state, parent);
    transformOf(state, entity).rotation = glm::normalize(glm::inverse(parentRotation) * lookRotation(direction, up));
    return 0;
}

constexpr std::array kEntityFunctions = {
    ScriptFunction{"parent", "entity:parent() -> Entity or nil", "What the entity hangs under; nil for a root.",
                   entityParent},
    ScriptFunction{"setParent", "entity:setParent(parent, keepWorldTransform)",
                   "Hangs the entity under another, or under nothing for nil. It stays where it is in the world "
                   "unless `keepWorldTransform` is false, in which case it keeps its own position, rotation and "
                   "scale and moves with what it now hangs under. An entity cannot hang under itself, nor under "
                   "what hangs under it.",
                   entitySetParent},
    ScriptFunction{"children", "entity:children() -> {Entity}", "What hangs under the entity, in order.",
                   entityChildren},
    ScriptFunction{"position", "entity:position() -> Vec3",
                   "Where the entity is under what it hangs under, in metres. A copy: changing it moves nothing "
                   "until it is given to setPosition().",
                   entityPosition},
    ScriptFunction{"setPosition", "entity:setPosition(position)",
                   "Puts the entity at a place under what it hangs under.", entitySetPosition},
    ScriptFunction{"rotation", "entity:rotation() -> Quat", "How the entity is turned against what it hangs under.",
                   entityRotation},
    ScriptFunction{"setRotation", "entity:setRotation(rotation)", "Turns the entity against what it hangs under.",
                   entitySetRotation},
    ScriptFunction{"scale", "entity:scale() -> Vec3",
                   "How large the entity is along its three axes, against what it hangs under.", entityScale},
    ScriptFunction{"setScale", "entity:setScale(scale)", "Makes the entity that large along its three axes.",
                   entitySetScale},
    ScriptFunction{"worldPosition", "entity:worldPosition() -> Vec3",
                   "Where the entity is in the world, with everything it hangs under as it is at this moment.",
                   entityWorldPosition},
    ScriptFunction{"setWorldPosition", "entity:setWorldPosition(position)",
                   "Puts the entity at a place in the world, whatever it hangs under.", entitySetWorldPosition},
    ScriptFunction{"worldRotation", "entity:worldRotation() -> Quat",
                   "How the entity is turned in the world, with everything it hangs under as it is at this moment.",
                   entityWorldRotation},
    ScriptFunction{"forward", "entity:forward() -> Vec3",
                   "The direction the entity faces in the world: its -Z, of length one.", entityForward},
    ScriptFunction{"right", "entity:right() -> Vec3", "The entity's right in the world: its +X, of length one.",
                   entityRight},
    ScriptFunction{"up", "entity:up() -> Vec3", "The entity's up in the world: its +Y, of length one.", entityUp},
    ScriptFunction{"translate", "entity:translate(offset)",
                   "Moves the entity by an offset, in the directions of what it hangs under.", entityTranslate},
    ScriptFunction{"rotate", "entity:rotate(rotation)",
                   "Turns the entity further, about its own axes: its rotation times the one given.", entityRotate},
    ScriptFunction{"lookAt", "entity:lookAt(point, up)",
                   "Turns the entity so that it faces a point of the world, with its up as near to `up` - the "
                   "world's, when left out - as that allows.",
                   entityLookAt},
};

// --- cordite.time -------------------------------------------------------------------------

int timeSeconds(lua_State* state) {
    lua_pushnumber(state, static_cast<lua_Number>(ScriptSystem::of(state).seconds()));
    return 1;
}

int timeDelta(lua_State* state) {
    lua_pushnumber(state, static_cast<lua_Number>(ScriptSystem::of(state).deltaSeconds()));
    return 1;
}

int timeFrame(lua_State* state) {
    lua_pushinteger(state, static_cast<lua_Integer>(ScriptSystem::of(state).updates()));
    return 1;
}

constexpr std::array kTimeFunctions = {
    ScriptFunction{"seconds", "cordite.time.seconds() -> number",
                   "How long the game has been running, in seconds: the time of every frame so far, this one "
                   "included.",
                   timeSeconds},
    ScriptFunction{"delta", "cordite.time.delta() -> number",
                   "The time that passes with this frame, in seconds: what `update` is called with.", timeDelta},
    ScriptFunction{"frame", "cordite.time.frame() -> integer", "Which frame of the game this is, counted from 1.",
                   timeFrame},
};

// --- cordite.assets -----------------------------------------------------------------------

int assetsFind(lua_State* state) {
    const std::string reference(checkText(state, 1));
    const AssetInfo* asset = checkContext(state).assets.database().findByReference(reference);
    if (asset == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    const std::string id = asset->id.toString();
    lua_pushlstring(state, id.data(), id.size());
    return 1;
}

constexpr std::array kAssetFunctions = {
    ScriptFunction{"find", "cordite.assets.find(reference) -> string or nil",
                   "The id of an asset by where it is - \"models/crate.gltf#mesh/0\", \"builtin#mesh/cube\" - which "
                   "is what a component is given to name the asset; nil when there is none.",
                   assetsFind},
};

} // namespace

Status registerWorldApi(ScriptRuntime& runtime) {
    CORDITE_TRY(runtime.registerLibrary("cordite.world", kWorldFunctions));
    CORDITE_TRY(runtime.registerLibrary("cordite.Entity", kEntityFunctions));
    CORDITE_TRY(runtime.registerLibrary("cordite.time", kTimeFunctions));
    return runtime.registerLibrary("cordite.assets", kAssetFunctions);
}

} // namespace cordite
