#pragma once

/// @file
/// Which assets a scene uses, and whether it will work as it stands.
///
/// Components refer to assets by id. These functions find every such reference by
/// walking the reflected properties of every component, so they cover component types
/// that are added later without being told about them.
///
/// @code
/// for (const AssetReferenceUse& use : collectAssetReferences(world, components)) {
///     preload(use.asset);
/// }
///
/// for (const SceneProblem& problem : findSceneProblems(world, components, assets.database())) {
///     CORDITE_LOG_WARN("Scene", "{}: {}", world.name(problem.entity), problem.message);
/// }
/// @endcode

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <string>
#include <vector>

namespace cordite {

/// One place where a component names an asset.
struct AssetReferenceUse {
    Entity entity;
    /// The name of the component type, such as "MeshRenderer".
    std::string component;
    /// The property, with an index for an entry of a list: "mesh", "materials[1]".
    std::string property;
    /// Never nil: empty slots are not uses.
    Uuid asset;
    /// What the property expects, as PropertyAttributes::referenceType: "Mesh". May be
    /// empty for a property that takes any asset.
    std::string expectedType;
};

/// Every reference to an asset in the components of a world, in hierarchy order.
[[nodiscard]] std::vector<AssetReferenceUse> collectAssetReferences(const World& world,
                                                                    const ComponentRegistry& components);

/// Something in a scene that will not work as it stands.
struct SceneProblem {
    Entity entity;
    /// One sentence, naming the component and the property.
    std::string message;
};

/// Checks every asset reference in a world: the asset must exist and be of the kind the
/// property expects. Then its colliders, for what the physics will not make a shape or
/// a body of (findColliderProblems()). A scene without problems draws and collides as
/// it was meant to.
[[nodiscard]] std::vector<SceneProblem> findSceneProblems(const World& world, const ComponentRegistry& components,
                                                          const AssetDatabase& assets);

} // namespace cordite
