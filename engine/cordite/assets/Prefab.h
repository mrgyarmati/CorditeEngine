#pragma once

/// @file
/// A prefab as an asset: a few entities that belong together, written down once and placed
/// as often as they are wanted.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> barrel, assets.loadPrefab(barrelId));
/// // Placing it is the world's business (cordite/world/Prefab.h):
/// CORDITE_TRY_ASSIGN(const Entity placed, instantiatePrefab(world, components, barrel->entities, barrelId));
///
/// // Writing one:
/// CORDITE_TRY(writeJsonFile(path, prefabDocument({.name = "Barrel", .entities = entities})));
/// @endcode
///
/// A prefab file is a small scene, in a file whose name ends in `.prefab.json`:
///
/// @code{.json}
/// {
///     "format": "cordite.prefab",
///     "version": 1,
///     "name": "Barrel",
///     "entities": [
///         {"id": "...", "name": "Barrel", "enabled": true, "parent": null, "components": {...}},
///         {"id": "...", "name": "Lid", "enabled": true, "parent": "...", "components": {...}}
///     ]
/// }
/// @endcode
///
/// Its entities are written as those of a scene are, and one of them - the root of the
/// prefab, which comes first - hangs under none of the others. The assets know a prefab
/// as what its file says and no more: which components there are, and whether these
/// entities are right for them, is said when the prefab is placed in a world.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cordite {

inline constexpr uint32_t kPrefabFormatVersion = 1;

/// The most a prefab file may hold, in bytes, and the most entities a prefab may have.
inline constexpr size_t kMaxPrefabFileBytes = size_t{16} * 1024 * 1024;
inline constexpr size_t kMaxPrefabEntities = 10000;

struct PrefabData {
    /// What the prefab is called. Empty when its document gives it no name.
    std::string name;
    /// Its entities, as serializeEntities() writes them: an array of objects, one at
    /// least and kMaxPrefabEntities at most.
    Json entities;
};

/// Reads what a prefab file holds. Fails with ParseError for a document that is no
/// prefab - not an object, another format, a member a prefab does not have, no entities
/// or more than kMaxPrefabEntities, an entity that is no object - and with
/// VersionMismatch for one written by a newer engine. What the entities are made of is
/// not looked at here.
[[nodiscard]] Result<PrefabData> parsePrefab(const Json& document);

/// The document of a prefab, as its file holds it.
[[nodiscard]] Json prefabDocument(const PrefabData& prefab);

} // namespace cordite
