#include "cordite/world/SceneSerializer.h"

#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace cordite {
namespace {

constexpr std::array<std::string_view, 4> kSceneMembers = {"format", "version", "name", "entities"};
constexpr std::array<std::string_view, 5> kEntityMembers = {"id", "name", "enabled", "parent", "components"};

/// Maps the identifier of each entity being duplicated to the identifier of its copy.
using IdMap = std::unordered_map<Uuid, Uuid>;

Json serializeEntity(const World& world, Entity entity, const ComponentRegistry& components) {
    Json json = Json::object();
    json["id"] = world.id(entity).toString();
    json["name"] = world.name(entity);
    json["enabled"] = world.isEnabled(entity);

    const Entity parent = world.parent(entity);
    json["parent"] = parent.isNull() ? Json(nullptr) : Json(world.id(parent).toString());

    Json componentsJson = Json::object();
    for (const ComponentDescriptor* descriptor : components.components()) {
        if (const void* instance = descriptor->getConst(world.registry(), entity.handle)) {
            componentsJson[descriptor->type.name()] = descriptor->type.toJson(instance);
        }
    }
    json["components"] = std::move(componentsJson);
    return json;
}

void remapEntityReference(Json& value, const IdMap& idMap) {
    if (!value.is_string()) {
        return;
    }
    const std::optional<Uuid> id = Uuid::parse(value.get<std::string>());
    if (!id) {
        return;
    }
    if (const auto mapped = idMap.find(*id); mapped != idMap.end()) {
        value = mapped->second.toString();
    }
}

/// Redirects the entity references inside the JSON of a component, or of a struct nested
/// in one, according to `idMap`. References to entities outside the map stay as they are.
/// Malformed values are left alone: parsing reports them afterwards.
void remapEntityReferences(Json& json, const TypeInfo& type, const IdMap& idMap) {
    if (!json.is_object()) {
        return;
    }
    for (const PropertyInfo& property : type.properties()) {
        const auto member = json.find(property.name());
        if (member == json.end()) {
            continue;
        }
        if (property.type() == PropertyType::Object && property.objectType() != nullptr) {
            remapEntityReferences(*member, *property.objectType(), idMap);
        } else if (property.attributes().semantic == PropertySemantic::EntityReference) {
            if (member->is_array()) {
                for (Json& element : *member) {
                    remapEntityReference(element, idMap);
                }
            } else {
                remapEntityReference(*member, idMap);
            }
        }
    }
}

/// The entity-level members of one entry of an "entities" array.
struct EntityRecord {
    Uuid id;
    std::string name;
    bool enabled = true;
    /// Nil when the entity has no parent.
    Uuid parentId;
    /// Points into the document being read; null when the entry has no components.
    const Json* components = nullptr;
};

Result<EntityRecord> parseEntityRecord(const Json& json) {
    if (!json.is_object()) {
        return makeError(ErrorCode::ParseError, "expected an object, found {}", jsonTypeName(json));
    }
    CORDITE_TRY(rejectUnknownMembers(json, kEntityMembers));

    EntityRecord record;

    CORDITE_TRY_ASSIGN(const std::string idText, jsonGet<std::string>(json, "id"));
    const std::optional<Uuid> id = Uuid::parse(idText);
    if (!id || id->isNil()) {
        return makeError(ErrorCode::ParseError, "'{}' is not a valid entity id", idText);
    }
    record.id = *id;

    CORDITE_TRY_ASSIGN(std::optional<std::string> name, jsonGetOptional<std::string>(json, "name"));
    record.name = std::move(name).value_or("Entity");

    CORDITE_TRY_ASSIGN(const std::optional<bool> enabled, jsonGetOptional<bool>(json, "enabled"));
    record.enabled = enabled.value_or(true);

    CORDITE_TRY_ASSIGN(const std::optional<std::string> parentText, jsonGetOptional<std::string>(json, "parent"));
    if (parentText) {
        const std::optional<Uuid> parentId = Uuid::parse(*parentText);
        if (!parentId || parentId->isNil()) {
            return makeError(ErrorCode::ParseError, "'parent' must be an entity id or null, found '{}'", *parentText);
        }
        record.parentId = *parentId;
    }

    if (const auto components = json.find("components"); components != json.end() && !components->is_null()) {
        if (!components->is_object()) {
            return makeError(ErrorCode::ParseError, "'components' must be an object, found {}",
                             jsonTypeName(*components));
        }
        record.components = &*components;
    }
    return record;
}

struct ComponentPlan {
    const ComponentDescriptor* descriptor = nullptr;
    PropertyPatch patch;
};

/// An entity that has passed every check and is ready to be created.
struct EntityPlan {
    Uuid id;
    std::string name;
    bool enabled = true;
    /// Position of the parent in the list of plans, when the parent is part of the group.
    std::optional<size_t> parentIndex;
    /// The parent the entity was serialized with, when that is not part of the group.
    Uuid outsideParentId;
    std::vector<ComponentPlan> components;
};

struct PlanOptions {
    bool generateNewIds = false;
    /// Treat a parent that is not part of the array as an error, as in a whole scene.
    bool requireParentsInGroup = false;
    /// When set, the identifiers of the planned entities must be free in this world.
    const World* destination = nullptr;
};

/// The entity-level members of every entry of an "entities" array, and where each id is.
struct EntityRecords {
    std::vector<EntityRecord> records;
    std::unordered_map<Uuid, size_t> indexById;
};

/// Reads the entity-level data of every entry into `result`, so that parents and
/// references can be resolved whatever the order of the entries.
Status parseEntityRecords(const Json& entities, EntityRecords& result) {
    if (!entities.is_array()) {
        return makeError(ErrorCode::ParseError, "'entities' must be an array, found {}", jsonTypeName(entities));
    }

    result.records.reserve(entities.size());
    for (size_t index = 0; index < entities.size(); ++index) {
        Result<EntityRecord> record = parseEntityRecord(entities[index]);
        if (!record) {
            return tl::unexpected(record.error().withContext(fmt::format("entity #{}", index)));
        }
        if (!result.indexById.emplace(record->id, index).second) {
            return makeError(ErrorCode::ParseError, "entity #{} repeats the id {}", index, record->id.toString());
        }
        result.records.push_back(std::move(*record));
    }
    return {};
}

/// Works out where the entity at `index` is attached, and records it in `plan`. A parent
/// that is part of the array has to come before its children, because that is the order
/// in which entities are created.
Status planParent(EntityPlan& plan, const EntityRecords& all, size_t index, bool requireParentsInGroup) {
    const Uuid& parentId = all.records[index].parentId;
    if (parentId.isNil()) {
        return {};
    }

    const auto parent = all.indexById.find(parentId);
    if (parent == all.indexById.end()) {
        if (requireParentsInGroup) {
            return makeError(ErrorCode::ParseError, "its parent {} does not exist", parentId.toString());
        }
        plan.outsideParentId = parentId;
        return {};
    }
    if (parent->second == index) {
        return makeError(ErrorCode::ParseError, "it is listed as its own parent");
    }
    if (parent->second > index) {
        return makeError(ErrorCode::ParseError, "its parent must be listed before it");
    }
    plan.parentIndex = parent->second;
    return {};
}

/// Validates the components of one entry. With `newIds`, references to entities that
/// are being given new ids are redirected to those first.
Result<std::vector<ComponentPlan>> planComponents(const Json& componentsJson, const ComponentRegistry& components,
                                                  const IdMap* newIds) {
    std::vector<ComponentPlan> plans;
    plans.reserve(componentsJson.size());
    for (const auto& [componentName, componentJson] : componentsJson.items()) {
        const ComponentDescriptor* descriptor = components.find(componentName);
        if (descriptor == nullptr) {
            return makeError(ErrorCode::NotFound, "unknown component '{}'", componentName);
        }

        // The input is only copied when references in it have to be rewritten.
        const Json* source = &componentJson;
        Json redirected;
        if (newIds != nullptr) {
            redirected = componentJson;
            remapEntityReferences(redirected, descriptor->type, *newIds);
            source = &redirected;
        }
        CORDITE_TRY_ASSIGN(PropertyPatch patch, descriptor->type.parse(*source));
        plans.push_back({descriptor, std::move(patch)});
    }
    return plans;
}

/// Validates an "entities" array completely. Nothing is created: the result describes
/// what createPlanned will do, which then has no reason left to fail.
Result<std::vector<EntityPlan>> planEntities(const Json& entities, const ComponentRegistry& components,
                                             const PlanOptions& options) {
    EntityRecords all;
    CORDITE_TRY(parseEntityRecords(entities, all));

    IdMap newIds;
    if (options.generateNewIds) {
        for (const EntityRecord& record : all.records) {
            newIds.emplace(record.id, Uuid::generate());
        }
    }

    std::vector<EntityPlan> plans;
    plans.reserve(all.records.size());
    for (size_t index = 0; index < all.records.size(); ++index) {
        const EntityRecord& record = all.records[index];
        // Every problem from here on is reported against the entity it was found in.
        const auto fail = [&record](const Error& error) {
            return tl::unexpected(
                error.withContext(fmt::format("entity '{}' ({})", record.name, record.id.toString())));
        };

        EntityPlan plan;
        plan.id = options.generateNewIds ? newIds.at(record.id) : record.id;
        plan.name = record.name;
        plan.enabled = record.enabled;

        if (options.destination != nullptr && !options.destination->findById(plan.id).isNull()) {
            return fail(Error{ErrorCode::AlreadyExists, "an entity with this id already exists in the world"});
        }
        if (const Status status = planParent(plan, all, index, options.requireParentsInGroup); !status) {
            return fail(status.error());
        }
        if (record.components != nullptr) {
            Result<std::vector<ComponentPlan>> planned =
                planComponents(*record.components, components, options.generateNewIds ? &newIds : nullptr);
            if (!planned) {
                return fail(planned.error());
            }
            plan.components = std::move(*planned);
        }
        plans.push_back(std::move(plan));
    }
    return plans;
}

/// Creates the planned entities in order. `outsideParent` decides where an entity goes
/// whose parent is not part of the plan.
Result<std::vector<Entity>> createPlanned(World& world, std::span<const EntityPlan> plans,
                                          const std::function<Entity(const EntityPlan&)>& outsideParent) {
    std::vector<Entity> created;
    created.reserve(plans.size());
    for (const EntityPlan& plan : plans) {
        // Neither call can fail for a plan made by planEntities: identifiers were checked
        // and a parent always precedes its children.
        CORDITE_TRY_ASSIGN(const Entity entity, world.createEntityWithId(plan.id, plan.name));
        world.setEnabled(entity, plan.enabled);

        const Entity parent = plan.parentIndex ? created[*plan.parentIndex] : outsideParent(plan);
        if (!parent.isNull()) {
            const bool keepWorldTransform = false;
            CORDITE_TRY(world.setParent(entity, parent, keepWorldTransform));
        }

        // Components come last, so whoever reacts to them finds the entity in its place.
        for (const ComponentPlan& component : plan.components) {
            component.descriptor->emplace(world.registry(), entity.handle, component.patch);
        }
        created.push_back(entity);
    }
    return created;
}

} // namespace

Json serializeScene(const World& world, const ComponentRegistry& components) {
    Json document = makeDocument(kSceneFormat);
    document["name"] = world.sceneName();

    Json entities = Json::array();
    world.traverse(
        [&](Entity entity, uint32_t /*depth*/) { entities.push_back(serializeEntity(world, entity, components)); });
    document["entities"] = std::move(entities);
    return document;
}

Status deserializeScene(World& world, const Json& document, const ComponentRegistry& components) {
    CORDITE_TRY(readDocumentHeader(document, kSceneFormat));
    CORDITE_TRY(rejectUnknownMembers(document, kSceneMembers));
    CORDITE_TRY_ASSIGN(const std::optional<std::string> name, jsonGetOptional<std::string>(document, "name"));

    const auto entities = document.find("entities");
    if (entities == document.end()) {
        return makeError(ErrorCode::ParseError, "the scene has no 'entities' array");
    }
    CORDITE_TRY_ASSIGN(const std::vector<EntityPlan> plans,
                       planEntities(*entities, components, {.requireParentsInGroup = true}));

    // Everything is valid: only now does the world change.
    world.clear();
    world.setSceneName(name.value_or("Untitled"));
    CORDITE_TRY(createPlanned(world, plans, [](const EntityPlan& /*plan*/) { return Entity{}; }));
    return {};
}

Status saveScene(const std::filesystem::path& path, const World& world, const ComponentRegistry& components) {
    return writeJsonFile(path, serializeScene(world, components));
}

Status loadScene(const std::filesystem::path& path, World& world, const ComponentRegistry& components) {
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(path));
    if (const Status status = deserializeScene(world, document, components); !status) {
        return tl::unexpected(status.error().withContext(pathToUtf8(path)));
    }
    return {};
}

Json serializeEntities(const World& world, std::span<const Entity> roots, const ComponentRegistry& components) {
    const std::unordered_set<Entity> listed(roots.begin(), roots.end());
    const auto isBelowListedEntity = [&](Entity entity) {
        for (Entity ancestor = world.parent(entity); !ancestor.isNull(); ancestor = world.parent(ancestor)) {
            if (listed.contains(ancestor)) {
                return true;
            }
        }
        return false;
    };

    Json entities = Json::array();
    std::unordered_set<Entity> written;
    for (const Entity root : roots) {
        if (!world.isValid(root) || isBelowListedEntity(root) || !written.insert(root).second) {
            continue;
        }
        world.traverseSubtree(root, [&](Entity entity, uint32_t /*depth*/) {
            entities.push_back(serializeEntity(world, entity, components));
        });
    }
    return entities;
}

Result<std::vector<Entity>> instantiateEntities(World& world, const Json& entities, const ComponentRegistry& components,
                                                const InstantiateOptions& options) {
    if (options.parent && !options.parent->isNull() && !world.isValid(*options.parent)) {
        return makeError(ErrorCode::InvalidArgument, "the parent for the new entities is not a valid entity");
    }

    CORDITE_TRY_ASSIGN(
        const std::vector<EntityPlan> plans,
        planEntities(entities, components, {.generateNewIds = options.generateNewIds, .destination = &world}));
    if (options.requireSingleRoot) {
        // A parent is listed before its children, so a single root is always the first.
        const auto topLevel =
            std::ranges::count_if(plans, [](const EntityPlan& plan) { return !plan.parentIndex.has_value(); });
        if (topLevel != 1) {
            return makeError(ErrorCode::ParseError,
                             "the entities must have one root with all the others below it, but {} of them "
                             "have no parent among them",
                             topLevel);
        }
    }

    return createPlanned(world, plans, [&](const EntityPlan& plan) {
        if (options.parent) {
            return *options.parent;
        }
        // Null when the recorded parent no longer exists, which makes the entity a root.
        return plan.outsideParentId.isNil() ? Entity{} : world.findById(plan.outsideParentId);
    });
}

} // namespace cordite
