#pragma once

/// @file
/// Keeping the prefab instances of an editor's world up to date with their prefabs.
///
/// The world module brings one instance up to date with the entities it is given
/// (syncPrefabInstance, cordite/world/Prefab.h); this finds the instances of a world, loads
/// the prefab of each from the assets and does it for all of them - when a scene is opened
/// (EditorContext::openScene) and when the assets are read again
/// (EditorActions::updatePrefabInstances, which records it as a step of the history).
///
/// @code
/// const std::vector<Entity> changed = syncPrefabInstances(world, components, assets);
/// for (const Entity root : linkedPrefabRoots(world, prefabId)) { ... }
/// @endcode

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <optional>
#include <vector>

namespace cordite {

/// The roots of the linked prefab instances of `world` - entities with a PrefabInstance and
/// a PrefabLink - in the order of the hierarchy, an instance inside another after it; those
/// of `prefab` alone, when one is given.
[[nodiscard]] std::vector<Entity> linkedPrefabRoots(const World& world, std::optional<Uuid> prefab = std::nullopt);

/// Brings every linked instance of `world` - of `prefab` alone, when one is given - up to
/// date with its prefab (syncPrefabInstance), the outer instances before the ones inside
/// them. Returns the roots of those that changed. A prefab that cannot be loaded, or whose
/// entities are wrong, is said in the log, once for each instance, and its instances are
/// left as they are: a scene is read without its prefabs.
[[nodiscard]] std::vector<Entity> syncPrefabInstances(World& world, const ComponentRegistry& components,
                                                      AssetManager& assets, std::optional<Uuid> prefab = std::nullopt);

} // namespace cordite
