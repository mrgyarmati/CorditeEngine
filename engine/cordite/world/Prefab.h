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
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// What of an instance is applied to its prefab: one override of one entity, every override
/// of one entity, or - from the root, with no entry - the whole instance.
struct PrefabApply {
    /// A linked entity of the instance.
    Entity entity;
    /// One override of it, as its PrefabLink lists it; empty for all of them - and, when
    /// `entity` is the root of the instance, for every override of every entity of the
    /// instance and every entity added to it, with what hangs under it.
    std::string entry;
};

/// What the prefab whose entities are `entities` - as its file holds them - becomes when
/// what `apply` names of the instance whose root is `root` is written into it: the
/// instance's values of the properties it overrides, the components it added, without
/// those it left out, its names and whether its entities are enabled - and, for the whole
/// instance, the entities added to it, under the entities of the prefab they hang under,
/// with their own ids. A reference to an entity of the instance is led to the entity of
/// the prefab it is a copy of. The root's Transform and name stay the prefab's. Nothing of
/// the world changes; markPrefabApplied() does that once the prefab is written.
///
/// Fails with InvalidArgument when `root` is no linked instance of the prefab - its root
/// is linked to another entity than the prefab's root - when `apply.entity` is no linked
/// entity of it, when `apply.entry` is none of its overrides, and when what is to be
/// applied holds an instance of another prefab, which cannot be applied yet; as
/// syncPrefabInstance() does for entities that are wrong. How many entities a prefab may
/// have is the assets' to say (kMaxPrefabEntities), and whoever writes the file checks it.
[[nodiscard]] Result<Json> prefabEntitiesWithOverrides(const World& world, const ComponentRegistry& components,
                                                       Entity root, const Json& entities, const PrefabApply& apply);

/// Makes what prefabEntitiesWithOverrides() wrote into the prefab the prefab's in the
/// instance: the overrides that were applied are no longer the entities' own, and the
/// entities added to the instance are linked to themselves in the prefab, which has their
/// ids. The arguments are those prefabEntitiesWithOverrides() succeeded with.
void markPrefabApplied(World& world, Entity root, const PrefabApply& apply);

/// Whether `entity` is the root of a linked instance of its own: it has a PrefabInstance and a
/// PrefabLink to the same prefab. An instance placed inside another is one, and no part of the
/// one around it - even when both are of the same prefab.
[[nodiscard]] bool isPrefabInstanceRoot(const World& world, Entity entity);

/// The roots of the linked prefab instances of `world` - entities with a PrefabInstance and a
/// PrefabLink to the same prefab - in the order of the hierarchy; those of `prefab` alone,
/// when one is given. A prefab placed inside an instance of another, as part of that one's
/// prefab, is linked to that prefab, and is no root of its own.
[[nodiscard]] std::vector<Entity> linkedPrefabRoots(const World& world, std::optional<Uuid> prefab = std::nullopt);

/// The root of the instance a linked entity is part of: the nearest entity at it or above
/// it that is the root of an instance of the prefab its link names. Null for an entity that
/// is no part of one.
[[nodiscard]] Entity prefabInstanceRootOf(const World& world, Entity entity);

/// Reads the entities of a prefab, as its file holds them, into `entities`, and returns what
/// to call it in a message - its path among the assets. Fails as reading it does.
using PrefabReader = std::function<Result<std::string>(const Uuid& prefab, Json& entities)>;

/// How deep prefabs may be placed inside each other.
inline constexpr size_t kMaxPrefabNesting = 32;

/// The entities of the prefab `prefab`, called `name` - `entities`, as its file holds them -
/// with every prefab placed inside it brought up to date with what that one is now
/// (syncPrefabInstance), but for what the file overrides of it: what placing the prefab, or
/// bringing an instance of it up to date, works from. Those inside are read with `read`, and
/// what is inside them in turn first. A file whose prefabs hold nothing of another is
/// returned as it is. What is made of a prefab inside gets an id that follows from where it
/// is, the same every time.
///
/// Fails with InvalidArgument for a prefab that holds itself, directly or through others -
/// the message names the chain - and for prefabs placed inside each other more than
/// kMaxPrefabNesting deep; as `read` fails; and as syncPrefabInstance() does for entities
/// that are wrong.
[[nodiscard]] Result<Json> resolvePrefabEntities(const ComponentRegistry& components, const Uuid& prefab,
                                                 std::string_view name, const Json& entities, const PrefabReader& read);

/// What a prefab made of `root` and everything below it holds as its entities: the root
/// first, hanging under nothing, and without the PrefabInstance it has when it was
/// placed from a prefab itself - a prefab is not an instance of another. `root` must be
/// valid.
[[nodiscard]] Json serializePrefabEntities(const World& world, const ComponentRegistry& components, Entity root);

} // namespace cordite
