#pragma once

/// @file
/// The components that put an entity into the simulation: a collider gives it a shape, a
/// rigid body makes it move.
///
/// @code
/// const Entity crate = world.createEntity("Crate");
/// world.add<ColliderComponent>(crate) = {.shape = ShapeType::Box, .size = Vec3(1.0f)};
/// world.add<RigidBodyComponent>(crate) = {.mass = 20.0f};
///
/// const Entity floor = world.createEntity("Floor");            // a collider alone stands still
/// world.add<ColliderComponent>(floor) = {.size = Vec3(20.0f, 1.0f, 20.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
///
/// physics.update(world, frameSeconds);                         // PhysicsSystem
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "Collider": {"shape": "capsule", "size": [1, 1, 1], "radius": 0.3, "height": 1.8,
///              "center": [0, 0.9, 0], "friction": 0.5, "restitution": 0},
/// "RigidBody": {"motion": "dynamic", "mass": 80, "linearDamping": 0.05, "angularDamping": 0.05,
///               "gravityScale": 1, "continuous": false, "allowSleep": true}
/// @endcode
///
/// What becomes a body:
///
/// - An entity with a RigidBody is a body, shaped by its own Collider and by the
///   Colliders of the entities below it that have no RigidBody of their own - so a body
///   of several shapes is an entity with colliders hanging under it.
/// - A Collider with no RigidBody on its entity or above it is a body that stands
///   still: the ground, a wall.
/// - A RigidBody without any Collider has no shape, and is no body.
///
/// A shape is as large as its entity is scaled - by its own scale and by the scales of
/// everything above it, multiplied: a box by each axis, a sphere by the largest of the
/// three, a capsule by Y in height and by the larger of X and Z in thickness.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/world/ComponentRegistry.h"

namespace cordite {

/// The shape of an entity, for collisions.
struct ColliderComponent {
    ShapeType shape = ShapeType::Box;
    /// Of a box: its edge lengths, in metres, before the entity's scale.
    Vec3 size{1.0f};
    /// Of a sphere and of a capsule, in metres.
    float radius = 0.5f;
    /// Of a capsule: from end to end along the entity's Y axis, in metres.
    float height = 2.0f;
    /// Where the middle of the shape is, in the space of the entity.
    Vec3 center{0.0f};
    /// How much the surface resists sliding: 0 ice, 1 rubber.
    float friction = 0.5f;
    /// How much speed is kept in a bounce: 0 none, 1 all of it.
    float restitution = 0.0f;
};

/// Makes an entity a body that the simulation moves, or that moves through it.
struct RigidBodyComponent {
    BodyMotion motion = BodyMotion::Dynamic;
    /// In kilograms.
    float mass = 1.0f;
    /// The share of its speed, and of its spin, that it loses in a second by itself.
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    /// How much gravity pulls at it: 1 as at everything, 0 not at all.
    float gravityScale = 1.0f;
    /// Checked against what it might pass through between two steps: for what is small
    /// and fast.
    bool continuous = false;
    /// It may fall asleep when it has come to rest.
    bool allowSleep = true;
    /// How fast it moves, in metres per second, and turns, in radians per second about
    /// each axis. Kept up to date by the physics system while time passes. Setting them
    /// gives a dynamic body those speeds; those of a kinematic body follow from how its
    /// entity is moved, and are not its to be given. Not saved.
    Vec3 linearVelocity{0.0f};
    Vec3 angularVelocity{0.0f};
};

/// Registers the components of the physics module: "Collider" and "RigidBody".
[[nodiscard]] Status registerPhysicsComponents(ComponentRegistry& registry);

} // namespace cordite
