#pragma once

/// @file
/// The handle that identifies an entity inside a World.

#include "cordite/world/Entt.h"

#include <cstddef>
#include <functional>

namespace cordite {

/// Refers to one entity of one World. It is a plain value: copy it freely, compare it,
/// use it as a key in a hash map. It carries no pointer to its world, so every operation
/// goes through the World it came from.
///
/// @code
/// const Entity lamp = world.createEntity("Lamp");
/// world.destroyEntity(lamp);
/// world.isValid(lamp);            // false: the handle is stale
/// Entity{}.isNull();              // true: a default-constructed Entity refers to nothing
/// @endcode
///
/// A handle stops being valid when its entity is destroyed. To refer to an entity across
/// saves, reloads or play sessions, store its Uuid (World::id) instead: handles are not
/// stable, Uuids are.
struct Entity {
    entt::entity handle = entt::null;

    /// True for the "no entity" value, which is what a default-constructed Entity is.
    [[nodiscard]] constexpr bool isNull() const noexcept { return handle == entt::null; }

    constexpr bool operator==(const Entity&) const noexcept = default;
};

} // namespace cordite

template <>
struct std::hash<cordite::Entity> {
    size_t operator()(const cordite::Entity& entity) const noexcept {
        return std::hash<entt::id_type>{}(entt::to_integral(entity.handle));
    }
};
