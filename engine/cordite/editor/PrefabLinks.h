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
/// if (prefabViewOf(world, entity).keepsProperty("Light", "intensity")) { /* mark it */ }
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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// What an entity is to the prefabs of its world: what the hierarchy and the inspector show
/// of it, worked out without an interface (prefabViewOf).
struct PrefabView {
    enum class Role : uint8_t {
        /// No part of an instance of a prefab.
        None,
        /// The root of a linked instance.
        Root,
        /// An entity of an instance, linked to its prefab - the root of a prefab inside the
        /// instance's prefab among them (innerPrefab).
        Part,
        /// An entity added to an instance: its prefab does not have it, until the instance
        /// is applied from its root.
        Added,
    };

    Role role = Role::None;
    /// The root of the instance the entity is, or is part of, or was added to; null for None.
    Entity root;
    /// The prefab of that instance.
    Uuid prefab;
    /// For a Part that is the root of a prefab inside the instance's prefab: that prefab.
    Uuid innerPrefab;
    /// What the entity keeps of its own (PrefabLinkComponent::overrides), for a Root and a
    /// Part.
    std::vector<std::string> overrides;

    /// Whether the entity keeps `entry` of its own.
    [[nodiscard]] bool keeps(std::string_view entry) const;
    /// Whether a property of a component of the entity is its own: the property is
    /// overridden, or the whole component was added to it.
    [[nodiscard]] bool keepsProperty(std::string_view component, std::string_view property) const;
    /// Whether the component was added to the entity, and is none of its prefab's.
    [[nodiscard]] bool addedComponent(std::string_view component) const;
    /// The components the prefab's entity has that the entity leaves out ("-Light").
    [[nodiscard]] std::vector<std::string> leftOut() const;
};

/// What the hierarchy writes after the name of an entity of an instance: "+" for one added
/// to it, "*" for one that keeps something of its own, nothing otherwise.
[[nodiscard]] std::string_view prefabMarker(const PrefabView& view);

/// What `entity` is to the prefabs of `world`. An entity that is no entity is None.
[[nodiscard]] PrefabView prefabViewOf(const World& world, Entity entity);

/// The command line that takes back what `entity` keeps of its own - `entry` alone, or all of
/// it when that is empty: `prefab.revert` - or writes it into its prefab, `prefab.apply`. A
/// panel hands it on to be run between frames: applying writes a file and reads the assets
/// again, which nothing may do in the middle of drawing them.
[[nodiscard]] std::string prefabCommandLine(std::string_view command, const World& world, Entity entity,
                                            std::string_view entry = {});

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
