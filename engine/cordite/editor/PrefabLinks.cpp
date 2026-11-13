#include "cordite/editor/PrefabLinks.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Prefab.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/world/Components.h"

#include <memory>
#include <string>

namespace cordite {

PrefabReader prefabReaderOf(AssetManager& assets) {
    return [&assets](const Uuid& prefab, Json& entities) -> Result<std::string> {
        CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> data, assets.loadPrefab(prefab));
        entities = data->entities;
        const AssetInfo* asset = assets.database().find(prefab);
        return asset != nullptr ? asset->reference() : prefab.toString();
    };
}

Result<Json> currentPrefabEntities(AssetManager& assets, const ComponentRegistry& components, const Uuid& prefab) {
    const PrefabReader read = prefabReaderOf(assets);
    Json entities;
    CORDITE_TRY_ASSIGN(const std::string name, read(prefab, entities));
    Result<Json> resolved = resolvePrefabEntities(components, prefab, name, entities, read);
    if (!resolved) {
        // What is wrong is wrong with a file, and the message says which.
        return tl::unexpected(resolved.error().withContext(name));
    }
    return resolved;
}

std::vector<Entity> syncPrefabInstances(World& world, const ComponentRegistry& components, AssetManager& assets,
                                        std::optional<Uuid> prefab) {
    std::vector<Entity> changed;
    for (const Entity root : linkedPrefabRoots(world, prefab)) {
        // An outer instance brought up to date may have taken this one away.
        if (!world.isValid(root)) {
            continue;
        }
        const Uuid id = world.tryGet<PrefabInstanceComponent>(root)->prefab;
        const Result<Json> entities = currentPrefabEntities(assets, components, id);
        if (!entities) {
            CORDITE_LOG_WARN("Editor", "'{}' is not brought up to date with its prefab {}: {}", world.name(root),
                             id.toString(), entities.error().message);
            continue;
        }
        const Result<PrefabSync> synced = syncPrefabInstance(world, components, root, *entities);
        if (!synced) {
            CORDITE_LOG_WARN("Editor", "'{}' is not brought up to date with its prefab {}: {}", world.name(root),
                             id.toString(), synced.error().message);
            continue;
        }
        if (synced->changed()) {
            changed.push_back(root);
        }
    }
    return changed;
}

} // namespace cordite
