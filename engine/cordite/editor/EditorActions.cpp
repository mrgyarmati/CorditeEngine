#include "cordite/editor/EditorActions.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Model.h"
#include "cordite/assets/Prefab.h"
#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/PrefabLinks.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/EntityPath.h"
#include "cordite/world/Prefab.h"
#include "cordite/world/SceneSerializer.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <system_error>
#include <tuple>
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

/// The entities under one root as an edit found them and as it left them: a step of the
/// history that puts back one or the other whole, for edits that change what an instance
/// of a prefab is made of.
struct SubtreeChange {
    Uuid root;
    Json before;
    Json after;
    std::vector<Placement> placement;
};

/// The entities under `root` as they are, as `before` of a change.
SubtreeChange subtreeBefore(const EditorContext& context, Entity root) {
    const std::array<Entity, 1> roots = {root};
    return {.root = context.world().id(root),
            .before = serializeEntities(context.world(), roots, context.components()),
            .placement = placementsOf(context.world(), roots)};
}

/// Fills in what `change` left of the entities under its root.
Status subtreeAfter(EditorContext& context, SubtreeChange& change) {
    CORDITE_TRY_ASSIGN(const Entity root, findEntity(context, change.root));
    const std::array<Entity, 1> roots = {root};
    change.after = serializeEntities(context.world(), roots, context.components());
    return {};
}

/// A step of the history that puts back the entities of `changes`, as they were when undone
/// - the last first, so that what is inside another goes back before it - and as they were
/// left when redone.
UndoEntry subtreeEdit(EditorContext& context, std::string label, std::vector<SubtreeChange> changes) {
    EditorContext* target = &context;
    const auto shared = std::make_shared<const std::vector<SubtreeChange>>(std::move(changes));
    const auto apply = [target, shared](bool undo) -> Status {
        const auto put = [target, undo](const SubtreeChange& change) -> Status {
            const std::array<Uuid, 1> ids = {change.root};
            CORDITE_TRY(destroyEntities(*target, ids));
            return restoreEntities(*target, undo ? change.before : change.after, change.placement);
        };
        if (undo) {
            for (const SubtreeChange& change : std::views::reverse(*shared)) {
                CORDITE_TRY(put(change));
            }
        } else {
            for (const SubtreeChange& change : *shared) {
                CORDITE_TRY(put(change));
            }
        }
        return {};
    };
    return {
        .label = std::move(label), .undo = [apply] { return apply(true); }, .redo = [apply] { return apply(false); }};
}

/// What makes an entity and what is below it an instance of a prefab: the PrefabInstance of
/// the root and the PrefabLink of each, or none.
struct PrefabMark {
    Uuid root;
    std::optional<PrefabInstanceComponent> instance;
    std::vector<std::pair<Uuid, std::optional<PrefabLinkComponent>>> links;

    friend bool operator==(const PrefabMark& first, const PrefabMark& second) {
        const auto sameInstance = [](const std::optional<PrefabInstanceComponent>& a,
                                     const std::optional<PrefabInstanceComponent>& b) {
            return a.has_value() == b.has_value() && (!a || a->prefab == b->prefab);
        };
        const auto sameLink = [](const std::optional<PrefabLinkComponent>& a,
                                 const std::optional<PrefabLinkComponent>& b) {
            return a.has_value() == b.has_value() && (!a || (a->source == b->source && a->overrides == b->overrides));
        };
        return first.root == second.root && sameInstance(first.instance, second.instance) &&
               std::ranges::equal(first.links, second.links, [&](const auto& a, const auto& b) {
                   return a.first == b.first && sameLink(a.second, b.second);
               });
    }
};

/// The mark of `root` and of everything below it, as it is.
PrefabMark prefabMarkOf(const World& world, Entity root) {
    PrefabMark mark{.root = world.id(root)};
    if (const auto* instance = world.tryGet<PrefabInstanceComponent>(root)) {
        mark.instance = *instance;
    }
    std::vector<Entity> pending = {root};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        std::optional<PrefabLinkComponent> link;
        if (const auto* held = world.tryGet<PrefabLinkComponent>(entity)) {
            link = *held;
        }
        mark.links.emplace_back(world.id(entity), std::move(link));
        for (const Entity child : world.children(entity)) {
            pending.push_back(child);
        }
    }
    return mark;
}

/// Gives the entities of `mark` the components it says, and takes away those it has none of.
/// Every entity is found before any is changed.
Status applyPrefabMark(EditorContext& context, const PrefabMark& mark) {
    CORDITE_TRY_ASSIGN(const Entity root, findEntity(context, mark.root));
    std::vector<Entity> entities;
    entities.reserve(mark.links.size());
    for (const auto& [id, link] : mark.links) {
        CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, id));
        entities.push_back(entity);
    }
    World& world = context.world();
    if (mark.instance) {
        world.add<PrefabInstanceComponent>(root) = *mark.instance;
    } else {
        world.remove<PrefabInstanceComponent>(root);
    }
    for (size_t index = 0; index < entities.size(); ++index) {
        if (const std::optional<PrefabLinkComponent>& link = mark.links[index].second) {
            world.add<PrefabLinkComponent>(entities[index]) = *link;
        } else {
            world.remove<PrefabLinkComponent>(entities[index]);
        }
    }
    return {};
}

/// What an edit changed of the overrides of a linked copy of a prefab's entity.
struct OverrideChange {
    Uuid id;
    std::vector<std::string> before;
    std::vector<std::string> after;
};

/// Notes in the PrefabLink of `entity` what `change` makes of its overrides - what an edit
/// of a linked copy keeps of its own when the prefab changes - and adds that to `changes`.
/// Nothing for an entity without a link, or a change that changes nothing.
void noteOverrides(World& world, Entity entity, const std::function<void(std::vector<std::string>&)>& change,
                   std::vector<OverrideChange>& changes) {
    auto* link = world.tryGet<PrefabLinkComponent>(entity);
    if (link == nullptr) {
        return;
    }
    std::vector<std::string> after = link->overrides;
    change(after);
    if (after == link->overrides) {
        return;
    }
    changes.push_back({.id = world.id(entity), .before = link->overrides, .after = after});
    link->overrides = std::move(after);
}

/// Whether a component is one of what marks an instance of a prefab - never an override.
bool isPrefabMark(std::string_view component) {
    return component == PrefabInstanceComponent::kName || component == PrefabLinkComponent::kName;
}

/// Adds `entry` to the overrides, when it is not among them.
void addOverride(std::vector<std::string>& overrides, std::string entry) {
    if (std::ranges::find(overrides, entry) == overrides.end()) {
        overrides.push_back(std::move(entry));
    }
}

/// Notes the properties of `component` that differ between `before` and `after` - the
/// component as an edit found it and left it - as overrides of `entity`. The Transform of
/// the root of an instance is always its own, and not noted.
void noteChangedProperties(World& world, Entity entity, const std::string& component, const Json& before,
                           const Json& after, std::vector<OverrideChange>& changes) {
    if (component == "Transform" && world.has<PrefabInstanceComponent>(entity)) {
        return;
    }
    noteOverrides(
        world, entity,
        [&](std::vector<std::string>& overrides) {
            for (const auto& [property, value] : after.items()) {
                const auto was = before.find(property);
                if (was == before.end() || *was != value) {
                    addOverride(overrides, fmt::format("{}.{}", component, property));
                }
            }
        },
        changes);
}

/// Sets the overrides of the entity of `id` to `overrides`, when it still has a link.
Status setOverrides(EditorContext& context, const Uuid& id, const std::vector<std::string>& overrides) {
    CORDITE_TRY_ASSIGN(const Entity entity, findEntity(context, id));
    if (auto* link = context.world().tryGet<PrefabLinkComponent>(entity)) {
        link->overrides = overrides;
    }
    return {};
}

/// `entry`, made to undo and redo `changes` of overrides along with what it does: an edit of
/// a linked copy and what it keeps of its own are one step of the history.
UndoEntry withOverrideChanges(EditorContext& context, UndoEntry entry, std::vector<OverrideChange> changes) {
    if (changes.empty()) {
        return entry;
    }
    EditorContext* target = &context;
    const auto shared = std::make_shared<const std::vector<OverrideChange>>(std::move(changes));
    entry.undo = [target, shared, undo = std::move(entry.undo)]() -> Status {
        CORDITE_TRY(undo());
        for (const OverrideChange& change : *shared) {
            CORDITE_TRY(setOverrides(*target, change.id, change.before));
        }
        return {};
    };
    entry.redo = [target, shared, redo = std::move(entry.redo)]() -> Status {
        CORDITE_TRY(redo());
        for (const OverrideChange& change : *shared) {
            CORDITE_TRY(setOverrides(*target, change.id, change.after));
        }
        return {};
    };
    return entry;
}

/// Pushes an entry that applies `before` to a component when undone and `after` when
/// redone.
void pushComponentEdit(EditorContext& context, Entity entity, const ComponentDescriptor& descriptor,
                       PropertyPatch before, PropertyPatch after, std::vector<OverrideChange> overrides) {
    EditorContext* target = &context;
    const ComponentDescriptor* type = &descriptor;
    const Uuid id = context.world().id(entity);
    const auto apply = [target, type, id](const PropertyPatch& patch) -> Status {
        CORDITE_TRY_ASSIGN(void* instance, findInstance(*target, id, *type));
        patch.applyTo(instance);
        return {};
    };

    context.history().push(withOverrideChanges(
        context,
        {
            .label = fmt::format("Edit {} of '{}'", descriptor.type.name(), context.world().name(entity)),
            .undo = [apply, before = std::move(before)] { return apply(before); },
            .redo = [apply, after = std::move(after)] { return apply(after); },
        },
        std::move(overrides)));
}

/// What one entity's component was and is, for an edit of several.
struct EntityPatches {
    Uuid id;
    PropertyPatch before;
    PropertyPatch after;
};

void pushComponentEdits(EditorContext& context, const ComponentDescriptor& descriptor,
                        std::vector<EntityPatches> patches, std::vector<OverrideChange> overrides) {
    EditorContext* target = &context;
    const ComponentDescriptor* type = &descriptor;
    // Shared by undo and redo, which are copied about with the history.
    const auto shared = std::make_shared<const std::vector<EntityPatches>>(std::move(patches));
    const auto apply = [target, type, shared](bool undo) -> Status {
        // Every component is found before any is changed: an edit is applied whole or not at all.
        std::vector<void*> instances;
        instances.reserve(shared->size());
        for (const EntityPatches& entity : *shared) {
            CORDITE_TRY_ASSIGN(void* instance, findInstance(*target, entity.id, *type));
            instances.push_back(instance);
        }
        for (size_t index = 0; index < shared->size(); ++index) {
            const EntityPatches& entity = (*shared)[index];
            (undo ? entity.before : entity.after).applyTo(instances[index]);
        }
        return {};
    };
    context.history().push(
        withOverrideChanges(context,
                            {
                                .label = fmt::format("Edit {} of {} entities", descriptor.type.name(), shared->size()),
                                .undo = [apply] { return apply(true); },
                                .redo = [apply] { return apply(false); },
                            },
                            std::move(overrides)));
}

/// What placing anything asks of where and as what, before anything is loaded: a parent
/// that is an entity, a name a path can find, a position that is a place.
Status checkPlacement(const EditorContext& context, Entity parent, std::string_view name,
                      const std::optional<Vec3>& position) {
    if (!parent.isNull()) {
        CORDITE_TRY(requireValid(context, parent));
    }
    if (name.find('/') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "an entity name must not contain '/', got '{}'", name);
    }
    if (position && !isFinite(*position)) {
        return makeError(ErrorCode::InvalidArgument,
                         "a position to place something at must be finite, got [{}, {}, {}]", position->x, position->y,
                         position->z);
    }
    return {};
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

Status EditorActions::copyEntities(std::span<const Entity> entities) {
    const World& world = m_context.world();
    const std::vector<Entity> copied = topLevelEntities(world, entities);
    if (copied.empty()) {
        return makeError(ErrorCode::InvalidArgument, "there is no valid entity to copy");
    }
    m_context.setClipboard(serializeEntities(world, copied, m_context.components()));
    return {};
}

Status EditorActions::cutEntities(std::span<const Entity> entities) {
    CORDITE_TRY(copyEntities(entities));
    return deleteEntities(entities);
}

Result<std::vector<Entity>> EditorActions::pasteEntities(std::optional<Entity> parent) {
    const Json& copied = m_context.clipboard();
    if (!copied.is_array() || copied.empty()) {
        return makeError(ErrorCode::NotFound, "nothing has been copied to paste");
    }
    World& world = m_context.world();
    CORDITE_TRY_ASSIGN(
        const std::vector<Entity> created,
        instantiateEntities(world, copied, m_context.components(), {.parent = parent, .generateNewIds = true}));
    // What was copied as such, not what came along below it.
    std::vector<Entity> pasted = topLevelEntities(world, created);

    // Redoing must bring back these very entities, ids included, so that later history
    // entries still find them.
    Json snapshot = serializeEntities(world, pasted, m_context.components());
    std::vector<Placement> placements = placementsOf(world, pasted);
    std::vector<Uuid> ids = idsOf(world, pasted);
    EditorContext* context = &m_context;
    m_context.history().push({
        .label = fmt::format("Paste {}", describeCount(pasted, world)),
        .undo = [context, ids = std::move(ids)] { return destroyEntities(*context, ids); },
        .redo = [context, snapshot = std::move(snapshot),
                 placements = std::move(placements)] { return restoreEntities(*context, snapshot, placements); },
    });
    m_context.setSelection(pasted);
    return pasted;
}

Result<Entity> EditorActions::instantiateModel(const Uuid& model, const ModelInstanceOptions& options) {
    CORDITE_TRY(checkPlacement(m_context, options.parent, options.name, options.position));
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> data, m_context.assets().loadModel(model));

    const Entity instance = cordite::instantiateModel(m_context.world(), *data, options);
    recordPlaced(instance);
    return instance;
}

Result<Entity> EditorActions::instantiatePrefab(const Uuid& prefab, const PrefabInstanceOptions& options) {
    CORDITE_TRY(checkPlacement(m_context, options.parent, options.name, options.position));
    CORDITE_TRY_ASSIGN(const Json entities, currentPrefabEntities(m_context.assets(), m_context.components(), prefab));

    const Result<Entity> instance =
        cordite::instantiatePrefab(m_context.world(), m_context.components(), entities, prefab, options);
    if (!instance) {
        // What is wrong is wrong with a file, and the message says which.
        const AssetInfo* asset = m_context.assets().database().find(prefab);
        return tl::unexpected(instance.error().withContext(asset != nullptr ? asset->reference() : prefab.toString()));
    }
    recordPlaced(*instance);
    return *instance;
}

Result<Entity> EditorActions::instantiateMesh(const Uuid& mesh, const PlacementOptions& options) {
    AssetManager& assets = m_context.assets();
    const AssetInfo* info = assets.database().find(mesh);
    if (info == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset with the id {}", mesh.toString());
    }
    if (info->type != AssetType::Mesh) {
        return makeError(ErrorCode::InvalidArgument, "{} is {}, not a mesh", info->reference(),
                         withArticle(info->type));
    }
    CORDITE_TRY(checkPlacement(m_context, options.parent, options.name, options.position));
    // Loaded first: what cannot be drawn is not placed.
    CORDITE_TRY(assets.loadMesh(mesh));
    // A mesh of a model is drawn as the model draws it.
    std::vector<Uuid> materials;
    if (const AssetInfo* file =
            (info->subPath.empty() || info->isBuiltin()) ? nullptr : assets.database().findByReference(info->path);
        file != nullptr && file->type == AssetType::Model) {
        CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> model, assets.loadModel(file->id));
        materials = materialsOfMesh(*model, mesh);
    }

    World& world = m_context.world();
    const Entity entity = world.createEntity(options.name.empty() ? entityNameFrom(info->name, "Mesh") : options.name);
    if (!options.parent.isNull()) {
        // A new entity has no children, so this cannot make a cycle and cannot fail.
        CORDITE_VERIFY(world.setParent(entity, options.parent, false).has_value(), "cannot put {} under its parent",
                       info->reference());
    }
    if (options.position) {
        world.transform(entity).position = *options.position;
    }
    auto& renderer = world.add<MeshRendererComponent>(entity);
    renderer.mesh = mesh;
    renderer.materials = std::move(materials);
    recordPlaced(entity);
    return entity;
}

Result<Entity> EditorActions::instantiateAsset(const Uuid& asset, const PlacementOptions& options) {
    const AssetInfo* info = m_context.assets().database().find(asset);
    if (info == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset with the id {}", asset.toString());
    }
    switch (info->type) {
        case AssetType::Model:
            return instantiateModel(asset,
                                    {.parent = options.parent, .name = options.name, .position = options.position});
        case AssetType::Prefab:
            return instantiatePrefab(asset,
                                     {.parent = options.parent, .name = options.name, .position = options.position});
        case AssetType::Mesh:
            return instantiateMesh(asset, options);
        default:
            return makeError(ErrorCode::InvalidArgument,
                             "{} is {}; what is placed in a scene is a model, a prefab or a mesh", info->reference(),
                             withArticle(info->type));
    }
}

void EditorActions::recordPlaced(Entity instance) {
    const World& world = m_context.world();

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
}

Result<Uuid> EditorActions::createPrefab(Entity entity, std::string_view assetPath, bool overwrite) {
    CORDITE_TRY(requireValid(m_context, entity));
    if (m_context.isPlaying()) {
        return makeError(ErrorCode::InvalidState,
                         "the scene is being played: stop it before making a prefab, or it is made of a game "
                         "half-way through");
    }
    AssetManager& assets = m_context.assets();
    const std::filesystem::path& root = assets.database().rootDirectory();
    if (root.empty()) {
        return makeError(ErrorCode::InvalidState, "there is no project open to keep a prefab in");
    }

    const World& world = m_context.world();
    const std::string path = assetPath.empty() ? defaultPrefabPath(world.name(entity)) : std::string(assetPath);
    const std::filesystem::path file = root / pathFromUtf8(path);
    // Asks the file system, so that a path cannot leave by a link or by "..".
    CORDITE_TRY_ASSIGN(const std::string reference, relativePathWithin(root, file));
    if (assetTypeOfFile(reference) != AssetType::Prefab) {
        return makeError(ErrorCode::InvalidArgument, "the name of a prefab file ends in \".prefab.json\"; got '{}'",
                         reference);
    }
    std::error_code failed;
    if (!overwrite && std::filesystem::exists(file, failed)) {
        return makeError(ErrorCode::AlreadyExists, "'{}' exists already; it is replaced only when that is asked for",
                         reference);
    }

    PrefabData prefab{.name = world.name(entity),
                      .entities = serializePrefabEntities(world, m_context.components(), entity)};
    if (prefab.entities.size() > kMaxPrefabEntities) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is {} entities with what is below it; a prefab may have {} at most", prefab.name,
                         prefab.entities.size(), kMaxPrefabEntities);
    }
    // A prefab written over must still be one that can be read: not one that holds itself,
    // which it would if the entity holds an instance of it.
    if (const AssetInfo* replaced = assets.database().findByReference(reference);
        replaced != nullptr && replaced->type == AssetType::Prefab) {
        CORDITE_TRY(resolvePrefabEntities(m_context.components(), replaced->id, reference, prefab.entities,
                                          prefabReaderOf(assets)));
    }

    if (const std::filesystem::path directory = file.parent_path(); !directory.empty()) {
        CORDITE_TRY(ensureDirectory(directory));
    }
    CORDITE_TRY(writeJsonFile(file, prefabDocument(prefab)));
    CORDITE_TRY(assets.refresh());

    // Found by the file, not by its name: on a file system that does not tell upper from
    // lower case, the name the assets have for it need not be the one that was asked for.
    // Only prefabs are looked at, since each look asks the file system.
    const AssetDatabase& database = assets.database();
    const auto listed = std::ranges::find_if(database.assets(), [&](const AssetInfo& candidate) {
        std::error_code ignored;
        return candidate.type == AssetType::Prefab &&
               std::filesystem::equivalent(database.sourceFile(candidate), file, ignored);
    });
    if (listed == database.assets().end()) {
        return makeError(ErrorCode::InvalidState, "the prefab was written to '{}', but the assets do not list it",
                         reference);
    }
    const Uuid id = listed->id;

    // What it was made of is its first instance: marked with the prefab, and every entity of
    // it linked to itself in the file, which has the ids it has. One step of the history, and
    // no snapshot: the entities keep their handles. Marked so already, there is nothing to edit.
    const PrefabMark before = prefabMarkOf(m_context.world(), entity);
    PrefabMark after = before;
    after.instance = PrefabInstanceComponent{.prefab = id};
    for (auto& [linked, link] : after.links) {
        link = PrefabLinkComponent{.prefab = id, .source = linked};
    }
    if (after != before) {
        CORDITE_TRY(applyPrefabMark(m_context, after));
        EditorContext* context = &m_context;
        // Shared by undo and redo, which are copied about with the history.
        const auto was = std::make_shared<const PrefabMark>(before);
        const auto is = std::make_shared<const PrefabMark>(std::move(after));
        m_context.history().push({
            .label = fmt::format("Make '{}' an instance of its prefab", world.name(entity)),
            .undo = [context, was] { return applyPrefabMark(*context, *was); },
            .redo = [context, is] { return applyPrefabMark(*context, *is); },
        });
    }
    // What was placed from a prefab that was written over takes what it is now, and so does
    // what holds it inside.
    if (const Result<size_t> updated = updatePrefabInstances(); !updated) {
        return tl::unexpected(updated.error());
    }
    return id;
}

Result<size_t> EditorActions::updatePrefabInstances(std::optional<Uuid> prefab) {
    if (m_context.isPlaying()) {
        return makeError(ErrorCode::InvalidState, "the scene is being played: what is played is not the document");
    }
    // Every instance as it is, before any is brought up to date: one may be inside another.
    std::vector<SubtreeChange> found;
    for (const Entity root : linkedPrefabRoots(m_context.world(), prefab)) {
        found.push_back(subtreeBefore(m_context, root));
    }
    const std::vector<Entity> updated =
        syncPrefabInstances(m_context.world(), m_context.components(), m_context.assets(), prefab);
    std::vector<SubtreeChange> changes;
    for (const Entity root : updated) {
        const Uuid id = m_context.world().id(root);
        const auto change = std::ranges::find(found, id, &SubtreeChange::root);
        if (change != found.end()) {
            changes.push_back(std::move(*change));
        }
    }
    for (SubtreeChange& change : changes) {
        CORDITE_TRY(subtreeAfter(m_context, change));
    }
    if (changes.empty()) {
        return size_t{0};
    }
    const size_t count = changes.size();
    m_context.history().push(
        subtreeEdit(m_context, fmt::format("Bring {} prefab instance(s) up to date", count), std::move(changes)));
    return count;
}

Status EditorActions::revertPrefabOverrides(Entity entity, std::string_view entry) {
    CORDITE_TRY(requireValid(m_context, entity));
    if (m_context.isPlaying()) {
        return makeError(ErrorCode::InvalidState, "the scene is being played: stop it before reverting");
    }
    World& world = m_context.world();
    if (!world.has<PrefabLinkComponent>(entity)) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is no linked copy of an entity of a prefab",
                         world.name(entity));
    }
    const Entity root = prefabInstanceRootOf(world, entity);
    if (root.isNull()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is in no linked instance of a prefab", world.name(entity));
    }
    const auto* link = world.tryGet<PrefabLinkComponent>(entity);
    if (!entry.empty() && std::ranges::find(link->overrides, entry) == link->overrides.end()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' does not keep {} of its own", world.name(entity), entry);
    }
    // Loaded before anything changes: a prefab that is gone changes nothing.
    const Uuid prefab = world.tryGet<PrefabInstanceComponent>(root)->prefab;
    CORDITE_TRY_ASSIGN(const Json entities, currentPrefabEntities(m_context.assets(), m_context.components(), prefab));

    SubtreeChange change = subtreeBefore(m_context, root);
    if (!entry.empty()) {
        std::erase(world.tryGet<PrefabLinkComponent>(entity)->overrides, std::string(entry));
    } else if (entity == root) {
        std::vector<Entity> pending = {root};
        while (!pending.empty()) {
            const Entity reverted = pending.back();
            pending.pop_back();
            // What is linked to this prefab - not an instance placed in it, of whatever prefab.
            if (reverted != root && isPrefabInstanceRoot(world, reverted)) {
                continue;
            }
            if (auto* held = world.tryGet<PrefabLinkComponent>(reverted); held != nullptr && held->prefab == prefab) {
                held->overrides.clear();
            }
            for (const Entity child : world.children(reverted)) {
                pending.push_back(child);
            }
        }
    } else {
        world.tryGet<PrefabLinkComponent>(entity)->overrides.clear();
    }
    CORDITE_TRY(syncPrefabInstance(world, m_context.components(), root, entities));
    CORDITE_TRY(subtreeAfter(m_context, change));
    if (change.after == change.before) {
        return {};
    }
    std::vector<SubtreeChange> changes;
    changes.push_back(std::move(change));
    m_context.history().push(
        subtreeEdit(m_context,
                    entry.empty() ? fmt::format("Revert '{}' to its prefab", world.name(entity))
                                  : fmt::format("Revert {} of '{}' to its prefab", entry, world.name(entity)),
                    std::move(changes)));
    return {};
}

Result<Uuid> EditorActions::applyPrefabOverrides(Entity entity, std::string_view entry) {
    CORDITE_TRY(requireValid(m_context, entity));
    if (m_context.isPlaying()) {
        return makeError(ErrorCode::InvalidState,
                         "the scene is being played: stop it before applying, or the prefab is made of a game "
                         "half-way through");
    }
    World& world = m_context.world();
    const Entity root = prefabInstanceRootOf(world, entity);
    if (root.isNull()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is in no linked instance of a prefab", world.name(entity));
    }
    const Uuid prefab = world.tryGet<PrefabInstanceComponent>(root)->prefab;
    AssetManager& assets = m_context.assets();
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> data, assets.loadPrefab(prefab));
    const AssetInfo* asset = assets.database().find(prefab);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "the prefab {} is not among the assets", prefab.toString());
    }
    // Worked out from what the prefab is now, with the prefabs inside it up to date.
    CORDITE_TRY_ASSIGN(const Json current, currentPrefabEntities(assets, m_context.components(), prefab));
    const PrefabApply apply{.entity = entity, .entry = std::string(entry)};
    CORDITE_TRY_ASSIGN(Json entities, prefabEntitiesWithOverrides(world, m_context.components(), root, current, apply));
    if (entities.size() > kMaxPrefabEntities) {
        return makeError(ErrorCode::InvalidArgument, "the prefab would be {} entities; a prefab may have {} at most",
                         entities.size(), kMaxPrefabEntities);
    }
    // What is written must still be read: not a prefab that holds itself, which it would if
    // an instance of it was placed in the instance.
    CORDITE_TRY(
        resolvePrefabEntities(m_context.components(), prefab, asset->reference(), entities, prefabReaderOf(assets)));
    const std::filesystem::path file = assets.database().sourceFile(*asset);
    // Every instance, before anything changes: those of the prefab take what it is now, and so
    // do those of prefabs that hold it.
    std::vector<SubtreeChange> found;
    for (const Entity instance : linkedPrefabRoots(world)) {
        found.push_back(subtreeBefore(m_context, instance));
    }

    CORDITE_TRY(writeJsonFile(file, prefabDocument({.name = data->name, .entities = std::move(entities)})));
    CORDITE_TRY(assets.refresh());

    markPrefabApplied(world, root, apply);
    // What changed is told by the instances before and after; a prefab that cannot be read
    // now - it was just written - is said in the log, and leaves them as they are.
    std::ignore = syncPrefabInstances(world, m_context.components(), assets);
    std::vector<SubtreeChange> changes;
    for (SubtreeChange& change : found) {
        CORDITE_TRY(subtreeAfter(m_context, change));
        if (change.after != change.before) {
            changes.push_back(std::move(change));
        }
    }
    if (!changes.empty()) {
        m_context.history().push(
            subtreeEdit(m_context,
                        entry.empty() ? fmt::format("Apply '{}' to its prefab", world.name(entity))
                                      : fmt::format("Apply {} of '{}' to its prefab", entry, world.name(entity)),
                        std::move(changes)));
    }
    return prefab;
}

std::string defaultPrefabPath(std::string_view entityName) {
    std::string file;
    for (const char character : toLowerAscii(entityName)) {
        const bool plain = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (plain) {
            file.push_back(character);
        } else if (!file.empty() && file.back() != '-') {
            file.push_back('-');
        }
    }
    if (!file.empty() && file.back() == '-') {
        file.pop_back();
    }
    return fmt::format("prefabs/{}.prefab.json", file.empty() ? "prefab" : file);
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
    // The name of the root of an instance is its own anyway; that of a copy inside is kept.
    std::vector<OverrideChange> overrides;
    if (!world.has<PrefabInstanceComponent>(entity)) {
        noteOverrides(
            world, entity, [](std::vector<std::string>& entries) { addOverride(entries, "name"); }, overrides);
    }
    m_context.history().push(
        withOverrideChanges(m_context,
                            {
                                .label = fmt::format("Rename '{}' to '{}'", before, name),
                                .undo = [apply, before = std::move(before)] { return apply(before); },
                                .redo = [apply, after = std::string(name)] { return apply(after); },
                            },
                            std::move(overrides)));
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
    std::vector<OverrideChange> overrides;
    noteOverrides(world, entity, [](std::vector<std::string>& entries) { addOverride(entries, "enabled"); }, overrides);
    m_context.history().push(
        withOverrideChanges(m_context,
                            {
                                .label = fmt::format("{} '{}'", enabled ? "Enable" : "Disable", world.name(entity)),
                                .undo = [apply, enabled] { return apply(!enabled); },
                                .redo = [apply, enabled] { return apply(enabled); },
                            },
                            std::move(overrides)));
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
    // A copy keeps what is added to it - or no longer leaves out what the prefab has.
    std::vector<OverrideChange> overrides;
    noteOverrides(
        world, entity,
        [&](std::vector<std::string>& entries) {
            if (isPrefabMark(descriptor->type.name())) {
                return;
            }
            const std::string removed = "-" + descriptor->type.name();
            if (std::ranges::find(entries, removed) != entries.end()) {
                std::erase(entries, removed);
            } else {
                addOverride(entries, "+" + descriptor->type.name());
            }
        },
        overrides);
    m_context.history().push(
        withOverrideChanges(m_context,
                            {
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
                            },
                            std::move(overrides)));
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
    // A copy keeps leaving out what is removed from it - or no longer has what it added.
    std::vector<OverrideChange> overrides;
    noteOverrides(
        world, entity,
        [&](std::vector<std::string>& entries) {
            if (isPrefabMark(descriptor->type.name())) {
                return;
            }
            const std::string added = "+" + descriptor->type.name();
            if (std::ranges::find(entries, added) != entries.end()) {
                std::erase(entries, added);
            } else {
                addOverride(entries, "-" + descriptor->type.name());
            }
        },
        overrides);
    m_context.history().push(
        withOverrideChanges(m_context,
                            {
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
                            },
                            std::move(overrides)));
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
    const Json after = descriptor->type.toJson(instance);
    if (after == before) {
        // Setting properties to the values they have is not an edit.
        return {};
    }
    std::vector<OverrideChange> overrides;
    noteChangedProperties(world, entity, descriptor->type.name(), before, after, overrides);
    pushComponentEdit(m_context, entity, *descriptor, std::move(restore), std::move(edit), std::move(overrides));
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
    std::vector<OverrideChange> overrides;
    noteChangedProperties(world, entity, descriptor->type.name(), before, after, overrides);
    pushComponentEdit(m_context, entity, *descriptor, std::move(restore), std::move(redo), std::move(overrides));
    return {};
}

Status EditorActions::recordComponentEdits(std::span<const Entity> entities, std::string_view component,
                                           std::span<const Json> before) {
    if (entities.size() != before.size()) {
        return makeError(ErrorCode::InvalidArgument, "{} entities were given with {} components as they were",
                         entities.size(), before.size());
    }
    CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, findComponent(m_context, component));
    World& world = m_context.world();
    std::vector<EntityPatches> changed;
    Entity last;
    std::vector<OverrideChange> overrides;
    for (size_t index = 0; index < entities.size(); ++index) {
        const Entity entity = entities[index];
        CORDITE_TRY(requireValid(m_context, entity));
        const void* instance = descriptor->getConst(world.registry(), entity.handle);
        if (instance == nullptr) {
            return makeError(ErrorCode::NotFound, "'{}' has no {} component", world.name(entity), component);
        }
        const Json after = descriptor->type.toJson(instance);
        if (after == before[index]) {
            continue;
        }
        CORDITE_TRY_ASSIGN(PropertyPatch restore, descriptor->type.parse(before[index]));
        CORDITE_TRY_ASSIGN(PropertyPatch redo, descriptor->type.parse(after));
        noteChangedProperties(world, entity, descriptor->type.name(), before[index], after, overrides);
        changed.push_back({world.id(entity), std::move(restore), std::move(redo)});
        last = entity;
    }
    if (changed.size() == 1) {
        // Named by the entity, as an edit of one is.
        pushComponentEdit(m_context, last, *descriptor, std::move(changed.front().before),
                          std::move(changed.front().after), std::move(overrides));
    } else if (!changed.empty()) {
        pushComponentEdits(m_context, *descriptor, std::move(changed), std::move(overrides));
    }
    return {};
}

} // namespace cordite
