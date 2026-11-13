#pragma once

/// @file
/// Keeping the prefab instances of an editor's world up to date with their prefabs.
///
/// The world module brings one instance up to date with the entities it is given
/// (syncPrefabInstance, cordite/world/Prefab.h) and brings the prefabs inside a prefab up to
/// date with theirs (resolvePrefabEntities); this reads prefabs from the assets for both, and
/// does it for every instance of a world - when a scene is opened (EditorContext::openScene)
/// and when the assets are read again (EditorActions::updatePrefabInstances, which records
/// it as a step of the history).
///
/// @code
/// CORDITE_TRY_ASSIGN(const Json entities, currentPrefabEntities(assets, components, prefabId));
/// const std::vector<Entity> changed = syncPrefabInstances(world, components, assets);
/// @endcode

#include "cordite/assets/AssetManager.h"
#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/Prefab.h"
#include "cordite/world/World.h"

#include <optional>
#include <vector>

namespace cordite {

/// Reads prefabs from `assets`, for resolvePrefabEntities(): their entities, and their paths
/// for messages.
[[nodiscard]] PrefabReader prefabReaderOf(AssetManager& assets);

/// What placing the prefab `prefab`, or bringing an instance of it up to date, works from:
/// its entities as its file holds them, with the prefabs inside it brought up to date
/// (resolvePrefabEntities). Fails as loading it does, and as resolvePrefabEntities() does.
[[nodiscard]] Result<Json> currentPrefabEntities(AssetManager& assets, const ComponentRegistry& components,
                                                 const Uuid& prefab);

/// Brings every linked instance of `world` - of `prefab` alone, when one is given - up to
/// date with its prefab (currentPrefabEntities, syncPrefabInstance), in the order of the
/// hierarchy. Returns the roots of those that changed. A prefab that cannot be loaded, or
/// whose entities are wrong, is said in the log, once for each instance, and its instances
/// are left as they are: a scene is read without its prefabs.
[[nodiscard]] std::vector<Entity> syncPrefabInstances(World& world, const ComponentRegistry& components,
                                                      AssetManager& assets, std::optional<Uuid> prefab = std::nullopt);

} // namespace cordite
