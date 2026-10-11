#include "cordite/world/World.h"

#include <algorithm>
#include <ranges>
#include <span>

namespace cordite {
namespace {

/// Calls `visitor(entity, depth)` for the given entities and everything below them:
/// parents before children, siblings in order. It keeps its own stack, so the depth of a
/// hierarchy is not limited by the call stack. The visitor must not change the hierarchy.
template <typename Visitor>
void visitDepthFirst(const entt::registry& registry, std::span<const entt::entity> starts, const Visitor& visitor) {
    // Each batch of siblings is pushed in reverse so that the first one is popped first.
    std::vector<std::pair<entt::entity, uint32_t>> pending;
    pending.reserve(starts.size());
    for (const entt::entity start : starts | std::views::reverse) {
        pending.emplace_back(start, 0u);
    }

    while (!pending.empty()) {
        const auto [entity, depth] = pending.back();
        pending.pop_back();
        visitor(entity, depth);

        const std::vector<entt::entity>& children = registry.get<Hierarchy>(entity).children;
        for (const entt::entity child : children | std::views::reverse) {
            pending.emplace_back(child, depth + 1);
        }
    }
}

} // namespace

World::World() {
    // EnTT destroys an entity's components in the reverse order in which their storages
    // were created. Creating these four first makes them the last to go, which is what
    // lets code that reacts to the destruction of another component still read them.
    m_registry.storage<EntityInfo>();
    m_registry.storage<Transform>();
    m_registry.storage<Hierarchy>();
    m_registry.storage<WorldTransform>();
}

World::~World() {
    // Destroying the registry alone would not notify anyone.
    clear();
}

Entity World::createEntity(std::string_view name) {
    return createEntityInternal(Uuid::generate(), name);
}

Result<Entity> World::createEntityWithId(const Uuid& id, std::string_view name) {
    if (id.isNil()) {
        return makeError(ErrorCode::InvalidArgument, "an entity cannot have the nil identifier");
    }
    if (m_entitiesById.contains(id)) {
        return makeError(ErrorCode::AlreadyExists, "an entity with id {} already exists", id.toString());
    }
    return createEntityInternal(id, name);
}

Entity World::createEntityInternal(const Uuid& id, std::string_view name) {
    const entt::entity handle = m_registry.create();
    m_registry.emplace<EntityInfo>(handle, id, std::string(name), true);
    m_registry.emplace<Transform>(handle);
    m_registry.emplace<Hierarchy>(handle);
    m_registry.emplace<WorldTransform>(handle);

    m_entitiesById.emplace(id, handle);
    m_roots.push_back(handle);
    return Entity{handle};
}

std::vector<entt::entity>& World::siblingsOf(entt::entity entity) {
    const entt::entity parentHandle = m_registry.get<Hierarchy>(entity).parent;
    return parentHandle == entt::null ? m_roots : m_registry.get<Hierarchy>(parentHandle).children;
}

const std::vector<entt::entity>& World::siblingsOf(entt::entity entity) const {
    const entt::entity parentHandle = m_registry.get<Hierarchy>(entity).parent;
    return parentHandle == entt::null ? m_roots : m_registry.get<Hierarchy>(parentHandle).children;
}

void World::destroyEntity(Entity entity) {
    if (!isValid(entity)) {
        return;
    }

    std::vector<entt::entity> subtree;
    visitDepthFirst(m_registry, std::span(&entity.handle, 1),
                    [&subtree](entt::entity member, uint32_t /*depth*/) { subtree.push_back(member); });

    // Backwards through a parents-first list is children first. Each entity is unlinked
    // only after its components are gone, so whoever reacts to their destruction still
    // finds the entity attached to its parent.
    for (const entt::entity member : subtree | std::views::reverse) {
        const entt::entity parentHandle = m_registry.get<Hierarchy>(member).parent;
        m_entitiesById.erase(m_registry.get<EntityInfo>(member).id);
        m_registry.destroy(member);

        // Looked up only now: destroying an entity moves other components around, so a
        // reference taken earlier could be stale. Going backwards, the entity is the last
        // remaining child of its parent; only the root of the subtree can sit anywhere.
        std::vector<entt::entity>& siblings =
            parentHandle == entt::null ? m_roots : m_registry.get<Hierarchy>(parentHandle).children;
        if (!siblings.empty() && siblings.back() == member) {
            siblings.pop_back();
        } else {
            std::erase(siblings, member);
        }
    }
}

void World::clear() {
    m_registry.clear();
    m_entitiesById.clear();
    m_roots.clear();
}

bool World::isValid(Entity entity) const {
    return !entity.isNull() && m_registry.valid(entity.handle);
}

const Uuid& World::id(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<EntityInfo>(entity.handle).id;
}

const std::string& World::name(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<EntityInfo>(entity.handle).name;
}

void World::setName(Entity entity, std::string_view name) {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    m_registry.get<EntityInfo>(entity.handle).name = name;
}

bool World::isEnabled(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<EntityInfo>(entity.handle).enabled;
}

void World::setEnabled(Entity entity, bool enabled) {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    m_registry.get<EntityInfo>(entity.handle).enabled = enabled;
}

bool World::isEnabledInHierarchy(Entity entity) const {
    if (!isValid(entity)) {
        return false;
    }
    for (entt::entity current = entity.handle; current != entt::null;
         current = m_registry.get<Hierarchy>(current).parent) {
        if (!m_registry.get<EntityInfo>(current).enabled) {
            return false;
        }
    }
    return true;
}

Entity World::findById(const Uuid& id) const {
    const auto found = m_entitiesById.find(id);
    return found != m_entitiesById.end() ? Entity{found->second} : Entity{};
}

Entity World::findByName(std::string_view name) const {
    Entity found;
    visitDepthFirst(m_registry, m_roots, [&](entt::entity entity, uint32_t /*depth*/) {
        if (found.isNull() && m_registry.get<EntityInfo>(entity).name == name) {
            found = Entity{entity};
        }
    });
    return found;
}

Status World::setParent(Entity child, Entity parent, bool keepWorldTransform) {
    if (!isValid(child)) {
        return makeError(ErrorCode::InvalidArgument, "the entity to reparent is not valid");
    }
    if (!parent.isNull() && !isValid(parent)) {
        return makeError(ErrorCode::InvalidArgument, "the new parent is not a valid entity");
    }
    if (parent == child || (!parent.isNull() && isDescendantOf(parent, child))) {
        return makeError(ErrorCode::InvalidArgument, "'{}' cannot become a child of itself or of its descendant '{}'",
                         name(child), name(parent));
    }

    Hierarchy& hierarchy = m_registry.get<Hierarchy>(child.handle);
    if (hierarchy.parent == parent.handle) {
        return {};
    }

    // Capture the placement before the links change.
    const Mat4 worldMatrix = keepWorldTransform ? computeWorldMatrix(child) : Mat4(1.0f);

    std::erase(siblingsOf(child.handle), child.handle);
    hierarchy.parent = parent.handle;
    siblingsOf(child.handle).push_back(child.handle);

    if (keepWorldTransform) {
        const Mat4 parentMatrix = parent.isNull() ? Mat4(1.0f) : computeWorldMatrix(parent);
        m_registry.get<Transform>(child.handle) = Transform::fromMatrix(glm::inverse(parentMatrix) * worldMatrix);
    }
    return {};
}

Entity World::parent(Entity entity) const {
    return isValid(entity) ? Entity{m_registry.get<Hierarchy>(entity.handle).parent} : Entity{};
}

std::vector<Entity> World::children(Entity entity) const {
    if (!entity.isNull() && !isValid(entity)) {
        return {};
    }

    const std::vector<entt::entity>& handles =
        entity.isNull() ? m_roots : m_registry.get<Hierarchy>(entity.handle).children;
    std::vector<Entity> result;
    result.reserve(handles.size());
    for (const entt::entity handle : handles) {
        result.push_back(Entity{handle});
    }
    return result;
}

size_t World::siblingIndex(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    const std::vector<entt::entity>& siblings = siblingsOf(entity.handle);
    return static_cast<size_t>(std::ranges::find(siblings, entity.handle) - siblings.begin());
}

void World::setSiblingIndex(Entity entity, size_t index) {
    if (!isValid(entity)) {
        return;
    }
    std::vector<entt::entity>& siblings = siblingsOf(entity.handle);
    std::erase(siblings, entity.handle);
    siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(std::min(index, siblings.size())), entity.handle);
}

bool World::isDescendantOf(Entity entity, Entity ancestor) const {
    if (!isValid(entity) || !isValid(ancestor)) {
        return false;
    }
    for (entt::entity current = m_registry.get<Hierarchy>(entity.handle).parent; current != entt::null;
         current = m_registry.get<Hierarchy>(current).parent) {
        if (current == ancestor.handle) {
            return true;
        }
    }
    return false;
}

void World::traverse(const Visitor& visitor) const {
    visitDepthFirst(m_registry, m_roots,
                    [&visitor](entt::entity entity, uint32_t depth) { visitor(Entity{entity}, depth); });
}

void World::traverseSubtree(Entity root, const Visitor& visitor) const {
    if (!isValid(root)) {
        return;
    }
    visitDepthFirst(m_registry, std::span(&root.handle, 1),
                    [&visitor](entt::entity entity, uint32_t depth) { visitor(Entity{entity}, depth); });
}

Transform& World::transform(Entity entity) {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<Transform>(entity.handle);
}

const Transform& World::transform(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<Transform>(entity.handle);
}

void World::updateWorldTransforms() {
    visitDepthFirst(m_registry, m_roots, [this](entt::entity entity, uint32_t /*depth*/) {
        const entt::entity parentHandle = m_registry.get<Hierarchy>(entity).parent;
        const Mat4 local = m_registry.get<Transform>(entity).toMatrix();
        // Parents are visited first, so the parent's matrix is already up to date.
        m_registry.get<WorldTransform>(entity).matrix =
            parentHandle == entt::null ? local : m_registry.get<WorldTransform>(parentHandle).matrix * local;
    });
}

const Mat4& World::worldMatrix(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");
    return m_registry.get<WorldTransform>(entity.handle).matrix;
}

Mat4 World::computeWorldMatrix(Entity entity) const {
    CORDITE_ASSERT(isValid(entity), "invalid entity");

    Mat4 matrix(1.0f);
    for (entt::entity current = entity.handle; current != entt::null;
         current = m_registry.get<Hierarchy>(current).parent) {
        matrix = m_registry.get<Transform>(current).toMatrix() * matrix;
    }
    return matrix;
}

} // namespace cordite
