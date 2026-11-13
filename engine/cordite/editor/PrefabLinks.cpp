#include "cordite/editor/PrefabLinks.h"

#include "cordite/core/Log.h"
#include "cordite/world/Components.h"
#include "cordite/world/Prefab.h"

#include <cstdint>
#include <memory>

namespace cordite {

std::vector<Entity> linkedPrefabRoots(const World& world, std::optional<Uuid> prefab) {
    std::vector<Entity> roots;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        const auto* instance = world.tryGet<PrefabInstanceComponent>(entity);
        if (instance != nullptr && world.has<PrefabLinkComponent>(entity) && (!prefab || instance->prefab == *prefab)) {
            roots.push_back(entity);
        }
    });
    return roots;
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
        const Result<std::shared_ptr<const PrefabData>> loaded = assets.loadPrefab(id);
        if (!loaded) {
            CORDITE_LOG_WARN("Editor", "'{}' is not brought up to date with its prefab {}: {}", world.name(root),
                             id.toString(), loaded.error().message);
            continue;
        }
        const Result<PrefabSync> synced = syncPrefabInstance(world, components, root, (*loaded)->entities);
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
