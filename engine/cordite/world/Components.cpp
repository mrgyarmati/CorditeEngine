#include "cordite/world/Components.h"

#include "cordite/math/MathReflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/ComponentRegistry.h"

namespace cordite {
namespace {

TypeInfo describeTransform() {
    return TypeBuilder<Transform>("Transform")
        .property("position", &Transform::position, {.description = "Offset from the parent", .unit = "m"})
        .property("rotation", &Transform::rotation,
                  {.description = "Orientation relative to the parent, as a quaternion [x, y, z, w]"})
        .property("scale", &Transform::scale, {.description = "Scale relative to the parent"})
        .build();
}

TypeInfo describeCamera() {
    return TypeBuilder<CameraComponent>("Camera")
        .enumProperty(
            "projection", &CameraComponent::projection,
            {{CameraProjection::Perspective, "perspective"}, {CameraProjection::Orthographic, "orthographic"}},
            {.description = "How the view volume is shaped"})
        .property("verticalFovDegrees", &CameraComponent::verticalFovDegrees,
                  {.description = "Vertical field of view of a perspective camera",
                   .minimum = 1.0,
                   .maximum = 179.0,
                   .unit = "deg"})
        .property("orthographicHeight", &CameraComponent::orthographicHeight,
                  {.description = "Height of the volume an orthographic camera sees", .minimum = 0.001, .unit = "m"})
        .property("nearPlane", &CameraComponent::nearPlane,
                  {.description = "Distance to the near clipping plane", .minimum = 0.001, .unit = "m"})
        .property("farPlane", &CameraComponent::farPlane,
                  {.description = "Distance to the far clipping plane", .minimum = 0.01, .unit = "m"})
        .property("primary", &CameraComponent::primary,
                  {.description = "The game renders from the first enabled primary camera"})
        .build();
}

} // namespace

Status registerWorldComponents(ComponentRegistry& registry) {
    CORDITE_TRY(registry.add<Transform>(
        describeTransform(),
        {.description = "Where the entity is, relative to its parent. Every entity has one.", .optional = false}));
    CORDITE_TRY(registry.add<CameraComponent>(
        describeCamera(), {.description = "A point of view the scene can be rendered from, looking along -Z."}));
    return {};
}

} // namespace cordite
