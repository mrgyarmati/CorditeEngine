#include "cordite/editor/EditorActions.h"

#include "cordite/world/SceneSerializer.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace cordite {
namespace {

/// Where an entity sits among its siblings, for putting it back there.
struct Placement {
    Uuid id;
    size_t siblingIndex = 0;
};

Result<Entity> findEntity(EditorContext& context, const Uuid& id) {
    const Entity entity = context.world().findById(id);
    if (entity.isNull()) {
        return makeError(ErrorCode::InvalidState, "the entity {} no longer exists", id.toString());
    }
    return entity;
}

Status requireValid(const EditorContext& context, Entity entity) {
    if (!context.world().isValid(entity)) {
        return makeError(ErrorCode::InvalidArgument, "the entity is not valid");
    }
    return {};
}

Result<const ComponentDescriptor*> findComponent(const EditorContext& context, std::string_view name) {
    const ComponentDescriptor* descriptor = context.components().find(name);
    if (descriptor == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no component type named '{}'", name);
    }
    return descriptor;
}

/// The instance of a component on an entity that a history entry refers to by id.
Result<void*> findInstance(EditorContext& context, const Uuid& id, const ComponentDescriptor& descriptor) {
    CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, id));
    void* instance = descriptor.get(context.world().registry(), entity.handle);
    if (instance == nullptr) {
        return makeError(ErrorCode::InvalidState, "'{}' no longer has a {} component", context.world().name(entity),
                         descriptor.type.name());
    }
    return instance;
}

/// The valid entities of `entities` that are not listed twice and not below another
/// listed entity, in the order given.
std::vector<Entity> topLevelEntities(const World& world, std::span<const Entity> entities) {
    const std::unordered_set<Entity> listed(entities.begin(), entities.end());
    std::unordered_set<Entity> taken;
    std::vector<Entity> result;
    for (const Entity entity : entities) {
        if (!world.isValid(entity) || !taken.insert(entity).second) {
            continue;
        }
        bool covered = false;
        for (Entity ancestor = world.parent(entity); !ancestor.isNull(); ancestor = world.parent(ancestor)) {
            covered = covered || listed.contains(ancestor);
        }
        if (!covered) {
            result.push_back(entity);
        }
    }
    return result;
}

std::vector<Placement> placementsOf(const World& world, std::span<const Entity> entities) {
    std::vector<Placement> placements;
    placements.reserve(entities.size());
    for (const Entity entity : entities) {
        placements.push_back({world.id(entity), world.siblingIndex(entity)});
    }
    // Lowest position first: putting entities back in that order makes each index
    // mean what it meant when it was recorded.
    std::ranges::sort(placements, {}, &Placement::siblingIndex);
    return placements;
}

/// Brings back entities from a snapshot made by serializeEntities, with their ids, under
/// the parents they had and at the positions they had.
Status restoreEntities(EditorContext& context, const Json& snapshot, std::span<const Placement> placements) {
    CORDITE_TRY(instantiateEntities(context.world(), snapshot, context.components()));
    for (const Placement& placement : placements) {
        CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, placement.id));
        context.world().setSiblingIndex(entity, placement.siblingIndex);
    }
    return {};
}

Status destroyEntities(EditorContext& context, std::span<const Uuid> ids) {
    for (const Uuid& id : ids) {
        CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, id));
        context.world().destroyEntity(entity);
    }
    return {};
}

std::vector<Uuid> idsOf(const World& world, std::span<const Entity> entities) {
    std::vector<Uuid> ids;
    ids.reserve(entities.size());
    for (const Entity entity : entities) {
        ids.push_back(world.id(entity));
    }
    return ids;
}

std::string describeCount(std::span<const Entity> entities, const World& world) {
    return entities.size() == 1 ? fmt::format("'{}'", world.name(entities.front()))
                                : fmt::format("{} entities", entities.size());
}

/// The state of an entity's place in the hierarchy that reparenting changes.
struct Attachment {
    /// Nil for a root.
    Uuid parentId;
    size_t siblingIndex = 0;
    Transform transform;

    bool operator==(const Attachment&) const = default;
};

Attachment attachmentOf(const World& world, Entity entity) {
    const Entity parent = world.parent(entity);
    return {parent.isNull() ? Uuid() : world.id(parent), world.siblingIndex(entity), world.transform(entity)};
}

Status applyAttachment(EditorContext& context, const Uuid& id, const Attachment& attachment) {
    World& world = context.world();
    CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, id));
    Entity parent;
    if (!attachment.parentId.isNil()) {
        CORDITE_TRY_ASSIGN(parent, findEntity(context, attachment.parentId));
    }

    const bool keepWorldTransform = false;
    CORDITE_TRY(world.setParent(entity, parent, keepWorldTransform));
    world.setSiblingIndex(entity, attachment.siblingIndex);
    world.transform(entity) = attachment.transform;
    return {};
}

/// Pushes an entry that applies `before` to a component when undone and `after` when
/// redone.
void pushComponentEdit(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                       PropertyPatch before, PropertyPatch after) {
    EditorContext* target = &context;
    const ComponentDescriptor* type = &descriptor;
    const Uuid id = context.world().id(entity);
    const auto apply = [target, type, id](const PropertyPatch& patch) -> Status {
        CORDITE_TRY_ASSIGN(void* instance, findInstance(*target, id, *type));
        patch.applyTo(instance);
        return {};
    };

    context.history().push({
        .label = fmt::format("Edit {} of '{}'", descriptor.type.name(), context.world().name(entity)),
        .undo = [apply, before = std::move(before)] { return apply(before); },
        .redo = [apply, after = std::move(after)] { return apply(after); },
    });
}

} // namespace

Result<Entity> EditorActions::createEntity(const CreateEntityOptions& options) {
    World& world = m_context.world();
    if (!options.parent.isNull() && !world.isValid(options.parent)) {
        return makeError(ErrorCode::InvalidArgument, "the parent for the new entity is not valid");
    }
    if (options.name.empty() || options.name.find('/') != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument, "an entity name must not be empty or contain '/', got '{}'",
                         options.name);
    }
    if (!options.components.is_object()) {
        return makeError(ErrorCode::InvalidArgument,
                         "'components' must be an object mapping component names to properties, found {}",
                         jsonTypeName(options.components));
    }

    // Described as scene data and created by the scene loader, which validates every
    // component before anything exists.
    const Uuid id = Uuid::generate();
    Json record = Json::object();
    record["id"] = id.toString();
    record["name"] = options.name;
    record["enabled"] = true;
    record["parent"] = options.parent.isNull() ? Json(nullptr) : Json(world.id(options.parent).toString());
    record["components"] = options.components;
    Json snapshot = Json::array();
    snapshot.push_back(std::move(record));

    CORDITE_TRY_ASSIGN(const std::vector<Entity> created, instantiateEntities(world, snapshot, m_context.components()));

    EditorContext* context = &m_context;
    m_context.history().push({
        .label = fmt::format("Create '{}'", options.name),
        .undo = [context, id] { return destroyEntities(*context, std::span(&id, 1)); },
        .redo = [context, snapshot = std::move(snapshot)] { return restoreEntities(*context, snapshot, {}); },
    });
    return created.front();
}

Status EditorActions::deleteEntities(std::span<const Entity> entities) {
    World& world = m_context.world();
    const std::vector<Entity> roots = topLevelEntities(world, entities);
    if (roots.empty()) {
        return makeError(ErrorCode::InvalidArgument, "there is no valid entity to delete");
    }

    std::string label = fmt::format("Delete {}", describeCount(roots, world));
    Json snapshot = serializeEntities(world, roots, m_context.components());
    std::vector<Placement> placements = placementsOf(world, roots);
    std::vector<Uuid> ids = idsOf(world, roots);
    for (const Entity root : roots) {
        world.destroyEntity(root);
    }

    EditorContext* context = &m_context;
    m_context.history().push({
        .label = std::move(label),
        .undo = [context, snapshot = std::move(snapshot),
                 placements = std::move(placements)] { return restoreEntities(*context, snapshot, placements); },
        .redo = [context, ids = std::move(ids)] { return destroyEntities(*context, ids); },
    });
    return {};
}

Result<std::vector<Entity>> EditorActions::duplicateEntities(std::span<const Entity> entities) {
    World& world = m_context.world();
    const std::vector<Entity> originals = topLevelEntities(world, entities);
    if (originals.empty()) {
        return makeError(ErrorCode::InvalidArgument, "there is no valid entity to duplicate");
    }

    const Json snapshot = serializeEntities(world, originals, m_context.components());
    CORDITE_TRY_ASSIGN(const std::vector<Entity> created,
                       instantiateEntities(world, snapshot, m_context.components(), {.generateNewIds = true}));

    // `created` lists every copy, parents first; the copy of each original is the first
    // entity of its subtree. Each one goes right after the entity it was copied from.
    std::vector<Entity> copies;
    copies.reserve(originals.size());
    size_t offset = 0;
    for (const Entity original : originals) {
        const Entity copy = created[offset];
        world.setSiblingIndex(copy, world.siblingIndex(original) + 1);
        copies.push_back(copy);
        world.traverseSubtree(original, [&offset](Entity /*entity*/, uint32_t /*depth*/) { ++offset; });
    }

    // Redoing must bring back these very entities, ids included, so that later history
    // entries still find them.
    Json copiesSnapshot = serializeEntities(world, copies, m_context.components());
    std::vector<Placement> placements = placementsOf(world, copies);
    std::vector<Uuid> ids = idsOf(world, copies);

    EditorContext* context = &m_context;
    m_context.history().push({
        .label = fmt::format("Duplicate {}", describeCount(originals, world)),
        .undo = [context, ids = std::move(ids)] { return destroyEntities(*context, ids); },
        .redo = [context, copiesSnapshot = std::move(copiesSnapshot),
                 placements = std::move(placements)] { return restoreEntities(*context, copiesSnapshot, placements); },
    });

    m_context.setSelection(copies);
    return copies;
}

Result<Entity> EditorActions::instantiateModel(const Uuid& model, const ModelInstanceOptions& options) {
    World& world = m_context.world();
    if (!options.parent.isNull()) {
        CORDITE_TRY(requireValid(m_context, options.parent));
    }
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> data, m_context.assets().loadModel(model));

    const Entity instance = cordite::instantiateModel(world, *data, options);

    // Redoing must bring back these very entities, ids included, so that later history
    // entries still find them.
    const std::vector<Entity> roots = {instance};
    Json snapshot = serializeEntities(world, roots, m_context.components());
    std::vector<Placement> placements = placementsOf(world, roots);
    std::vector<Uuid> ids = idsOf(world, roots);

    EditorContext* context = &m_context;
    m_context.history().push({
        .label = fmt::format("Place '{}'", world.name(instance)),
        .undo = [context, ids = std::move(ids)] { return destroyEntities(*context, ids); },
        .redo = [context, snapshot = std::move(snapshot),
                 placements = std::move(placements)] { return restoreEntities(*context, snapshot, placements); },
    });

    m_context.setSelection(roots);
    return instance;
}

Status EditorActions::renameEntity(Entity entity, std::string_view name) {
    CORDITE_TRY(requireValid(m_context, entity));
    if (name.empty() || name.find('/') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "an entity name must not be empty or contain '/', got '{}'", name);
    }

    World& world = m_context.world();
    std::string before = world.name(entity);
    if (before == name) {
        return {};
    }
    world.setName(entity, name);

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);
    const auto apply = [context, id](const std::string& value) -> Status {
        CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
        context->world().setName(target, value);
        return {};
    };
    m_context.history().push({
        .label = fmt::format("Rename '{}' to '{}'", before, name),
        .undo = [apply, before = std::move(before)] { return apply(before); },
        .redo = [apply, after = std::string(name)] { return apply(after); },
    });
    return {};
}

Status EditorActions::setEntityEnabled(Entity entity, bool enabled) {
    CORDITE_TRY(requireValid(m_context, entity));
    World& world = m_context.world();
    if (world.isEnabled(entity) == enabled) {
        return {};
    }
    world.setEnabled(entity, enabled);

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);
    const auto apply = [context, id](bool value) -> Status {
        CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
        context->world().setEnabled(target, value);
        return {};
    };
    m_context.history().push({
        .label = fmt::format("{} '{}'", enabled ? "Enable" : "Disable", world.name(entity)),
        .undo = [apply, enabled] { return apply(!enabled); },
        .redo = [apply, enabled] { return apply(enabled); },
    });
    return {};
}

Status EditorActions::setParent(Entity entity, Entity parent, std::optional<size_t> siblingIndex,
                                bool keepWorldTransform) {
    CORDITE_TRY(requireValid(m_context, entity));
    World& world = m_context.world();

    const Attachment before = attachmentOf(world, entity);
    CORDITE_TRY(world.setParent(entity, parent, keepWorldTransform));
    if (siblingIndex) {
        world.setSiblingIndex(entity, *siblingIndex);
    }
    const Attachment after = attachmentOf(world, entity);
    if (after == before) {
        return {};
    }

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);
    m_context.history().push({
        .label = fmt::format("Move '{}'", world.name(entity)),
        .undo = [context, id, before] { return applyAttachment(*context, id, before); },
        .redo = [context, id, after] { return applyAttachment(*context, id, after); },
    });
    return {};
}

Status EditorActions::addComponent(Entity entity, std::string_view component, const Json& properties) {
    CORDITE_TRY(requireValid(m_context, entity));
    CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, findComponent(m_context, component));
    World& world = m_context.world();
    if (descriptor->has(world.registry(), entity.handle)) {
        return makeError(ErrorCode::AlreadyExists, "'{}' already has a {} component", world.name(entity), component);
    }

    CORDITE_TRY_ASSIGN(PropertyPatch patch, descriptor->type.parse(properties));
    descriptor->emplace(world.registry(), entity.handle, patch);

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);
    m_context.history().push({
        .label = fmt::format("Add {} to '{}'", component, world.name(entity)),
        .undo = [context, id, descriptor]() -> Status {
            CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
            descriptor->remove(context->world().registry(), target.handle);
            return {};
        },
        .redo = [context, id, descriptor, patch = std::move(patch)]() -> Status {
            CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
            descriptor->emplace(context->world().registry(), target.handle, patch);
            return {};
        },
    });
    return {};
}

Status EditorActions::removeComponent(Entity entity, std::string_view component) {
    CORDITE_TRY(requireValid(m_context, entity));
    CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, findComponent(m_context, component));
    World& world = m_context.world();
    if (!descriptor->traits.optional) {
        return makeError(ErrorCode::InvalidArgument, "every entity has a {} component; it cannot be removed",
                         component);
    }
    const void* instance = descriptor->getConst(world.registry(), entity.handle);
    if (instance == nullptr) {
        return makeError(ErrorCode::NotFound, "'{}' has no {} component", world.name(entity), component);
    }

    // What it takes to bring the component back exactly as it is.
    CORDITE_TRY_ASSIGN(PropertyPatch restore, descriptor->type.parse(descriptor->type.toJson(instance)));
    descriptor->remove(world.registry(), entity.handle);

    EditorContext* context = &m_context;
    const Uuid id = world.id(entity);
    m_context.history().push({
        .label = fmt::format("Remove {} from '{}'", component, world.name(entity)),
        .undo = [context, id, descriptor, restore = std::move(restore)]() -> Status {
            CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
            descriptor->emplace(context->world().registry(), target.handle, restore);
            return {};
        },
        .redo = [context, id, descriptor]() -> Status {
            CORDITE_TRY_ASSIGN(const Entity target, findEntity(*context, id));
            descriptor->remove(context->world().registry(), target.handle);
            return {};
        },
    });
    return {};
}

Status EditorActions::setComponentProperties(Entity entity, std::string_view component, const Json& properties) {
    CORDITE_TRY(requireValid(m_context, entity));
    CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, findComponent(m_context, component));
    World& world = m_context.world();
    void* instance = descriptor->get(world.registry(), entity.handle);
    if (instance == nullptr) {
        return makeError(ErrorCode::NotFound, "'{}' has no {} component", world.name(entity), component);
    }

    CORDITE_TRY_ASSIGN(PropertyPatch edit, descriptor->type.parseEdit(properties));
    const Json before = descriptor->type.toJson(instance);
    CORDITE_TRY_ASSIGN(PropertyPatch restore, descriptor->type.parse(before));

    edit.applyTo(instance);
    if (descriptor->type.toJson(instance) == before) {
        // Setting properties to the values they have is not an edit.
        return {};
    }
    pushComponentEdit(m_context, entity, *descriptor, std::move(restore), std::move(edit));
    return {};
}

Status EditorActions::recordComponentEdit(Entity entity, std::string_view component, const Json& before) {
    CORDITE_TRY(requireValid(m_context, entity));
    CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, findComponent(m_context, component));
    World& world = m_context.world();
    const void* instance = descriptor->getConst(world.registry(), entity.handle);
    if (instance == nullptr) {
        return makeError(ErrorCode::NotFound, "'{}' has no {} component", world.name(entity), component);
    }

    const Json after = descriptor->type.toJson(instance);
    if (after == before) {
        return {};
    }
    CORDITE_TRY_ASSIGN(PropertyPatch restore, descriptor->type.parse(before));
    CORDITE_TRY_ASSIGN(PropertyPatch redo, descriptor->type.parse(after));
    pushComponentEdit(m_context, entity, *descriptor, std::move(restore), std::move(redo));
    return {};
}

} // namespace cordite
