#include "cordite/physics/PhysicsComponents.h"

#include "cordite/core/Reflection.h"
#include "cordite/math/MathReflection.h"

#include <fmt/format.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The largest a shape may be along one axis, in metres: beyond any scene, and well
/// inside what the arithmetic of collisions can tell apart.
constexpr double kMaxShapeExtent = 10000.0;
/// The smallest: a millimetre.
constexpr double kMinShapeExtent = 0.001;

TypeInfo describeCollider() {
    return TypeBuilder<ColliderComponent>("Collider")
        .enumProperty("shape", &ColliderComponent::shape,
                      {{ShapeType::Box, std::string(nameOf(ShapeType::Box))},
                       {ShapeType::Sphere, std::string(nameOf(ShapeType::Sphere))},
                       {ShapeType::Capsule, std::string(nameOf(ShapeType::Capsule))},
                       {ShapeType::ConvexHull, std::string(nameOf(ShapeType::ConvexHull))},
                       {ShapeType::Mesh, std::string(nameOf(ShapeType::Mesh))}},
                      {.description = "Box: of `size`. Sphere: of `radius`. Capsule: a cylinder along Y with round "
                                      "ends, `radius` thick and `height` from end to end. ConvexHull: the smallest "
                                      "shape without dents around `mesh`. Mesh: the triangles of `mesh`, a surface "
                                      "without an inside - not for a dynamic body"})
        .property("size", &ColliderComponent::size,
                  {.description = "Box: its edge lengths, before the entity's scale",
                   .minimum = kMinShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("radius", &ColliderComponent::radius,
                  {.description = "Sphere and capsule: half their thickness",
                   .minimum = 0.5 * kMinShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("height", &ColliderComponent::height,
                  {.description = "Capsule: from end to end; no taller than it is wide, it is a sphere",
                   .minimum = kMinShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("mesh", &ColliderComponent::mesh,
                  {.description = "Convex hull and mesh: the mesh the shape is made of",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Mesh"})
        .property("trigger", &ColliderComponent::trigger,
                  {.description = "It stops nothing: it only notices the bodies that move into it and out of "
                                  "it. A body of its own, which goes where its entity goes"})
        .property("center", &ColliderComponent::center,
                  {.description = "Where the middle of the shape is, in the space of the entity",
                   .minimum = -kMaxShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("friction", &ColliderComponent::friction,
                  {.description = "How much the surface resists sliding: 0 ice, 1 rubber. Of two that touch, "
                                  "the geometric mean counts",
                   .minimum = 0.0,
                   .maximum = 4.0,
                   .step = 0.01})
        .property("restitution", &ColliderComponent::restitution,
                  {.description = "How much speed is kept in a bounce: 0 none, 1 all of it. Of two that touch, "
                                  "the larger counts",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .build();
}

TypeInfo describeRigidBody() {
    return TypeBuilder<RigidBodyComponent>("RigidBody")
        .enumProperty("motion", &RigidBodyComponent::motion,
                      {{BodyMotion::Static, std::string(nameOf(BodyMotion::Static))},
                       {BodyMotion::Kinematic, std::string(nameOf(BodyMotion::Kinematic))},
                       {BodyMotion::Dynamic, std::string(nameOf(BodyMotion::Dynamic))}},
                      {.description = "Static: it does not move. Kinematic: it goes where its entity is put, and "
                                      "pushes what is in its way. Dynamic: gravity and what it collides with "
                                      "move it, and its entity follows"})
        .property(
            "mass", &RigidBodyComponent::mass,
            {.description = "Of a dynamic body", .minimum = 0.001, .maximum = 1000000.0, .step = 0.1, .unit = "kg"})
        .property("linearDamping", &RigidBodyComponent::linearDamping,
                  {.description = "The share of its speed it loses in a second by itself",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("angularDamping", &RigidBodyComponent::angularDamping,
                  {.description = "The share of its spin it loses in a second by itself",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("gravityScale", &RigidBodyComponent::gravityScale,
                  {.description = "How much gravity pulls at it: 1 as at everything, 0 not at all, negative upwards",
                   .minimum = -100.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("continuous", &RigidBodyComponent::continuous,
                  {.description = "Checked against what it might pass through between two steps, at some cost: "
                                  "for what is small and fast"})
        .property("allowSleep", &RigidBodyComponent::allowSleep,
                  {.description = "It may fall asleep when it has come to rest, and then costs nothing until "
                                  "something touches it"})
        .property("linearVelocity", &RigidBodyComponent::linearVelocity,
                  {.description = "How fast it moves; kept up to date while the simulation runs. Setting it "
                                  "gives a dynamic body that speed",
                   .minimum = -100000.0,
                   .maximum = 100000.0,
                   .step = 0.01,
                   .unit = "m/s",
                   .transient = true})
        .property("angularVelocity", &RigidBodyComponent::angularVelocity,
                  {.description = "How fast it turns about each axis; kept up to date while the simulation runs. "
                                  "Setting it gives a dynamic body that spin",
                   .minimum = -100000.0,
                   .maximum = 100000.0,
                   .step = 0.01,
                   .unit = "rad/s",
                   .transient = true})
        .build();
}

TypeInfo describeJoint() {
    return TypeBuilder<JointComponent>("Joint")
        .enumProperty("type", &JointComponent::type,
                      {{JointType::Fixed, std::string(nameOf(JointType::Fixed))},
                       {JointType::Ball, std::string(nameOf(JointType::Ball))},
                       {JointType::Hinge, std::string(nameOf(JointType::Hinge))},
                       {JointType::Slider, std::string(nameOf(JointType::Slider))},
                       {JointType::Distance, std::string(nameOf(JointType::Distance))}},
                      {.description = "Fixed: the two move as one. Ball: held at `anchor`, turning freely. "
                                      "Hinge: turning about `axis` through `anchor`. Slider: sliding along "
                                      "`axis` without turning. Distance: `anchor` and `connectedAnchor` kept "
                                      "a distance apart - a rod, a rope, a spring"})
        .property("connectedTo", &JointComponent::connectedTo,
                  {.description = "The entity whose body this entity's body is held to; none holds it to the world",
                   .semantic = PropertySemantic::EntityReference})
        .property("anchor", &JointComponent::anchor,
                  {.description = "Where the joint is, in the space of this entity",
                   .minimum = -kMaxShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("axis", &JointComponent::axis,
                  {.description = "Hinge: the axis it turns about. Slider: the axis it slides along. In the space "
                                  "of this entity, of any length but none",
                   .minimum = -1000.0,
                   .maximum = 1000.0,
                   .step = 0.01})
        .property("connectedAnchor", &JointComponent::connectedAnchor,
                  {.description = "Distance: the other end, in the space of the entity it is held to - or in the "
                                  "world",
                   .minimum = -kMaxShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.01,
                   .unit = "m"})
        .property("limited", &JointComponent::limited,
                  {.description = "Whether a hinge, a slider or a distance is kept between `minimum` and "
                                  "`maximum`. A distance that is not keeps its ends as far apart as they were "
                                  "when it first held them"})
        .property("minimum", &JointComponent::minimum,
                  {.description = "Hinge: degrees, from -180 to 0. Slider: metres, at most 0. Distance: metres "
                                  "between the ends, at least 0",
                   .minimum = -kMaxShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.1})
        .property("maximum", &JointComponent::maximum,
                  {.description = "Hinge: degrees, from 0 to 180. Slider: metres, at least 0. Distance: metres "
                                  "between the ends, no less than `minimum`",
                   .minimum = -kMaxShapeExtent,
                   .maximum = kMaxShapeExtent,
                   .step = 0.1})
        .property("frequency", &JointComponent::frequency,
                  {.description = "How a distance, or a limit, gives: the frequency at which it swings; 0 for not "
                                  "at all",
                   .minimum = 0.0,
                   .maximum = 1000.0,
                   .step = 0.1,
                   .unit = "Hz"})
        .property("damping", &JointComponent::damping,
                  {.description = "How fast that swinging dies away: 0 never, 1 at once",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("collideConnected", &JointComponent::collideConnected,
                  {.description = "Whether the two bodies collide with each other. Mostly they must not: what is "
                                  "joined is close together"})
        .build();
}

TypeInfo describeCharacterController() {
    return TypeBuilder<CharacterControllerComponent>("CharacterController")
        .property("radius", &CharacterControllerComponent::radius,
                  {.description = "How far around the capsule is, before the entity's scale",
                   .minimum = static_cast<double>(kMinCharacterRadius),
                   .maximum = 100.0,
                   .step = 0.01,
                   .unit = "m"})
        .property("height", &CharacterControllerComponent::height,
                  {.description = "From its soles, which are where the entity is, to the top of its head, "
                                  "before the entity's scale",
                   .minimum = 0.01,
                   .maximum = 1000.0,
                   .step = 0.01,
                   .unit = "m"})
        .property("mass", &CharacterControllerComponent::mass,
                  {.description = "How hard it presses on what it stands on",
                   .minimum = 0.001,
                   .maximum = 1000000.0,
                   .step = 0.1,
                   .unit = "kg"})
        .property("maxSlope", &CharacterControllerComponent::maxSlope,
                  {.description = "The steepest ground it walks on, from level; on steeper ground it does not "
                                  "get up, and slides down",
                   .minimum = 0.0,
                   .maximum = 89.0,
                   .step = 1.0,
                   .unit = "deg"})
        .property("stepHeight", &CharacterControllerComponent::stepHeight,
                  {.description = "The highest step it walks up as if it were a ramp, before the entity's "
                                  "scale; 0 for none",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.01,
                   .unit = "m"})
        .property("pushForce", &CharacterControllerComponent::pushForce,
                  {.description = "The most force it pushes a dynamic body in its way with",
                   .minimum = 0.0,
                   .maximum = 1000000000.0,
                   .step = 1.0,
                   .unit = "N"})
        .property("gravityScale", &CharacterControllerComponent::gravityScale,
                  {.description = "How much gravity pulls at it: 1 as at everything, 0 not at all",
                   .minimum = -100.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("airControl", &CharacterControllerComponent::airControl,
                  {.description = "How much say `movement` has while it is in the air: its speed sideways is "
                                  "that much of `movement`, and the rest of what it left the ground with",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("movement", &CharacterControllerComponent::movement,
                  {.description = "The speed it is to walk with along the ground, in the world, until it is "
                                  "told otherwise; what of it points up or down is left out",
                   .minimum = -100000.0,
                   .maximum = 100000.0,
                   .step = 0.01,
                   .unit = "m/s",
                   .transient = true})
        .property("jump", &CharacterControllerComponent::jump,
                  {.description = "The speed it is to leave the ground with. Taken by the next update, which "
                                  "sets this back to 0; it jumps in the step after if it stands on ground then",
                   .minimum = 0.0,
                   .maximum = 100000.0,
                   .step = 0.01,
                   .unit = "m/s",
                   .transient = true})
        .property("velocity", &CharacterControllerComponent::velocity,
                  {.description = "How fast it moved in the last step: as fast as it got on, not as fast as it "
                                  "was told to. Kept up to date while the simulation runs",
                   .unit = "m/s",
                   .readOnly = true,
                   .transient = true})
        .enumProperty("ground", &CharacterControllerComponent::ground,
                      {{GroundState::OnGround, std::string(nameOf(GroundState::OnGround))},
                       {GroundState::OnSteepGround, std::string(nameOf(GroundState::OnSteepGround))},
                       {GroundState::InAir, std::string(nameOf(GroundState::InAir))}},
                      {.description = "What it has under its feet: ground it can walk on, ground too steep for "
                                      "it, or nothing. Kept up to date while the simulation runs",
                       .readOnly = true,
                       .transient = true})
        .property(
            "groundNormal", &CharacterControllerComponent::groundNormal,
            {.description = "Which way the ground under it faces; up, in the air", .readOnly = true, .transient = true})
        .property("groundEntity", &CharacterControllerComponent::groundEntity,
                  {.description = "The entity it stands on; none in the air",
                   .semantic = PropertySemantic::EntityReference,
                   .readOnly = true,
                   .transient = true})
        .build();
}

/// What is wrong with a collider, as far as can be told without making a shape of it.
void findColliderProblems(const World& world, Entity entity, std::vector<PhysicsProblem>& problems) {
    const auto* collider = world.tryGet<ColliderComponent>(entity);
    if (collider == nullptr || (collider->shape != ShapeType::ConvexHull && collider->shape != ShapeType::Mesh)) {
        return;
    }
    if (collider->mesh.isNil()) {
        problems.push_back(
            {entity, fmt::format("Collider.mesh names no mesh, and a collider of the shape {} is made of one",
                                 nameOf(collider->shape))});
    }
    // A trigger is a body of its own, which the simulation does not move.
    if (collider->shape != ShapeType::Mesh || collider->trigger) {
        return;
    }
    const Entity body = bodyEntityOf(world, entity);
    const auto* rigidBody = world.tryGet<RigidBodyComponent>(body);
    if (rigidBody != nullptr && rigidBody->motion == BodyMotion::Dynamic) {
        problems.push_back(
            {entity, fmt::format("Collider.shape is mesh, and the body it is part of - '{}' - is dynamic: the "
                                 "triangles of a mesh have no inside, and only a body that the simulation does "
                                 "not move is made of them",
                                 world.name(body))});
    }
}

/// Whether the simulation moves the body that an entity is.
bool isDynamicBody(const World& world, Entity body) {
    const auto* rigidBody = world.tryGet<RigidBodyComponent>(body);
    return rigidBody != nullptr && rigidBody->motion == BodyMotion::Dynamic;
}

/// What is wrong with the limits of a joint that has them, or nothing.
std::string limitProblemOf(const JointComponent& joint) {
    constexpr float kHalfTurn = 180.0f;
    const bool leavesNothing = joint.minimum == joint.maximum && joint.frequency <= 0.0f;
    switch (joint.type) {
        case JointType::Hinge:
            if (joint.minimum > 0.0f || joint.maximum < 0.0f || joint.minimum < -kHalfTurn ||
                joint.maximum > kHalfTurn) {
                return fmt::format("Joint.minimum and Joint.maximum of a hinge are from between -180 and 0 to "
                                   "between 0 and 180 degrees, not from {} to {}",
                                   joint.minimum, joint.maximum);
            }
            break;
        case JointType::Slider:
            if (joint.minimum > 0.0f || joint.maximum < 0.0f) {
                return fmt::format("Joint.minimum and Joint.maximum of a slider are from at most 0 to at least 0 "
                                   "metres, not from {} to {}",
                                   joint.minimum, joint.maximum);
            }
            break;
        case JointType::Distance:
            if (joint.minimum < 0.0f || joint.maximum < joint.minimum) {
                return fmt::format("Joint.minimum and Joint.maximum of a distance are from 0 or more to as much or "
                                   "more metres, not from {} to {}",
                                   joint.minimum, joint.maximum);
            }
            return "";
        case JointType::Fixed:
        case JointType::Ball:
            return "";
    }
    if (leavesNothing) {
        return fmt::format("Joint: a {} that is limited to where it is, and does not give, is a fixed joint",
                           nameOf(joint.type));
    }
    return "";
}

/// What is wrong with a joint, as far as can be told without making one.
void findJointProblems(const World& world, Entity entity, std::vector<PhysicsProblem>& problems) {
    const auto* joint = world.tryGet<JointComponent>(entity);
    if (joint == nullptr) {
        return;
    }
    const bool hasAxis = joint->type == JointType::Hinge || joint->type == JointType::Slider;
    if (hasAxis && glm::length(joint->axis) <= kEpsilon) {
        problems.push_back(
            {entity, fmt::format("Joint.axis has no length, and a {} needs a direction", nameOf(joint->type))});
    }
    if (joint->limited) {
        if (std::string problem = limitProblemOf(*joint); !problem.empty()) {
            problems.push_back({entity, std::move(problem)});
        }
    }

    const Entity body = bodyEntityOf(world, entity);
    if (body.isNull()) {
        problems.push_back({entity, "Joint: the entity is part of no body that a joint could hold - neither it nor "
                                    "anything above it has a RigidBody, and it has no Collider"});
        return;
    }
    if (joint->connectedTo.isNil()) {
        if (!isDynamicBody(world, body)) {
            problems.push_back({entity, fmt::format("Joint holds nothing: '{}' is held to the world, and is not a "
                                                    "dynamic body that would move without it",
                                                    world.name(body))});
        }
        return;
    }
    const Entity connected = world.findById(joint->connectedTo);
    if (connected.isNull()) {
        problems.push_back({entity, fmt::format("Joint.connectedTo refers to the entity {}, which does not exist",
                                                joint->connectedTo.toString())});
        return;
    }
    const Entity other = bodyEntityOf(world, connected);
    if (other.isNull()) {
        problems.push_back(
            {entity, fmt::format("Joint.connectedTo refers to '{}', which is part of no body", world.name(connected))});
    } else if (other == body) {
        problems.push_back({entity, fmt::format("Joint.connectedTo refers to '{}', which is part of the same body "
                                                "- '{}' - as the joint itself: a joint holds two bodies",
                                                world.name(connected), world.name(body))});
    } else if (!isDynamicBody(world, body) && !isDynamicBody(world, other)) {
        problems.push_back({entity, fmt::format("Joint holds nothing: neither '{}' nor '{}' is a dynamic body",
                                                world.name(body), world.name(other))});
    }
}

/// What is wrong with a character, and with a collider that hangs on one.
void findCharacterProblems(const World& world, Entity entity, std::vector<PhysicsProblem>& problems) {
    const auto* collider = world.tryGet<ColliderComponent>(entity);
    const bool hasSolidCollider = collider != nullptr && !collider->trigger;
    if (world.has<CharacterControllerComponent>(entity)) {
        if (world.has<RigidBodyComponent>(entity) || hasSolidCollider) {
            problems.push_back(
                {entity,
                 fmt::format("CharacterController: the entity is a body as well - it has a {} - and "
                             "cannot be both. A character has a capsule of its own",
                             world.has<RigidBodyComponent>(entity) ? "RigidBody" : "Collider that is no trigger")});
        }
        return;
    }
    if (!hasSolidCollider) {
        return;
    }
    const Entity character = characterAbove(world, entity);
    if (!character.isNull()) {
        problems.push_back(
            {entity, fmt::format("Collider: it hangs on the character '{}' and is part of no rigid body. It would "
                                 "stand still, be carried about, and be in the way of the character wherever "
                                 "that goes: only triggers and rigid bodies hang on a character",
                                 world.name(character))});
    }
}

} // namespace

Entity characterAbove(const World& world, Entity entity) {
    for (Entity above = entity; !above.isNull(); above = world.parent(above)) {
        // What is part of a rigid body is that body's, whatever the body hangs on.
        if (world.has<RigidBodyComponent>(above)) {
            return Entity{};
        }
        if (above != entity && world.has<CharacterControllerComponent>(above)) {
            return above;
        }
    }
    return Entity{};
}

Entity bodyEntityOf(const World& world, Entity entity) {
    for (Entity above = entity; !above.isNull(); above = world.parent(above)) {
        if (world.has<RigidBodyComponent>(above)) {
            return above;
        }
    }
    // With no rigid body at or above it, a collider that is no trigger is a body of its own.
    const auto* collider = world.tryGet<ColliderComponent>(entity);
    return collider != nullptr && !collider->trigger ? entity : Entity{};
}

std::vector<PhysicsProblem> findPhysicsProblems(const World& world) {
    std::vector<PhysicsProblem> problems;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        findColliderProblems(world, entity, problems);
        findJointProblems(world, entity, problems);
        findCharacterProblems(world, entity, problems);
    });
    return problems;
}

Status registerPhysicsComponents(ComponentRegistry& registry) {
    CORDITE_TRY(registry.add<ColliderComponent>(
        describeCollider(), {.description = "The shape of the entity for collisions. Alone, it stands still; with a "
                                            "RigidBody on its entity or above it, it is part of that body."}));
    CORDITE_TRY(registry.add<RigidBodyComponent>(
        describeRigidBody(),
        {.description = "Makes the entity a body of the simulation, shaped by its Collider and by the Colliders "
                        "below it that have no RigidBody of their own."}));
    CORDITE_TRY(registry.add<JointComponent>(
        describeJoint(), {.description = "Holds the body the entity is part of to the body of another entity, or to "
                                         "the world, as the two are when the simulation first meets the joint."}));
    return registry.add<CharacterControllerComponent>(
        describeCharacterController(),
        {.description = "Makes the entity a character: an upright capsule with its soles where the entity is, "
                        "which walks as `movement` says and jumps when `jump` is given a speed. No body: it "
                        "has no RigidBody and no Collider but a trigger."});
}

} // namespace cordite
