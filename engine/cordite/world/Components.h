#pragma once

/// @file
/// The components that describe a scene.
///
/// Components are plain data. They carry no behavior and know nothing about the systems
/// that consume them: the renderer, physics and audio each read the components they care
/// about and never see each other. Every component here is described to the reflection
/// system in Components.cpp, which is what makes it saveable, editable and scriptable.
///
/// Every entity always has an EntityInfo, a Transform (cordite/math/Transform.h: its
/// placement relative to its parent), a Hierarchy and a WorldTransform. The remaining
/// components are optional.
///
/// @code
/// const Entity eye = world.createEntity("Eye");
/// world.transform(eye).position = {0.0f, 1.7f, 4.0f};
///
/// CameraComponent& camera = world.add<CameraComponent>(eye);
/// camera.verticalFovDegrees = 70.0f;
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cordite {

class ComponentRegistry;

/// Identity and state that every entity has. Change it through World::setName and
/// World::setEnabled; the id never changes.
struct EntityInfo {
    /// Stable across saves and reloads.
    Uuid id;
    /// For people. Need not be unique.
    std::string name;
    /// A disabled entity, and every entity below it, is ignored by all systems.
    bool enabled = true;
};

/// Links to the parent and children. Maintained by World: read it freely, but change it
/// only through World::setParent and World::setSiblingIndex.
struct Hierarchy {
    entt::entity parent = entt::null;
    /// In sibling order.
    std::vector<entt::entity> children;
};

/// The entity's placement in world space, derived from the Transforms up the hierarchy
/// by World::updateWorldTransforms. Never saved.
struct WorldTransform {
    Mat4 matrix{1.0f};
};

enum class CameraProjection : uint8_t {
    Perspective,
    Orthographic,
};

/// A point of view. The camera looks along the entity's -Z axis with +Y up.
struct CameraComponent {
    CameraProjection projection = CameraProjection::Perspective;
    /// Perspective only.
    float verticalFovDegrees = 60.0f;
    /// Orthographic only: the height of the visible volume in metres.
    float orthographicHeight = 10.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    /// The game renders from the first enabled camera marked primary.
    bool primary = true;
};

/// Registers the components declared in this file. Other modules that add components
/// provide a function like this one, and the engine calls them all at start-up. Fails
/// with AlreadyExists when called twice on the same registry.
[[nodiscard]] Status registerWorldComponents(ComponentRegistry& registry);

} // namespace cordite
