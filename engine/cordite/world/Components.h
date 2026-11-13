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
#include <string_view>
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

/// Says that an entity, with what hangs under it, was placed from a prefab, and which
/// one (cordite/world/Prefab.h). Nothing follows from it for the entity - it is an
/// ordinary one, and a prefab that is changed later does not change it - but a scene
/// says with it which prefabs it was made of, and an editor where an entity came from.
struct PrefabInstanceComponent {
    /// What the component is called in files and in the component registry.
    static constexpr std::string_view kName = "PrefabInstance";

    /// The prefab asset the entity was placed from. Nil for none.
    Uuid prefab;
};

/// Says which entity of a prefab an entity placed from it is a copy of, and what of it has
/// values of its own (cordite/world/Prefab.h): every entity of an instance has one, under
/// the root that a PrefabInstance names the prefab of. Entities placed before prefabs were
/// linked, and those added to an instance by hand, have none, and are left as they are.
struct PrefabLinkComponent {
    static constexpr std::string_view kName = "PrefabLink";

    /// The prefab whose file `source` is an id in: that of the instance the entity is part
    /// of. Every entity of an instance is linked to its root's prefab, the entities of a
    /// prefab placed inside it included; what is linked to another prefab is no part of it.
    Uuid prefab;
    /// The id the entity has in the prefab's file.
    Uuid source;
    /// What of the entity keeps values of its own when the prefab changes, in the order it
    /// got them: "Light.intensity" for a property; "+Light" for a component the prefab's
    /// entity does not have, "-Light" for one it has that this entity does not; "name" and
    /// "enabled". The Transform of the instance's root is always its own, and not listed.
    std::vector<std::string> overrides;
};

/// Registers the components declared in this file. Other modules that add components
/// provide a function like this one, and the engine calls them all at start-up. Fails
/// with AlreadyExists when called twice on the same registry.
[[nodiscard]] Status registerWorldComponents(ComponentRegistry& registry);

} // namespace cordite
