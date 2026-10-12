#pragma once

/// @file
/// The components that put an entity into the simulation: a collider gives it a shape, a
/// rigid body makes it move, a joint holds it to another - and a character controller
/// makes it something that walks.
///
/// @code
/// const Entity crate = world.createEntity("Crate");
/// world.add<ColliderComponent>(crate) = {.shape = ShapeType::Box, .size = Vec3(1.0f)};
/// world.add<RigidBodyComponent>(crate) = {.mass = 20.0f};
///
/// const Entity floor = world.createEntity("Floor");            // a collider alone stands still
/// world.add<ColliderComponent>(floor) = {.size = Vec3(20.0f, 1.0f, 20.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
///
/// const Entity door = world.createEntity("Door");               // hinged to what stands still
/// world.add<ColliderComponent>(door) = {.size = Vec3(1.0f, 2.0f, 0.1f)};
/// world.add<RigidBodyComponent>(door);
/// world.add<JointComponent>(door) = {.type = JointType::Hinge,
///                                    .connectedTo = world.id(floor),
///                                    .anchor = Vec3(-0.5f, 0.0f, 0.0f),
///                                    .limited = true,
///                                    .minimum = -90.0f,
///                                    .maximum = 90.0f};
///
/// const Entity guard = world.createEntity("Guard");             // walks where it is told
/// world.add<CharacterControllerComponent>(guard);
/// world.tryGet<CharacterControllerComponent>(guard)->movement = Vec3(1.5f, 0.0f, 0.0f);
///
/// physics.update(world, assets, frameSeconds);                 // PhysicsSystem
/// if (world.tryGet<CharacterControllerComponent>(guard)->ground == GroundState::OnGround) {
///     world.tryGet<CharacterControllerComponent>(guard)->jump = 4.0f;
/// }
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "Collider": {"shape": "capsule", "size": [1, 1, 1], "radius": 0.3, "height": 1.8,
///              "mesh": null, "trigger": false, "center": [0, 0.9, 0], "friction": 0.5,
///              "restitution": 0},
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
/// - A Collider that is a trigger is never part of another body: it is one of its own
///   that nothing runs into, and that says what moves into it and out of it.
///
/// A Joint holds the body its entity is part of to the body of another entity, or to
/// the world: as the two are when the simulation first meets the joint. From then on
/// it is that - how they stood to each other at that moment - which a hinge is turned
/// against and a slider has slid from, and of which its anchor, its axis and its
/// limits are said: also those it is given later. Only a Joint that is told to hold to
/// another entity begins again, from how the two stand then.
///
/// A CharacterController makes its entity a character: an upright capsule with its
/// soles where the entity is, which walks as `movement` says and jumps when `jump` is
/// given a speed. It is no body: it does not fall over, and it has a capsule of its
/// own - an entity that has one has no RigidBody and no Collider but a trigger, and
/// below it only triggers and rigid bodies hang. The simulation moves the entity and
/// leaves it turned as it is: which way a character looks is for whoever walks it.
///
/// A shape is as large as its entity is scaled - by its own scale and by the scales of
/// everything above it, multiplied: a box by each axis, a sphere by the largest of the
/// three, a capsule - and a character - by Y in height and by the larger of X and Z in
/// thickness, the hull around a mesh and the triangles of one by each axis.
///
/// Shapes made of a mesh: a convex hull is the smallest shape without dents around the
/// mesh - a solid that anything can be made of. The triangles of a mesh are the mesh
/// itself, dents and holes and all, but a surface without an inside: only for what the
/// simulation does not move - static, or kinematic - and stopped only from the side
/// its triangles face.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <string>
#include <vector>

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
    /// Of a convex hull and of a mesh: the mesh asset the shape is made of.
    Uuid mesh;
    /// It stops nothing: it only notices the bodies that move into it and out of it
    /// (PhysicsSystem::events()). A trigger is a body of its own, wherever its entity
    /// is in the hierarchy, and goes where its entity goes.
    bool trigger = false;
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

/// Holds the body an entity is part of to the body of another entity, or to the world.
struct JointComponent {
    JointType type = JointType::Fixed;
    /// The entity whose body this entity's is held to. None: the world.
    Uuid connectedTo;
    /// Where the joint is, in the space of this entity: the point about which a ball
    /// turns, a point on the axis of a hinge or a slider, this end of a distance.
    Vec3 anchor{0.0f};
    /// The axis a hinge turns about and a slider slides along, in the space of this
    /// entity; of any length but none.
    Vec3 axis{0.0f, 1.0f, 0.0f};
    /// The other end of a distance: in the space of the entity it is held to, or in
    /// the world.
    Vec3 connectedAnchor{0.0f};
    /// Whether what the joint leaves free is limited, and to what: a hinge in degrees,
    /// from at most 0 to at least 0, no further than half a turn either way; a slider
    /// in metres, likewise around 0; a distance in metres between its ends, from 0 up.
    /// A distance that is not limited keeps its ends as far apart as they were when it
    /// first held them.
    bool limited = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
    /// How a distance, or the limit of a hinge or a slider, gives: the frequency in
    /// hertz at which it swings - 0 for not at all - and how fast that dies away, from
    /// 0 for never to 1 for at once.
    float frequency = 0.0f;
    float damping = 0.0f;
    /// Whether the two bodies collide with each other.
    bool collideConnected = false;

    bool operator==(const JointComponent&) const = default;
};

/// Makes an entity a character: a capsule that stands upright with its soles where the
/// entity is, and walks where it is told.
struct CharacterControllerComponent {
    /// The capsule: how far around, and how tall from the soles to the top of the head,
    /// in metres - before the entity's scale. One that is no taller than it is wide is
    /// a ball.
    float radius = 0.3f;
    float height = 1.8f;
    /// In kilograms: how hard it presses on what it stands on.
    float mass = 70.0f;
    /// The steepest ground it walks on, in degrees from level; on steeper ground it does
    /// not get up, and slides down.
    float maxSlope = 50.0f;
    /// The highest step it walks up as if it were a ramp, in metres - before the
    /// entity's scale; 0 for none.
    float stepHeight = 0.35f;
    /// The most force it pushes a dynamic body in its way with, in newtons.
    float pushForce = 100.0f;
    /// How much gravity pulls at it: 1 as at everything, 0 not at all.
    float gravityScale = 1.0f;
    /// How much say `movement` has while it is in the air, from 0 to 1: its speed
    /// sideways is that much of `movement`, and the rest of what it left the ground with.
    float airControl = 1.0f;

    /// What it is told, and not part of a scene: the speed it is to walk with along the
    /// ground, in metres per second, in the world - what of it points up or down is
    /// left out - until it is told otherwise.
    Vec3 movement{0.0f};
    /// The speed it is to leave the ground with, in metres per second. The simulation
    /// takes it - and sets this back to nothing - with the next update, and the
    /// character jumps in the step after that if it stands on ground then.
    float jump = 0.0f;

    /// What the simulation says after every update, and not part of a scene: how fast
    /// it moved, what it has under its feet, which way that faces, and the entity it
    /// stands on - the one that has the body, or is the character; none in the air.
    Vec3 velocity{0.0f};
    GroundState ground = GroundState::InAir;
    Vec3 groundNormal{0.0f, 1.0f, 0.0f};
    Uuid groundEntity;

    bool operator==(const CharacterControllerComponent&) const = default;
};

/// Registers the components of the physics module: "Collider", "RigidBody", "Joint" and
/// "CharacterController".
[[nodiscard]] Status registerPhysicsComponents(ComponentRegistry& registry);

/// The entity whose body an entity is part of: the nearest at or above it that has a
/// RigidBody, or - with none - the entity itself where a Collider makes it a body that
/// stands still. None otherwise. Triggers, which are bodies of their own beside those,
/// are not what is asked about.
[[nodiscard]] Entity bodyEntityOf(const World& world, Entity entity);

/// The character that an entity hangs on: the nearest above it that has a
/// CharacterController, with no RigidBody at the entity or between the two - what is
/// part of a rigid body is that body's. None otherwise.
[[nodiscard]] Entity characterAbove(const World& world, Entity entity);

/// Something about the physics components of an entity that will not work as it stands.
struct PhysicsProblem {
    Entity entity;
    /// One sentence, naming the component and the property.
    std::string message;
};

/// What can be told of the colliders, joints and characters of a world without making
/// bodies of them, in the order of the hierarchy. Colliders: shapes made of meshes that
/// name no mesh, and the triangles of meshes in bodies that the simulation is to move.
/// Joints: one to an entity that is not there, on an entity that is part of no body or
/// to one that is part of none, between two parts of one body, between bodies of which
/// none is dynamic, with an axis of no length, and with limits that its kind does not
/// have. Characters: one whose entity is a body as well - it has a RigidBody, or a
/// Collider that is no trigger - and a collider that hangs on a character without
/// being part of a rigid body, which would stand still and be carried about.
/// What only shows when a shape is made - a mesh that cannot be read, points with no
/// volume between them - is said by the physics system when it is. Entities that are
/// switched off are checked like any other.
[[nodiscard]] std::vector<PhysicsProblem> findPhysicsProblems(const World& world);

} // namespace cordite
