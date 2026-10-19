#include "cordite/editor/EditorActions.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Model.h"
#include "cordite/assets/Prefab.h"
#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/StringUtils.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/EntityPath.h"
#include "cordite/world/SceneSerializer.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace cordite {
namespace {

/// The name of the component that marks an instance of a prefab.
constexpr std::string_view kPrefabInstance = "PrefabInstance";

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

/// What one entity's component was and is, for an edit of several.
struct EntityPatches {
    Uuid id;
    PropertyPatch before;
    PropertyPatch after;
};

void pushComponentEdits(EditorContext& context, const ComponentDescriptor& descriptor,
                        std::vector<EntityPatches> patches) {
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
    context.history().push({
        .label = fmt::format("Edit {} of {} entities", descriptor.type.name(), shared->size()),
        .undo = [apply] { return apply(true); },
        .redo = [apply] { return apply(false); },
    });
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
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> data, m_context.assets().loadPrefab(prefab));

    const Result<Entity> instance =
        cordite::instantiatePrefab(m_context.world(), m_context.components(), data->entities, prefab, options);
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

    Json mark = Json::object();
    mark["prefab"] = id.toString();
    CORDITE_TRY(world.has<PrefabInstanceComponent>(entity) ? setComponentProperties(entity, kPrefabInstance, mark)
                                                           : addComponent(entity, kPrefabInstance, mark));
    return id;
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
        changed.push_back({world.id(entity), std::move(restore), std::move(redo)});
        last = entity;
    }
    if (changed.size() == 1) {
        // Named by the entity, as an edit of one is.
        pushComponentEdit(m_context, last, *descriptor, std::move(changed.front().before),
                          std::move(changed.front().after));
    } else if (!changed.empty()) {
        pushComponentEdits(m_context, *descriptor, std::move(changed));
    }
    return {};
}

} // namespace cordite
