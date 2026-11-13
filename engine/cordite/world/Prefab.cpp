#include "cordite/world/Prefab.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Whether `link` lists `entry` among the overrides of its entity.
bool overrides(const PrefabLinkComponent* link, std::string_view entry) {
    return link != nullptr && std::ranges::find(link->overrides, entry) != link->overrides.end();
}

/// The id of an entity of a prefab, which its file says as text.
Result<Uuid> parseId(std::string_view text) {
    const std::optional<Uuid> id = Uuid::parse(text);
    if (!id) {
        return makeError(ErrorCode::ParseError, "'{}' is no id of an entity", text);
    }
    return *id;
}

/// An entity of a prefab, checked, as syncPrefabInstance() uses it.
struct PrefabEntity {
    Uuid source;
    /// The source of its parent; nil for the root.
    Uuid parent;
    std::string name;
    bool enabled = true;
    /// The components of the entity, each with what its file says.
    std::vector<std::pair<const ComponentDescriptor*, const Json*>> components;
};

/// Checks the entities of a prefab - one root, listed first, every parent before what
/// hangs under it; components that exist and whose values are right - before anything
/// is changed.
Result<std::vector<PrefabEntity>> readPrefabEntities(const ComponentRegistry& components, const Json& entities) {
    if (!entities.is_array() || entities.empty()) {
        return makeError(ErrorCode::ParseError, "the entities of a prefab must be a list of at least one");
    }
    std::vector<PrefabEntity> read;
    std::unordered_set<Uuid> sources;
    for (const Json& entity : entities) {
        if (!entity.is_object()) {
            return makeError(ErrorCode::ParseError, "an entity of a prefab must be an object");
        }
        PrefabEntity plan;
        CORDITE_TRY_ASSIGN(const std::string id, jsonGet<std::string>(entity, "id"));
        CORDITE_TRY_ASSIGN(plan.source, parseId(id));
        CORDITE_TRY_ASSIGN(const std::optional<std::string> parent, jsonGetOptional<std::string>(entity, "parent"));
        if (parent) {
            CORDITE_TRY_ASSIGN(plan.parent, parseId(*parent));
            if (!sources.contains(plan.parent)) {
                return makeError(ErrorCode::ParseError, "the entity {} of the prefab comes before its parent {}", id,
                                 *parent);
            }
        } else if (!read.empty()) {
            return makeError(ErrorCode::ParseError, "a prefab has one root, and {} is another", id);
        }
        CORDITE_TRY_ASSIGN(const std::optional<std::string> name, jsonGetOptional<std::string>(entity, "name"));
        plan.name = name.value_or("Entity");
        CORDITE_TRY_ASSIGN(const std::optional<bool> enabled, jsonGetOptional<bool>(entity, "enabled"));
        plan.enabled = enabled.value_or(true);
        if (const auto held = entity.find("components"); held != entity.end() && held->is_object()) {
            for (const auto& [componentName, value] : held->items()) {
                const ComponentDescriptor* descriptor = components.find(componentName);
                if (descriptor == nullptr) {
                    return makeError(ErrorCode::ParseError,
                                     "the entity {} of the prefab has a component {}, which is none", id,
                                     componentName);
                }
                if (const Result<PropertyPatch> checked = descriptor->type.parse(value); !checked) {
                    return tl::unexpected(
                        checked.error().withContext(fmt::format("{} of the entity {}", componentName, id)));
                }
                // A link in a prefab's file is the link its entity had where the prefab was
                // made - serializePrefabEntities() leaves none - and says nothing of the links
                // of an instance, which are the instance's own.
                if (componentName == PrefabLinkComponent::kName) {
                    continue;
                }
                plan.components.emplace_back(descriptor, &value);
            }
        }
        sources.insert(plan.source);
        read.push_back(std::move(plan));
    }
    if (!read.front().parent.isNil()) {
        return makeError(ErrorCode::ParseError, "the first entity of a prefab is its root, and hangs under nothing");
    }
    return read;
}

/// The entities of the instance under `root` that are linked to its prefab, by their
/// source: the root, and what hangs under it - but not under another instance inside it.
std::unordered_map<Uuid, Entity> linkedEntities(const World& world, Entity root) {
    std::unordered_map<Uuid, Entity> linked;
    std::vector<Entity> pending = {root};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        if (const auto* link = world.tryGet<PrefabLinkComponent>(entity)) {
            linked.emplace(link->source, entity);
        }
        if (entity != root && world.has<PrefabInstanceComponent>(entity)) {
            continue;
        }
        for (const Entity child : world.children(entity)) {
            pending.push_back(child);
        }
    }
    return linked;
}

/// The entity of the prefab whose source is `source`; null when it has none.
const PrefabEntity* findPrefabEntity(const std::vector<PrefabEntity>& prefab, const Uuid& source) {
    const auto found = std::ranges::find(prefab, source, &PrefabEntity::source);
    return found == prefab.end() ? nullptr : &*found;
}

/// Takes away the entities of an instance that the prefab no longer has, with what hangs
/// under them, and forgets them - and what hung under them - in `linked`.
void destroyWhatThePrefabLost(World& world, const std::vector<PrefabEntity>& prefab,
                              std::unordered_map<Uuid, Entity>& linked, PrefabSync& sync) {
    for (const auto& [source, entity] : linked) {
        if (findPrefabEntity(prefab, source) == nullptr && world.isValid(entity)) {
            world.destroyEntity(entity);
            ++sync.entitiesDestroyed;
        }
    }
    std::erase_if(linked, [&world](const auto& entry) { return !world.isValid(entry.second); });
}

/// Makes the entities the prefab gained, parents before what hangs under them, linked and
/// without components yet - so that every entity of the prefab has one in the instance that
/// a reference can be led to. Returns the sources of those that were made.
Result<std::unordered_set<Uuid>> createWhatThePrefabGained(World& world, const std::vector<PrefabEntity>& prefab,
                                                           std::unordered_map<Uuid, Entity>& linked, PrefabSync& sync) {
    std::unordered_set<Uuid> made;
    for (const PrefabEntity& entity : prefab) {
        const auto parent = linked.find(entity.parent);
        if (linked.contains(entity.source) || parent == linked.end()) {
            continue;
        }
        const Entity created = world.createEntity(entity.name);
        world.setEnabled(created, entity.enabled);
        CORDITE_TRY(world.setParent(created, parent->second, false));
        world.add<PrefabLinkComponent>(created).source = entity.source;
        linked.emplace(entity.source, created);
        made.insert(entity.source);
        ++sync.entitiesCreated;
    }
    return made;
}

/// What brings the entities of one instance up to date: the components there are, and where
/// a reference to an entity of the prefab leads in the instance.
struct InstanceUpdate {
    World& world;
    const ComponentRegistry& components;
    /// From the id of an entity of the prefab to that of its copy in the instance.
    std::unordered_map<Uuid, Uuid> copies;
    PrefabSync& sync;

    /// A value of a component of the prefab, its references led to the copies.
    [[nodiscard]] Json valueOf(const ComponentDescriptor& descriptor, const Json& value) const {
        Json remapped = value;
        remapEntityReferences(remapped, descriptor.type, copies);
        return remapped;
    }

    /// Gives an entity that was just made the components of the prefab's.
    void fill(Entity instance, const PrefabEntity& entity) const {
        for (const auto& [descriptor, value] : entity.components) {
            // Checked by readPrefabEntities: it parses, and an id led to another is an id.
            descriptor->emplace(world.registry(), instance.handle,
                                descriptor->type.parse(valueOf(*descriptor, *value)).value());
        }
    }

    /// Gives one component of the instance the prefab's values of what it does not override.
    void updateComponent(Entity instance, bool isRoot, const PrefabLinkComponent& link,
                         const ComponentDescriptor& descriptor, const Json& value) const {
        const std::string& name = descriptor.type.name();
        if (isRoot && name == "Transform") {
            return;
        }
        const Json prefabValue = valueOf(descriptor, value);
        void* component = descriptor.get(world.registry(), instance.handle);
        if (component == nullptr) {
            if (!overrides(&link, fmt::format("-{}", name))) {
                descriptor.emplace(world.registry(), instance.handle, descriptor.type.parse(prefabValue).value());
                ++sync.componentsAdded;
            }
            return;
        }
        const Json current = descriptor.type.toJson(component);
        Json taken = Json::object();
        for (const auto& [property, wanted] : prefabValue.items()) {
            if (overrides(&link, fmt::format("{}.{}", name, property))) {
                continue;
            }
            const auto now = current.find(property);
            if (now == current.end() || *now != wanted) {
                taken[property] = wanted;
            }
        }
        if (!taken.empty()) {
            descriptor.type.parse(taken).value().applyTo(component);
            sync.valuesChanged += taken.size();
        }
    }

    /// Takes away the components the instance has and the prefab's entity does not, but for
    /// those it added itself and the marks of prefabs.
    void removeComponents(Entity instance, const PrefabLinkComponent& link, const PrefabEntity& entity) const {
        for (const ComponentDescriptor* descriptor : components.components()) {
            const std::string& name = descriptor->type.name();
            if (name == PrefabLinkComponent::kName || name == PrefabInstanceComponent::kName ||
                !descriptor->traits.optional || !descriptor->has(world.registry(), instance.handle)) {
                continue;
            }
            const bool inPrefab = std::ranges::any_of(
                entity.components, [descriptor](const auto& held) { return held.first == descriptor; });
            if (!inPrefab && !overrides(&link, fmt::format("+{}", name))) {
                descriptor->remove(world.registry(), instance.handle);
                ++sync.componentsRemoved;
            }
        }
    }

    /// Brings an entity of the instance up to date with the prefab's.
    void update(Entity instance, bool isRoot, const PrefabEntity& entity) const {
        // Copied: a component that is added or removed may move the one the world holds.
        const PrefabLinkComponent* held = world.tryGet<PrefabLinkComponent>(instance);
        const PrefabLinkComponent link = held != nullptr ? *held : PrefabLinkComponent{};
        if (!isRoot && !overrides(&link, "name") && world.name(instance) != entity.name) {
            world.setName(instance, entity.name);
            ++sync.valuesChanged;
        }
        if (!overrides(&link, "enabled") && world.isEnabled(instance) != entity.enabled) {
            world.setEnabled(instance, entity.enabled);
            ++sync.valuesChanged;
        }
        for (const auto& [descriptor, value] : entity.components) {
            updateComponent(instance, isRoot, link, *descriptor, *value);
        }
        removeComponents(instance, link, entity);
    }
};

} // namespace

Result<PrefabSync> syncPrefabInstance(World& world, const ComponentRegistry& components, Entity root,
                                      const Json& entities) {
    if (!world.isValid(root) || !world.has<PrefabInstanceComponent>(root)) {
        return makeError(ErrorCode::InvalidArgument, "only the root of a prefab instance is brought up to date");
    }
    CORDITE_TRY_ASSIGN(const std::vector<PrefabEntity> prefab, readPrefabEntities(components, entities));
    PrefabSync sync;
    // An instance placed before prefabs were linked is a copy, linked to nothing.
    const auto* rootLink = world.tryGet<PrefabLinkComponent>(root);
    if (rootLink == nullptr || rootLink->source != prefab.front().source) {
        return sync;
    }

    std::unordered_map<Uuid, Entity> linked = linkedEntities(world, root);
    destroyWhatThePrefabLost(world, prefab, linked, sync);
    CORDITE_TRY_ASSIGN(const std::unordered_set<Uuid> made, createWhatThePrefabGained(world, prefab, linked, sync));

    InstanceUpdate update{.world = world, .components = components, .sync = sync};
    for (const auto& [source, entity] : linked) {
        update.copies.emplace(source, world.id(entity));
    }
    for (const PrefabEntity& entity : prefab) {
        const auto found = linked.find(entity.source);
        if (found == linked.end()) {
            continue;
        }
        if (made.contains(entity.source)) {
            update.fill(found->second, entity);
        } else {
            update.update(found->second, found->second == root, entity);
        }
    }
    return sync;
}

Result<Entity> instantiatePrefab(World& world, const ComponentRegistry& components, const Json& entities,
                                 const Uuid& prefab, const PrefabInstanceOptions& options) {
    if (options.position && !isFinite(*options.position)) {
        return makeError(ErrorCode::InvalidArgument, "the position of a prefab instance must be finite");
    }
    if (options.rotation && !isRotation(*options.rotation)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the rotation of a prefab instance must be finite and must not be zero");
    }

    // What reads scenes checks everything about the entities - and that they have one
    // root - before it creates the first of them.
    CORDITE_TRY_ASSIGN(
        const std::vector<Entity> created,
        instantiateEntities(world, entities, components,
                            {.parent = options.parent, .generateNewIds = true, .requireSingleRoot = true}));
    CORDITE_ASSERT(!created.empty(), "a group of entities with one root has an entity");
    if (created.empty()) {
        return makeError(ErrorCode::InvalidState, "placing a prefab created no entity");
    }

    // A parent is listed before its children, so the one entity that hangs under none of
    // the others is the first.
    const Entity root = created.front();
    if (!options.name.empty()) {
        world.setName(root, options.name);
    }
    if (options.position) {
        world.transform(root).position = *options.position;
    }
    if (options.rotation) {
        // A rotation that is one already is kept as it is: placing must not change it.
        world.transform(root).rotation = normalizeRotation(*options.rotation);
    }
    // add() gives the mark the root has already, when the prefab was written with one.
    world.add<PrefabInstanceComponent>(root).prefab = prefab;
    // Every copy is linked to the entity of the prefab it was made of: instantiateEntities
    // creates them in the order of the list.
    for (size_t index = 0; index < created.size() && index < entities.size(); ++index) {
        if (const std::optional<Uuid> source = Uuid::parse(jsonGetOr<std::string>(entities[index], "id", ""))) {
            world.add<PrefabLinkComponent>(created[index]) = PrefabLinkComponent{.source = *source};
        }
    }
    return root;
}

Json serializePrefabEntities(const World& world, const ComponentRegistry& components, Entity root) {
    CORDITE_ASSERT(world.isValid(root), "a prefab is made of an entity that exists");
    if (!world.isValid(root)) {
        return Json::array();
    }
    const std::array<Entity, 1> roots = {root};
    Json entities = serializeEntities(world, roots, components);
    // An entity that is valid is written, and the root comes first. In the prefab it
    // hangs under nothing, and is no instance; and no entity of a prefab is a copy of
    // another's, whatever it was where it was made.
    Json& first = entities.front();
    first["parent"] = nullptr;
    if (const auto held = first.find("components"); held != first.end() && held->is_object()) {
        held->erase(std::string(PrefabInstanceComponent::kName));
    }
    for (Json& entity : entities) {
        if (const auto held = entity.find("components"); held != entity.end() && held->is_object()) {
            held->erase(std::string(PrefabLinkComponent::kName));
        }
    }
    return entities;
}

} // namespace cordite
