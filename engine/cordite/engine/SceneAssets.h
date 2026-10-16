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
///
/// // What a scene file uses, read from its document without loading it:
/// CORDITE_TRY_ASSIGN(const std::vector<Uuid> used,
///                    assetsReferencedBy(AssetType::Scene, document, components));
/// @endcode

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
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

/// The assets that the entities of a scene or a prefab file refer to, given the document
/// the file holds: each once, in the order of their first use. They are read from the
/// document as it stands, without making entities of it, so that a file that would not
/// load still says what it uses: a component, a property or a value that is not known
/// is passed over, for loading to report. The PrefabInstance of the root of a prefab,
/// which placing the prefab replaces, is no use. Fails with ParseError or
/// VersionMismatch for a document that is no scene, or no prefab, of a version this
/// engine reads, and with InvalidArgument for any other type: only these two hold
/// entities. An engine hands it to its assets (AssetDatabaseDesc::readReferences).
[[nodiscard]] Result<std::vector<Uuid>> assetsReferencedBy(AssetType type, const Json& document,
                                                           const ComponentRegistry& components);

/// assetsReferencedBy() with `components`, as AssetDatabaseDesc::readReferences takes it:
/// what an engine reads its assets with. The registry must outlive every database that
/// is given it.
[[nodiscard]] AssetReferenceReader assetReferenceReader(const ComponentRegistry& components);

/// Something in a scene that will not work as it stands.
struct SceneProblem {
    Entity entity;
    /// One sentence, naming the component and the property.
    std::string message;
};

/// Checks every asset reference in a world: the asset must exist and be of the kind the
/// property expects. Then its colliders and joints, for what the physics will make no
/// shape, no body and no joint of (findPhysicsProblems()). A scene without problems
/// draws and collides as it was meant to.
[[nodiscard]] std::vector<SceneProblem> findSceneProblems(const World& world, const ComponentRegistry& components,
                                                          const AssetDatabase& assets);

} // namespace cordite
