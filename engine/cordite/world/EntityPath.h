#pragma once

/// @file
/// Paths of entities: the names of an entity and of what it hangs under, from a root of
/// its world down, joined by '/' - how the editor's commands and the tracks of a sequence
/// name the entities they mean.
///
/// @code
/// const std::string path = entityPath(world, lamp);                            // "Room/Table/Lamp"
/// CORDITE_TRY_ASSIGN(const Entity same, findEntityByPath(world, path));
/// // Below an entity, and from the roots whatever the entity:
/// CORDITE_TRY_ASSIGN(const Entity camera, findEntityByPath(world, "Rig/Camera", cinematic));
/// CORDITE_TRY_ASSIGN(const Entity room, findEntityByPath(world, "/Room", cinematic));
/// @endcode
///
/// A path finds an entity by names, and names need not be unique: two siblings of the same
/// name are not told apart by one, nor is a name with a '/' in it found. What has to be
/// found whatever it is called is found by its id.

#include "cordite/core/Result.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <span>
#include <string>
#include <string_view>

namespace cordite {

/// The names leading to an entity, from a root of its world down: "Room/Table/Lamp". The
/// entity must be valid.
[[nodiscard]] std::string entityPath(const World& world, Entity entity);

/// Entities as a message lists them: the path of each, and its id - of the first four, and
/// how many more there are.
[[nodiscard]] std::string describeEntities(const World& world, std::span<const Entity> entities);

/// The entity a path of names leads to. Each part of the path names a child of the entity
/// the part before led to: the first part a child of `from`, or a root where `from` is
/// null or the path begins with '/'. Empty parts - of a doubled slash, or a slash at the
/// end - add nothing, so a path without names leads to `from`. Fails with NotFound for a
/// part that names nothing there, and with InvalidArgument for a part that names several
/// (listing them), for a path that leads to no entity at all, and for a `from` that is no
/// longer there.
[[nodiscard]] Result<Entity> findEntityByPath(const World& world, std::string_view path, Entity from = {});

} // namespace cordite
