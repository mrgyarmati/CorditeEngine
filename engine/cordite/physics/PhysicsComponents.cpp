#include "cordite/physics/PhysicsComponents.h"

#include "cordite/core/Reflection.h"
#include "cordite/math/MathReflection.h"

#include <fmt/format.h>

#include <cstdint>
#include <string>

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

/// The rigid body a collider is part of: the nearest at its entity or above it, if any.
Entity bodyEntityOf(const World& world, Entity collider) {
    for (Entity entity = collider; !entity.isNull(); entity = world.parent(entity)) {
        if (world.has<RigidBodyComponent>(entity)) {
            return entity;
        }
    }
    return Entity{};
}

} // namespace

std::vector<ColliderProblem> findColliderProblems(const World& world) {
    std::vector<ColliderProblem> problems;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        const auto* collider = world.tryGet<ColliderComponent>(entity);
        if (collider == nullptr || (collider->shape != ShapeType::ConvexHull && collider->shape != ShapeType::Mesh)) {
            return;
        }
        if (collider->mesh.isNil()) {
            problems.push_back(
                {entity, fmt::format("Collider.mesh names no mesh, and a collider of the shape {} is made of one",
                                     nameOf(collider->shape))});
        }
        const Entity body = collider->shape == ShapeType::Mesh ? bodyEntityOf(world, entity) : Entity{};
        if (!body.isNull() && world.tryGet<RigidBodyComponent>(body)->motion == BodyMotion::Dynamic) {
            problems.push_back(
                {entity, fmt::format("Collider.shape is mesh, and the body it is part of - '{}' - is dynamic: the "
                                     "triangles of a mesh have no inside, and only a body that the simulation does "
                                     "not move is made of them",
                                     world.name(body))});
        }
    });
    return problems;
}

Status registerPhysicsComponents(ComponentRegistry& registry) {
    CORDITE_TRY(registry.add<ColliderComponent>(
        describeCollider(), {.description = "The shape of the entity for collisions. Alone, it stands still; with a "
                                            "RigidBody on its entity or above it, it is part of that body."}));
    return registry.add<RigidBodyComponent>(
        describeRigidBody(),
        {.description = "Makes the entity a body of the simulation, shaped by its Collider and by the Colliders "
                        "below it that have no RigidBody of their own."});
}

} // namespace cordite
