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
/// ids of their own - what refers to an entity of the group refers to its copy - and
/// marks the root with a PrefabInstance component that names the prefab. The copies are
/// ordinary entities from then on: they are saved with their scene and changed like any
/// others, and a prefab that is changed later does not change them.

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

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

/// What a prefab made of `root` and everything below it holds as its entities: the root
/// first, hanging under nothing, and without the PrefabInstance it has when it was
/// placed from a prefab itself - a prefab is not an instance of another. `root` must be
/// valid.
[[nodiscard]] Json serializePrefabEntities(const World& world, const ComponentRegistry& components, Entity root);

} // namespace cordite
