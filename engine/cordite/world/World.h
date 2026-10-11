#pragma once

/// @file
/// The container of everything in a scene: entities, their components and their
/// parent/child hierarchy.
///
/// @code
/// World world;
/// const Entity room = world.createEntity("Room");
/// const Entity lamp = world.createEntity("Lamp");
/// CORDITE_TRY(world.setParent(lamp, room));
///
/// world.transform(lamp).position = {0.0f, 2.5f, 0.0f};
/// world.add<CameraComponent>(lamp).verticalFovDegrees = 45.0f;
///
/// world.updateWorldTransforms();                    // once per frame, after gameplay
/// const Mat4 placement = world.worldMatrix(lamp);
///
/// for (const auto [handle, camera, placement] : world.registry().view<CameraComponent, WorldTransform>().each()) {
///     ...
/// }
/// @endcode
///
/// Systems iterate components through registry(). Creating and destroying entities and
/// changing the hierarchy always goes through World, which keeps its indices consistent.
///
/// A system may also listen for components appearing and disappearing
/// (`world.registry().on_destroy<T>()`). World guarantees that while a component is
/// being destroyed its entity still has its EntityInfo, Transform and Hierarchy, is still
/// linked to its parent, and has no children left; and that every component is destroyed
/// that way before the World itself goes away.
///
/// References and pointers to components are short-lived: destroying an entity or
/// removing a component may move other components of the same type. Keep the Entity and
/// fetch the component again instead of holding on to a reference across such calls.
///
/// A World is not thread-safe, and it stays where it is: systems may keep a reference to
/// it, and must disconnect their listeners before they go away themselves.

#include "cordite/core/Assert.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"
#include "cordite/world/Entity.h"
#include "cordite/world/Entt.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {

class World {
public:
    /// Called for one entity during a traversal. `depth` is 0 for a root.
    using Visitor = std::function<void(Entity entity, uint32_t depth)>;

    World();
    ~World();
    CORDITE_NON_MOVABLE(World);

    /// Creates a root entity with a new random identifier.
    Entity createEntity(std::string_view name = "Entity");

    /// Creates a root entity with a given identifier, as when loading a scene. Fails
    /// with InvalidArgument for the nil Uuid and AlreadyExists when the id is in use.
    [[nodiscard]] Result<Entity> createEntityWithId(const Uuid& id, std::string_view name);

    /// Destroys an entity and everything below it in the hierarchy, children before
    /// parents. Handles to them become invalid. Destroying an invalid handle does nothing.
    void destroyEntity(Entity entity);

    /// Destroys every entity.
    void clear();

    [[nodiscard]] bool isValid(Entity entity) const;

    [[nodiscard]] size_t entityCount() const { return m_entitiesById.size(); }

    /// Entity-level data. The entity must be valid.
    [[nodiscard]] const Uuid& id(Entity entity) const;
    [[nodiscard]] const std::string& name(Entity entity) const;
    void setName(Entity entity, std::string_view name);
    [[nodiscard]] bool isEnabled(Entity entity) const;
    void setEnabled(Entity entity, bool enabled);

    /// True when the entity and all its ancestors are enabled. False for an invalid one.
    [[nodiscard]] bool isEnabledInHierarchy(Entity entity) const;

    /// Returns the entity with the given id, or a null Entity.
    [[nodiscard]] Entity findById(const Uuid& id) const;

    /// Returns the first entity with the given name in hierarchy order, or a null Entity.
    [[nodiscard]] Entity findByName(std::string_view name) const;

    /// Makes `parent` the parent of `child`, appending it to the parent's children; a
    /// null parent makes `child` a root. Nothing happens if it is the parent already.
    /// With `keepWorldTransform` the child's Transform is adjusted so that it stays where
    /// it is in the world, as far as position, rotation and scale can express that
    /// (shear, from a non-uniformly scaled and rotated parent, is lost). Fails with
    /// InvalidArgument when a handle is invalid or the change would make an entity its
    /// own ancestor.
    [[nodiscard]] Status setParent(Entity child, Entity parent, bool keepWorldTransform = true);

    /// Returns the parent, or a null Entity for a root or an invalid entity.
    [[nodiscard]] Entity parent(Entity entity) const;

    /// The children of an entity in sibling order; a null Entity gives the roots.
    [[nodiscard]] std::vector<Entity> children(Entity entity) const;

    /// The entities without a parent, in order.
    [[nodiscard]] std::vector<Entity> roots() const { return children(Entity{}); }

    /// The position of an entity among its siblings (the roots, for a root).
    [[nodiscard]] size_t siblingIndex(Entity entity) const;

    /// Moves an entity among its siblings. An index past the end moves it last.
    void setSiblingIndex(Entity entity, size_t index);

    /// True when `ancestor` is the parent of `entity`, or its parent's parent, and so on.
    /// An entity is not its own descendant.
    [[nodiscard]] bool isDescendantOf(Entity entity, Entity ancestor) const;

    /// Calls `visitor` for every entity, parents before children and siblings in order.
    /// The hierarchy must not be changed during the traversal.
    void traverse(const Visitor& visitor) const;

    /// Like traverse(), for `root` and everything below it. `root` is visited at depth 0.
    void traverseSubtree(Entity root, const Visitor& visitor) const;

    /// The entity's placement relative to its parent. The entity must be valid.
    [[nodiscard]] Transform& transform(Entity entity);
    [[nodiscard]] const Transform& transform(Entity entity) const;

    /// Recomputes the WorldTransform of every entity from the local Transforms. Call it
    /// once per frame after everything that moves entities has run.
    void updateWorldTransforms();

    /// The entity's local-to-world matrix as of the last updateWorldTransforms().
    [[nodiscard]] const Mat4& worldMatrix(Entity entity) const;

    /// The entity's local-to-world matrix computed now from the current Transforms. Slower
    /// than worldMatrix(); for tools that just moved something and cannot wait a frame.
    [[nodiscard]] Mat4 computeWorldMatrix(Entity entity) const;

    /// Adds a component, or returns the one the entity already has. The entity must be
    /// valid.
    template <typename T, typename... Args>
    T& add(Entity entity, Args&&... args) {
        CORDITE_ASSERT(isValid(entity), "cannot add a component to an invalid entity");
        return m_registry.get_or_emplace<T>(entity.handle, std::forward<Args>(args)...);
    }

    /// Returns the component, or nullptr when the entity is invalid or has none.
    template <typename T>
    [[nodiscard]] T* tryGet(Entity entity) {
        return isValid(entity) ? m_registry.try_get<T>(entity.handle) : nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* tryGet(Entity entity) const {
        return isValid(entity) ? m_registry.try_get<T>(entity.handle) : nullptr;
    }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const {
        return isValid(entity) && m_registry.all_of<T>(entity.handle);
    }

    /// Removes a component if the entity has one.
    template <typename T>
    void remove(Entity entity) {
        if (isValid(entity)) {
            m_registry.remove<T>(entity.handle);
        }
    }

    /// Direct access to component storage, for systems that iterate with views. Do not
    /// create or destroy entities through it, and do not remove the four components
    /// every entity has.
    [[nodiscard]] entt::registry& registry() { return m_registry; }

    [[nodiscard]] const entt::registry& registry() const { return m_registry; }

    /// A label for the scene this world was loaded from or will be saved as.
    [[nodiscard]] const std::string& sceneName() const { return m_sceneName; }

    void setSceneName(std::string_view name) { m_sceneName = name; }

private:
    Entity createEntityInternal(const Uuid& id, std::string_view name);

    /// The list that holds `entity` as a sibling: its parent's children, or the roots.
    [[nodiscard]] std::vector<entt::entity>& siblingsOf(entt::entity entity);
    [[nodiscard]] const std::vector<entt::entity>& siblingsOf(entt::entity entity) const;

    entt::registry m_registry;
    std::unordered_map<Uuid, entt::entity> m_entitiesById;
    /// Entities without a parent, in sibling order.
    std::vector<entt::entity> m_roots;
    std::string m_sceneName = "Untitled";
};

} // namespace cordite
