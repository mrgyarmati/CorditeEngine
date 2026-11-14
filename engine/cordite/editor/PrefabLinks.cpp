#include "cordite/editor/PrefabLinks.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Prefab.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/world/Components.h"

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <string>

namespace cordite {

bool PrefabView::keeps(std::string_view entry) const {
    return std::ranges::find(overrides, entry) != overrides.end();
}

bool PrefabView::keepsProperty(std::string_view component, std::string_view property) const {
    return addedComponent(component) || keeps(fmt::format("{}.{}", component, property));
}

bool PrefabView::addedComponent(std::string_view component) const {
    return keeps(fmt::format("+{}", component));
}

std::vector<std::string> PrefabView::leftOut() const {
    std::vector<std::string> components;
    for (const std::string& entry : overrides) {
        if (entry.starts_with('-') && !entry.starts_with(kDeletedEntityOverride)) {
            components.push_back(entry.substr(1));
        }
    }
    return components;
}

size_t PrefabView::deletedEntities() const {
    return static_cast<size_t>(std::ranges::count_if(
        overrides, [](const std::string& entry) { return entry.starts_with(kDeletedEntityOverride); }));
}

std::string PrefabView::describeOverrides() const {
    std::string text;
    for (const std::string& entry : overrides) {
        if (!entry.starts_with(kDeletedEntityOverride)) {
            text += text.empty() ? entry : ", " + entry;
        }
    }
    if (const size_t deleted = deletedEntities(); deleted > 0) {
        const std::string count = fmt::format("{} deleted {}", deleted, deleted == 1 ? "entity" : "entities");
        text += text.empty() ? count : ", " + count;
    }
    return text;
}

std::string_view prefabMarker(const PrefabView& view) {
    if (view.role == PrefabView::Role::Added) {
        return "+";
    }
    return view.overrides.empty() ? std::string_view() : std::string_view("*");
}

PrefabView prefabViewOf(const World& world, Entity entity) {
    PrefabView view;
    if (!world.isValid(entity)) {
        return view;
    }
    const Entity root = prefabInstanceRootOf(world, entity);
    if (!root.isNull()) {
        view.role = root == entity ? PrefabView::Role::Root : PrefabView::Role::Part;
        view.root = root;
        view.prefab = world.tryGet<PrefabInstanceComponent>(root)->prefab;
        view.overrides = world.tryGet<PrefabLinkComponent>(entity)->overrides;
        if (const auto* inner = world.tryGet<PrefabInstanceComponent>(entity); inner != nullptr && entity != root) {
            view.innerPrefab = inner->prefab;
        }
        return view;
    }
    // Added: no part of an instance, below something that is.
    for (Entity above = world.parent(entity); !above.isNull(); above = world.parent(above)) {
        if (const Entity instance = prefabInstanceRootOf(world, above); !instance.isNull()) {
            view.role = PrefabView::Role::Added;
            view.root = instance;
            view.prefab = world.tryGet<PrefabInstanceComponent>(instance)->prefab;
            return view;
        }
    }
    return view;
}

std::string prefabCommandLine(std::string_view command, const World& world, Entity entity, std::string_view entry) {
    Json parameters = Json::object();
    parameters["entity"] = world.id(entity).toString();
    if (!entry.empty()) {
        parameters["override"] = std::string(entry);
    }
    return fmt::format("{} {}", command, parameters.dump());
}

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
