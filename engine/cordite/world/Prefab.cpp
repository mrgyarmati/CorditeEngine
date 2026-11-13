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

/// The prefab of the instance whose root is `root`; nil for an entity that is none.
Uuid prefabOf(const World& world, Entity root) {
    const auto* instance = world.tryGet<PrefabInstanceComponent>(root);
    return instance != nullptr ? instance->prefab : Uuid{};
}

/// Whether the link of `entity` is one a prefab made of what hangs under `top` holds: that of
/// an entity of a prefab placed inside it - one whose root is `entity` or above it, below
/// `top`. What is linked to anything else is the prefab's own entity, and has no link in it.
bool keepsLink(const World& world, Entity top, Entity entity) {
    const auto* link = world.tryGet<PrefabLinkComponent>(entity);
    if (link == nullptr) {
        return false;
    }
    for (Entity above = entity; !above.isNull() && above != top; above = world.parent(above)) {
        const auto* instance = world.tryGet<PrefabInstanceComponent>(above);
        if (instance != nullptr && instance->prefab == link->prefab) {
            return true;
        }
    }
    return false;
}

/// Takes the links out of `entities` - serialized from what hangs under `top` - that a prefab
/// does not hold (keepsLink).
void dropOwnLinks(const World& world, Entity top, Json& entities) {
    for (Json& entity : entities) {
        const auto held = entity.find("components");
        if (held == entity.end() || !held->is_object()) {
            continue;
        }
        const std::optional<Uuid> id = Uuid::parse(jsonGetOr<std::string>(entity, "id", ""));
        if (!id || !keepsLink(world, top, world.findById(*id))) {
            held->erase(std::string(PrefabLinkComponent::kName));
        }
    }
}

/// Notes `entry` among the overrides of a link that an entity of a prefab's file has - one
/// of a prefab placed inside it, whose value the file now holds as its own. A component added
/// and one left out cancel each other.
void noteInFileLink(Json& target, const std::string& entry) {
    const auto held = target.find("components");
    if (held == target.end() || !held->is_object() || !held->contains(PrefabLinkComponent::kName)) {
        return;
    }
    Json& overrides = (*held)[std::string(PrefabLinkComponent::kName)]["overrides"];
    if (!overrides.is_array()) {
        overrides = Json::array();
    }
    if (entry.starts_with('+') || entry.starts_with('-')) {
        const std::string opposite = (entry.starts_with('+') ? "-" : "+") + entry.substr(1);
        const auto cancelled = std::ranges::find(overrides, Json(opposite));
        if (cancelled != overrides.end()) {
            overrides.erase(cancelled);
            return;
        }
    }
    if (std::ranges::find(overrides, Json(entry)) == overrides.end()) {
        overrides.push_back(entry);
    }
}

/// The entities of the instance under `root`, by their source: the root and whatever hangs
/// under it that is linked to its prefab - the entities of a prefab inside it included,
/// which are its prefab's too, and not those of an instance of another prefab placed in it.
std::unordered_map<Uuid, Entity> linkedEntities(const World& world, Entity root) {
    const Uuid prefab = prefabOf(world, root);
    std::unordered_map<Uuid, Entity> linked;
    std::vector<Entity> pending = {root};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        // An instance placed inside is no part of this one, of whichever prefab it is.
        if (entity != root && isPrefabInstanceRoot(world, entity)) {
            continue;
        }
        if (const auto* link = world.tryGet<PrefabLinkComponent>(entity); link != nullptr && link->prefab == prefab) {
            linked.emplace(link->source, entity);
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
///
/// The id of what is made follows from the root of the instance and the source, so that an
/// instance brought up to date twice the same way - a prefab inside a prefab, which is
/// brought up to date whenever the outer one is read - gets the same ids both times.
Result<std::unordered_set<Uuid>> createWhatThePrefabGained(World& world, Entity root,
                                                           const std::vector<PrefabEntity>& prefab,
                                                           std::unordered_map<Uuid, Entity>& linked, PrefabSync& sync) {
    const Uuid prefabId = prefabOf(world, root);
    const Uuid rootId = world.id(root);
    std::unordered_set<Uuid> made;
    for (const PrefabEntity& entity : prefab) {
        const auto parent = linked.find(entity.parent);
        if (linked.contains(entity.source) || parent == linked.end()) {
            continue;
        }
        // Taken when a copy of the instance was made with the ids it has - pasted into the
        // world it was copied from: then any id will do.
        const Result<Entity> derived =
            world.createEntityWithId(Uuid::fromName(rootId, entity.source.toString()), entity.name);
        const Entity created = derived ? *derived : world.createEntity(entity.name);
        world.setEnabled(created, entity.enabled);
        CORDITE_TRY(world.setParent(created, parent->second, false));
        world.add<PrefabLinkComponent>(created) = PrefabLinkComponent{.prefab = prefabId, .source = entity.source};
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
    // An instance placed before prefabs were linked is a copy, linked to nothing; one inside
    // an instance of another prefab is part of that, and comes up to date with it.
    const auto* rootLink = world.tryGet<PrefabLinkComponent>(root);
    if (rootLink == nullptr || rootLink->prefab != prefabOf(world, root) || rootLink->source != prefab.front().source) {
        return sync;
    }

    std::unordered_map<Uuid, Entity> linked = linkedEntities(world, root);
    destroyWhatThePrefabLost(world, prefab, linked, sync);
    CORDITE_TRY_ASSIGN(const std::unordered_set<Uuid> made,
                       createWhatThePrefabGained(world, root, prefab, linked, sync));

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

namespace {

/// What of an instance is applied: linked entities, each with the overrides of it that are,
/// and the entities added to the instance whose groups are written into the prefab whole.
struct ApplyScope {
    std::vector<std::pair<Entity, std::vector<std::string>>> entries;
    std::vector<Entity> added;
};

Result<ApplyScope> scopeOf(const World& world, Entity root, const PrefabApply& apply) {
    const auto* link = world.tryGet<PrefabLinkComponent>(apply.entity);
    if (link == nullptr || prefabInstanceRootOf(world, apply.entity) != root) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is no linked entity of the instance '{}'",
                         world.isValid(apply.entity) ? world.name(apply.entity) : std::string("an entity"),
                         world.name(root));
    }
    ApplyScope scope;
    if (!apply.entry.empty()) {
        if (std::ranges::find(link->overrides, apply.entry) == link->overrides.end()) {
            return makeError(ErrorCode::InvalidArgument, "'{}' does not keep {} of its own", world.name(apply.entity),
                             apply.entry);
        }
        scope.entries.emplace_back(apply.entity, std::vector<std::string>{apply.entry});
        return scope;
    }
    if (apply.entity != root) {
        scope.entries.emplace_back(apply.entity, link->overrides);
        return scope;
    }
    std::vector<Entity> pending = {root};
    while (!pending.empty()) {
        const Entity entity = pending.back();
        pending.pop_back();
        const auto* held = world.tryGet<PrefabLinkComponent>(entity);
        if (held == nullptr || held->prefab != link->prefab ||
            (entity != root && isPrefabInstanceRoot(world, entity))) {
            // Added - an entity of its own, or an instance of a prefab placed in it, which the
            // prefab then holds with its link.
            scope.added.push_back(entity);
            continue;
        }
        scope.entries.emplace_back(entity, held->overrides);
        for (const Entity child : world.children(entity)) {
            pending.push_back(child);
        }
    }
    // In the order of the hierarchy, so that the prefab's file comes out the same every time.
    std::vector<Entity> order;
    world.traverse([&](Entity entity, uint32_t /*depth*/) { order.push_back(entity); });
    const auto place = [&order](Entity entity) { return std::ranges::find(order, entity) - order.begin(); };
    std::ranges::sort(scope.added, {}, place);
    return scope;
}

/// The JSON of a component of `entity`, with its references to entities of the instance led
/// to the entities of the prefab they are copies of.
Json componentOf(const World& world, const ComponentDescriptor& descriptor, Entity entity,
                 const std::unordered_map<Uuid, Uuid>& sources) {
    Json value = descriptor.type.toJson(descriptor.getConst(world.registry(), entity.handle));
    remapEntityReferences(value, descriptor.type, sources);
    return value;
}

/// Writes one override of `entity` into `target`, its entity in the prefab's file.
Status applyEntry(const World& world, const ComponentRegistry& components, Entity entity, bool isRoot,
                  const std::string& entry, const std::unordered_map<Uuid, Uuid>& sources, Json& target) {
    if (entry == "name") {
        // The root's name is the instance's own.
        if (!isRoot) {
            target["name"] = world.name(entity);
        }
        return {};
    }
    if (entry == "enabled") {
        target["enabled"] = world.isEnabled(entity);
        return {};
    }
    if (!target.contains("components") || !target["components"].is_object()) {
        target["components"] = Json::object();
    }
    Json& held = target["components"];
    const bool removed = entry.starts_with('-');
    const bool added = entry.starts_with('+');
    const size_t dot = entry.find('.');
    const std::string name =
        removed || added ? entry.substr(1) : entry.substr(0, dot == std::string::npos ? entry.size() : dot);
    const ComponentDescriptor* descriptor = components.find(name);
    if (descriptor == nullptr || (!removed && !added && dot == std::string::npos)) {
        return makeError(ErrorCode::InvalidArgument, "'{}' keeps {} of its own, which names no component",
                         world.name(entity), entry);
    }
    if (removed) {
        held.erase(name);
        return {};
    }
    if (isRoot && name == "Transform") {
        return {};
    }
    if (!descriptor->has(world.registry(), entity.handle)) {
        // Nothing of it to write: it was left out as well.
        return {};
    }
    Json value = componentOf(world, *descriptor, entity, sources);
    if (added || !held.contains(name) || !held[name].is_object()) {
        // A property of a component the prefab does not have brings the component with it.
        held[name] = std::move(value);
        return {};
    }
    const std::string property = entry.substr(dot + 1);
    if (const auto found = value.find(property); found != value.end()) {
        held[name][property] = *found;
    }
    return {};
}

} // namespace

Result<Json> prefabEntitiesWithOverrides(const World& world, const ComponentRegistry& components, Entity root,
                                         const Json& entities, const PrefabApply& apply) {
    if (!world.isValid(root) || !world.has<PrefabInstanceComponent>(root)) {
        return makeError(ErrorCode::InvalidArgument, "only what is in an instance of a prefab is applied to it");
    }
    CORDITE_TRY_ASSIGN(const std::vector<PrefabEntity> prefab, readPrefabEntities(components, entities));
    const auto* rootLink = world.tryGet<PrefabLinkComponent>(root);
    if (rootLink == nullptr || rootLink->prefab != prefabOf(world, root) || rootLink->source != prefab.front().source) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is no linked instance of this prefab: it was placed from what the prefab was before "
                         "it was made anew, or before prefabs were linked",
                         world.name(root));
    }
    CORDITE_TRY_ASSIGN(const ApplyScope scope, scopeOf(world, root, apply));

    std::unordered_map<Uuid, Uuid> sources;
    for (const auto& [source, entity] : linkedEntities(world, root)) {
        sources.emplace(world.id(entity), source);
    }
    std::unordered_map<Uuid, size_t> places;
    for (size_t index = 0; index < prefab.size(); ++index) {
        places.emplace(prefab[index].source, index);
    }

    Json result = entities;
    for (const auto& [entity, entries] : scope.entries) {
        const auto place = places.find(world.tryGet<PrefabLinkComponent>(entity)->source);
        if (place == places.end()) {
            // Gone from the prefab: bringing the instance up to date takes it away.
            continue;
        }
        for (const std::string& entry : entries) {
            CORDITE_TRY(applyEntry(world, components, entity, entity == root, entry, sources, result[place->second]));
            // What the prefab holds of a prefab inside it is that one's own now.
            noteInFileLink(result[place->second], entry);
        }
    }
    for (const Entity group : scope.added) {
        const std::array<Entity, 1> roots = {group};
        Json added = serializeEntities(world, roots, components);
        added.front()["parent"] = sources.at(world.id(world.parent(group))).toString();
        // A prefab placed in the instance stays linked to its own in the file.
        dropOwnLinks(world, world.parent(group), added);
        for (Json& entity : added) {
            // The value of an item of a JSON object that is not const can be changed through a
            // const view of the item.
            for (const auto& [name, value] : entity["components"].items()) {
                if (const ComponentDescriptor* descriptor = components.find(name)) {
                    remapEntityReferences(value, descriptor->type, sources);
                }
            }
            result.push_back(std::move(entity));
        }
    }
    // What is written must be a prefab, as bringing an instance up to date reads one.
    CORDITE_TRY(readPrefabEntities(components, result));
    return result;
}

void markPrefabApplied(World& world, Entity root, const PrefabApply& apply) {
    auto* link = world.tryGet<PrefabLinkComponent>(apply.entity);
    CORDITE_ASSERT(link != nullptr, "only what was applied is marked so");
    if (link == nullptr) {
        return;
    }
    if (!apply.entry.empty()) {
        std::erase(link->overrides, apply.entry);
        return;
    }
    if (apply.entity != root) {
        link->overrides.clear();
        return;
    }
    const Uuid prefab = link->prefab;
    // Each entity with whether it is of what was added - where everything is in the prefab
    // with the id it has, and this prefab's now, a prefab placed in it included.
    std::vector<std::pair<Entity, bool>> pending = {{root, false}};
    while (!pending.empty()) {
        const auto [entity, inAdded] = pending.back();
        pending.pop_back();
        auto* held = world.tryGet<PrefabLinkComponent>(entity);
        const bool added = inAdded || held == nullptr || held->prefab != prefab ||
                           (entity != root && isPrefabInstanceRoot(world, entity));
        if (added) {
            world.add<PrefabLinkComponent>(entity) = PrefabLinkComponent{.prefab = prefab, .source = world.id(entity)};
        } else {
            held->overrides.clear();
        }
        for (const Entity child : world.children(entity)) {
            pending.emplace_back(child, added);
        }
    }
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
            world.add<PrefabLinkComponent>(created[index]) = PrefabLinkComponent{.prefab = prefab, .source = *source};
        }
    }
    return root;
}

namespace {

/// A prefab on the way from the one that is resolved to one inside it.
struct Holder {
    Uuid prefab;
    std::string name;
};

/// "'a' holds 'b', which holds 'a'": the chain of prefabs, closed by `again`.
std::string describeChain(const std::vector<Holder>& chain, const Holder& again) {
    std::string text = fmt::format("'{}'", chain.front().name);
    for (size_t index = 1; index < chain.size(); ++index) {
        text += fmt::format(" holds '{}', which", chain[index].name);
    }
    text += fmt::format(" holds '{}'", again.name);
    return text;
}

Result<Json> resolve(const ComponentRegistry& components, const Json& entities, const PrefabReader& read,
                     std::vector<Holder>& chain) {
    World scratch;
    CORDITE_TRY_ASSIGN(const std::vector<Entity> created,
                       instantiateEntities(scratch, entities, components, {.requireSingleRoot = true}));
    const std::vector<Entity> inside = linkedPrefabRoots(scratch);
    if (inside.empty()) {
        return entities;
    }
    for (const Entity instance : inside) {
        // One brought up to date before may have taken this one away.
        if (!scratch.isValid(instance)) {
            continue;
        }
        const Uuid prefab = scratch.tryGet<PrefabInstanceComponent>(instance)->prefab;
        if (const auto again = std::ranges::find(chain, prefab, &Holder::prefab); again != chain.end()) {
            return makeError(ErrorCode::InvalidArgument, "{}: a prefab cannot hold itself",
                             describeChain(chain, *again));
        }
        if (chain.size() >= kMaxPrefabNesting) {
            return makeError(ErrorCode::InvalidArgument, "'{}' holds prefabs inside prefabs more than {} deep",
                             chain.front().name, kMaxPrefabNesting);
        }
        Json held;
        CORDITE_TRY_ASSIGN(std::string name, read(prefab, held));
        chain.push_back({.prefab = prefab, .name = std::move(name)});
        Result<Json> resolved = resolve(components, held, read, chain);
        chain.pop_back();
        if (!resolved) {
            return tl::unexpected(std::move(resolved).error());
        }
        CORDITE_TRY(syncPrefabInstance(scratch, components, instance, *resolved));
    }
    const std::array<Entity, 1> roots = {created.front()};
    return serializeEntities(scratch, roots, components);
}

} // namespace

Result<Json> resolvePrefabEntities(const ComponentRegistry& components, const Uuid& prefab, std::string_view name,
                                   const Json& entities, const PrefabReader& read) {
    std::vector<Holder> chain = {{.prefab = prefab, .name = std::string(name)}};
    return resolve(components, entities, read, chain);
}

Json serializePrefabEntities(const World& world, const ComponentRegistry& components, Entity root) {
    CORDITE_ASSERT(world.isValid(root), "a prefab is made of an entity that exists");
    if (!world.isValid(root)) {
        return Json::array();
    }
    const std::array<Entity, 1> roots = {root};
    Json entities = serializeEntities(world, roots, components);
    // An entity that is valid is written, and the root comes first. In the prefab it
    // hangs under nothing, and is no instance; and the prefab's own entities are copies of
    // nothing, whatever they were where it was made - but those of a prefab placed inside
    // it keep their links to that one.
    Json& first = entities.front();
    first["parent"] = nullptr;
    if (const auto held = first.find("components"); held != first.end() && held->is_object()) {
        held->erase(std::string(PrefabInstanceComponent::kName));
    }
    dropOwnLinks(world, root, entities);
    return entities;
}

Entity prefabInstanceRootOf(const World& world, Entity entity) {
    const auto* link = world.tryGet<PrefabLinkComponent>(entity);
    if (link == nullptr) {
        return {};
    }
    // The nearest root of an instance is the one it is in, if any: an instance placed inside
    // another is no part of that one.
    for (Entity above = entity; !above.isNull(); above = world.parent(above)) {
        if (isPrefabInstanceRoot(world, above)) {
            return world.tryGet<PrefabInstanceComponent>(above)->prefab == link->prefab ? above : Entity{};
        }
    }
    return {};
}

bool isPrefabInstanceRoot(const World& world, Entity entity) {
    const auto* instance = world.tryGet<PrefabInstanceComponent>(entity);
    const auto* link = world.tryGet<PrefabLinkComponent>(entity);
    return instance != nullptr && link != nullptr && link->prefab == instance->prefab;
}

std::vector<Entity> linkedPrefabRoots(const World& world, std::optional<Uuid> prefab) {
    std::vector<Entity> roots;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (isPrefabInstanceRoot(world, entity) &&
            (!prefab || world.tryGet<PrefabInstanceComponent>(entity)->prefab == *prefab)) {
            roots.push_back(entity);
        }
    });
    return roots;
}

} // namespace cordite
