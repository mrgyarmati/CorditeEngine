#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/scripting/Lua.h"
#include "cordite/scripting/ScriptBindings.h"
#include "cordite/world/World.h"

#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace cordite {
namespace {

// Scripts ask the simulation what a ray hits and what is within reach, and push its
// bodies. To a script a body is the entity that has it: what comes back names entities,
// and what is pushed is an entity that has a body.
//
// Scripts run before the simulation moves on in a frame. What they are told is how
// things stood when the last frame ended: an entity a script has moved since is found
// where it was, and one it has only just made has no body yet.

/// How far a ray reaches, in metres, when a script does not say.
constexpr float kDefaultReach = 1000.0f;

/// The simulation of the update that is under way. Raises where there is none.
PhysicsSystem& checkPhysics(lua_State* state) {
    PhysicsSystem* physics = checkContext(state).physics;
    if (physics == nullptr) {
        luaL_error(state, "nothing is simulated here: this world has no physics to ask");
    }
    return *physics;
}

/// A distance or a radius at `index`, in metres: a number from 0 up, and finite as the
/// float the engine keeps. `what` says in a message what it is.
float checkLength(lua_State* state, int index, const char* what) {
    const auto number = static_cast<double>(luaL_checknumber(state, index));
    const auto length = static_cast<float>(number);
    if (!(number >= 0.0) || !std::isfinite(length)) {
        luaL_argerror(state, index, lua_pushfstring(state, "%s is a number from 0 up", what));
    }
    return length;
}

/// A direction at `index`: a vector of some length.
Vec3 checkDirection(lua_State* state, int index) {
    const Vec3 direction = checkVec3(state, index);
    if (!(glm::length(direction) > 1.0e-6f)) {
        luaL_argerror(state, index, "a direction: a vector of some length");
    }
    return direction;
}

/// The entity of a body, or the null entity for a body whose entity is gone - destroyed
/// since the simulation last looked.
Entity entityOf(lua_State* state, const PhysicsSystem& physics, BodyId body) {
    const Entity entity = physics.entityOf(body);
    return checkContext(state).world.isValid(entity) ? entity : Entity{};
}

/// Pushes what a ray or a ball hit - whose body, where, which way the surface faces
/// there and how far it was - or nil for nothing.
void pushHit(lua_State* state, const PhysicsSystem& physics, const std::optional<RayHit>& hit) {
    if (!hit) {
        lua_pushnil(state);
        return;
    }
    lua_createtable(state, 0, 4);
    pushEntity(state, entityOf(state, physics, hit->body));
    lua_setfield(state, -2, "entity");
    pushVec3(state, hit->position);
    lua_setfield(state, -2, "position");
    pushVec3(state, hit->normal);
    lua_setfield(state, -2, "normal");
    lua_pushnumber(state, static_cast<lua_Number>(scriptNumber(hit->distance)));
    lua_setfield(state, -2, "distance");
}

/// Pushes the entities of bodies, in the order of the bodies, each once.
void pushEntitiesOf(lua_State* state, const PhysicsSystem& physics, const std::vector<BodyId>& bodies) {
    lua_createtable(state, static_cast<int>(bodies.size()), 0);
    lua_Integer position = 1;
    for (const BodyId body : bodies) {
        const Entity entity = entityOf(state, physics, body);
        if (!entity.isNull()) {
            pushEntity(state, entity);
            lua_rawseti(state, -2, position++);
        }
    }
}

int physicsRaycast(lua_State* state) {
    const PhysicsSystem& physics = checkPhysics(state);
    const Vec3 origin = checkVec3(state, 1);
    const Vec3 direction = checkDirection(state, 2);
    const float reach = lua_isnoneornil(state, 3) ? kDefaultReach : checkLength(state, 3, "how far a ray reaches");
    pushHit(state, physics, physics.simulation().raycast(Ray{.origin = origin, .direction = direction}, reach));
    return 1;
}

int physicsSphereCast(lua_State* state) {
    const PhysicsSystem& physics = checkPhysics(state);
    const Vec3 origin = checkVec3(state, 1);
    const Vec3 direction = checkDirection(state, 2);
    const float radius = checkLength(state, 3, "the radius of a ball");
    const float reach = lua_isnoneornil(state, 4) ? kDefaultReach : checkLength(state, 4, "how far a ball is moved");
    pushHit(state, physics,
            physics.simulation().sphereCast(Ray{.origin = origin, .direction = direction}, radius, reach));
    return 1;
}

int physicsOverlapSphere(lua_State* state) {
    const PhysicsSystem& physics = checkPhysics(state);
    const Vec3 center = checkVec3(state, 1);
    const float radius = checkLength(state, 2, "the radius of a ball");
    pushEntitiesOf(state, physics, physics.simulation().overlapSphere(center, radius));
    return 1;
}

int physicsOverlapBox(lua_State* state) {
    const PhysicsSystem& physics = checkPhysics(state);
    const Vec3 center = checkVec3(state, 1);
    const Vec3 size = checkVec3(state, 2);
    if (size.x < 0.0f || size.y < 0.0f || size.z < 0.0f) {
        return luaL_argerror(state, 2, "the size of a box: three numbers from 0 up");
    }
    const Quat rotation = lua_isnoneornil(state, 3) ? kIdentityQuat : checkQuat(state, 3);
    pushEntitiesOf(state, physics, physics.simulation().overlapBox(center, size, rotation));
    return 1;
}

int physicsGravity(lua_State* state) {
    pushVec3(state, checkPhysics(state).simulation().gravity());
    return 1;
}

int physicsSetGravity(lua_State* state) {
    PhysicsSystem& physics = checkPhysics(state);
    physics.simulation().setGravity(checkVec3(state, 1));
    return 0;
}

constexpr std::array kPhysicsFunctions = {
    ScriptFunction{"raycast", "cordite.physics.raycast(origin, direction, reach) -> hit or nil",
                   "The first body a ray hits within `reach` metres - 1000 when left out - as a table: `entity`, "
                   "whose body it is, `position`, the `normal` of the surface there, which points out of the body, "
                   "and `distance`, in metres. nil when it hits nothing. Triggers are not hit. Bodies are where "
                   "the last frame left them.",
                   physicsRaycast},
    ScriptFunction{"sphereCast", "cordite.physics.sphereCast(origin, direction, radius, reach) -> hit or nil",
                   "The first body a ball of `radius` touches when its middle is moved along a ray, as raycast() "
                   "gives it: `distance` is how far the middle got, `position` where ball and body touch, and "
                   "`normal` points from the body to the ball.",
                   physicsSphereCast},
    ScriptFunction{"overlapSphere", "cordite.physics.overlapSphere(center, radius) -> {Entity}",
                   "The entities whose bodies reach into a ball, each once, in an order that is the same every "
                   "time. Triggers are not among them.",
                   physicsOverlapSphere},
    ScriptFunction{"overlapBox", "cordite.physics.overlapBox(center, size, rotation) -> {Entity}",
                   "The entities whose bodies reach into a box of `size` metres along its three axes, turned by "
                   "`rotation` about its middle when one is given.",
                   physicsOverlapBox},
    ScriptFunction{"gravity", "cordite.physics.gravity() -> Vec3",
                   "What pulls at every body, in metres per second squared: (0, -9.81, 0) unless it was changed.",
                   physicsGravity},
    ScriptFunction{"setGravity", "cordite.physics.setGravity(gravity)",
                   "Changes what pulls at every body, from the next step of the simulation on.", physicsSetGravity},
};

// --- cordite.Entity: pushing its body ---------------------------------------------------------

/// The body of the entity at `index`. Raises for an entity that has none.
BodyId checkBody(lua_State* state, int index, const PhysicsSystem& physics) {
    const BodyId body = physics.bodyOf(checkEntity(state, index));
    if (body.isNull()) {
        luaL_error(state, "the entity has no body to push: it needs a Collider, and has a body from the frame "
                          "after it was given one");
    }
    return body;
}

int entityAddForce(lua_State* state) {
    PhysicsSystem& physics = checkPhysics(state);
    const BodyId body = checkBody(state, 1, physics);
    physics.simulation().addForce(body, checkVec3(state, 2));
    return 0;
}

int entityAddImpulse(lua_State* state) {
    PhysicsSystem& physics = checkPhysics(state);
    const BodyId body = checkBody(state, 1, physics);
    physics.simulation().addImpulse(body, checkVec3(state, 2));
    return 0;
}

int entityAddTorque(lua_State* state) {
    PhysicsSystem& physics = checkPhysics(state);
    const BodyId body = checkBody(state, 1, physics);
    physics.simulation().addTorque(body, checkVec3(state, 2));
    return 0;
}

constexpr std::array kEntityFunctions = {
    ScriptFunction{"addForce", "entity:addForce(force)",
                   "Pushes the body of the entity at its centre with a force, in newtons, for the next step of the "
                   "simulation; a script that pushes on calls this every frame. Only a dynamic body is moved by "
                   "it. An error for an entity without a body.",
                   entityAddForce},
    ScriptFunction{"addImpulse", "entity:addImpulse(impulse)",
                   "Knocks the body of the entity at its centre, in newton seconds: its speed changes at once, by "
                   "the impulse divided by its mass. Only a dynamic body is moved by it.",
                   entityAddImpulse},
    ScriptFunction{"addTorque", "entity:addTorque(torque)",
                   "Twists the body of the entity about its centre, in newton metres about each axis, for the next "
                   "step of the simulation. Only a dynamic body is turned by it.",
                   entityAddTorque},
};

} // namespace

Status registerPhysicsApi(ScriptRuntime& runtime) {
    CORDITE_TRY(runtime.registerLibrary("cordite.physics", kPhysicsFunctions));
    return runtime.registerLibrary("cordite.Entity", kEntityFunctions);
}

} // namespace cordite
