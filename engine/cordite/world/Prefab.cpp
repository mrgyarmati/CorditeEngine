#include "cordite/world/Prefab.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Json.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include <array>
#include <vector>

namespace cordite {
namespace {

/// The name the component that marks an instance has in files.
constexpr const char* kPrefabInstanceName = "PrefabInstance";

} // namespace

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
    // hangs under nothing, and is no instance.
    Json& first = entities.front();
    first["parent"] = nullptr;
    if (const auto held = first.find("components"); held != first.end() && held->is_object()) {
        held->erase(kPrefabInstanceName);
    }
    return entities;
}

} // namespace cordite
