#pragma once

/// @file
/// Placing a prefab in a world, and making one of what is there.
///
/// @code
/// // The entities of a prefab asset (PrefabData::entities, cordite/assets/Prefab.h):
/// CORDITE_TRY_ASSIGN(const Entity barrel,
///                    instantiatePrefab(world, components, prefab->entities, prefabId,
///                                      {.parent = yard, .position = Vec3(2.0f, 0.0f, -1.0f)}));
///
/// // And the other way: what is written into a prefab file for an entity and all below it.
/// const Json entities = serializePrefabEntities(world, components, barrel);
/// @endcode
///
/// A prefab is a group of entities with one root. Placing it makes copies of them with
/// ids of their own - what refers to an entity of the group refers to its copy - marks
/// the root with a PrefabInstance component that names the prefab, and links every copy
/// to the entity of the prefab it was made of (PrefabLink). The copies are saved with
/// their scene in full, and a scene is read without its prefabs; when the prefab
/// changes, syncPrefabInstance() gives them what it is now, but for what they override
/// (docs/architecture.md, "A prefab stays linked to what was placed from it").

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstddef>
#include <optional>
#include <string>

namespace cordite {

struct PrefabInstanceOptions {
    /// The instance becomes the last child of this entity; null makes it a root.
    Entity parent;
    /// The name of the root of the instance. Empty keeps the name the prefab gives it.
    std::string name;
    /// Where the root of the instance is put and how it is turned, relative to its
    /// parent. Without, it is where and as the prefab has it. A rotation need not be of
    /// length one; it is made so.
    std::optional<Vec3> position;
    std::optional<Quat> rotation;
};

/// Creates the entities of a prefab and returns the root of the instance: the one entity
/// of the group that hangs under none of the others. Every entity gets an id of its own,
/// and the root a PrefabInstance that names `prefab` - instead of the one it has, when
/// the prefab was written with one.
///
/// Fails with InvalidArgument for a parent that is no entity, a position that is not
/// finite and a rotation that is not finite or zero; with ParseError for a group that
/// has no root or more than one; and as instantiateEntities() does for entities that are
/// wrong - an unknown component, a property out of range. Nothing is created then.
[[nodiscard]] Result<Entity> instantiatePrefab(World& world, const ComponentRegistry& components, const Json& entities,
                                               const Uuid& prefab, const PrefabInstanceOptions& options = {});

/// What syncPrefabInstance() changed in an instance.
struct PrefabSync {
    size_t entitiesCreated = 0;
    size_t entitiesDestroyed = 0;
    size_t componentsAdded = 0;
    size_t componentsRemoved = 0;
    /// Properties - and names and the enabled state of entities - given the prefab's value.
    size_t valuesChanged = 0;

    [[nodiscard]] bool changed() const {
        return entitiesCreated + entitiesDestroyed + componentsAdded + componentsRemoved + valuesChanged > 0;
    }
};

/// Gives the instance whose root is `root` what its prefab - `entities`, as the prefab's
/// file holds them - is now, but for what it overrides (PrefabLinkComponent::overrides).
/// Every entity of the instance that the prefab has takes the prefab's values of its
/// components, its name and whether it is enabled; a component the prefab's entity has
/// and it lacks is added, one it has that the prefab's lacks is taken away; an entity the
/// prefab has and the instance lacks is made, under the copy of its parent, and a linked
/// entity whose source the prefab no longer has is destroyed, with what hangs under it.
/// The root keeps its own Transform, name and PrefabInstance; entities without a
/// PrefabLink - added to the instance by hand - are left as they are, and so is what
/// hangs under another instance inside this one.
///
/// Fails, changing nothing, with InvalidArgument for a root that is no entity or has no
/// PrefabInstance, with ParseError for entities that are not a group with one root, and as
/// TypeInfo::parse() fails for a component that is wrong.
[[nodiscard]] Result<PrefabSync> syncPrefabInstance(World& world, const ComponentRegistry& components, Entity root,
                                                    const Json& entities);

/// What a prefab made of `root` and everything below it holds as its entities: the root
/// first, hanging under nothing, and without the PrefabInstance it has when it was
/// placed from a prefab itself - a prefab is not an instance of another. `root` must be
/// valid.
[[nodiscard]] Json serializePrefabEntities(const World& world, const ComponentRegistry& components, Entity root);

} // namespace cordite
