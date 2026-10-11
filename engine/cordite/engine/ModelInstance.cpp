#include "cordite/engine/ModelInstance.h"

#include "cordite/core/Assert.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <vector>

namespace cordite {
namespace {

/// The far plane of a camera whose file gives none.
constexpr float kDefaultFarPlane = 1000.0f;

/// The name for an entity made from something called `name`. Entity names are never
/// empty and never contain '/', which separates the names in an entity path.
std::string entityName(std::string_view name, std::string_view fallback) {
    std::string result(name.empty() ? fallback : name);
    std::ranges::replace(result, '/', '_');
    return result;
}

CameraComponent makeCamera(const ModelCamera& source) {
    // Out-of-range values are brought into the ranges the component declares, as if
    // they had been typed into the inspector.
    CameraComponent camera;
    camera.projection = source.orthographic ? CameraProjection::Orthographic : CameraProjection::Perspective;
    camera.verticalFovDegrees = std::clamp(toDegrees(source.verticalFov), 1.0f, 179.0f);
    camera.orthographicHeight = std::max(source.orthographicHeight, 0.001f);
    camera.nearPlane = std::max(source.nearPlane, 0.001f);
    camera.farPlane = source.farPlane > camera.nearPlane ? source.farPlane : kDefaultFarPlane;
    camera.primary = false;
    return camera;
}

} // namespace

Entity instantiateModel(World& world, const ModelData& model, const ModelInstanceOptions& options) {
    const Entity instance = world.createEntity(entityName(options.name.empty() ? model.name : options.name, "Model"));
    if (!options.parent.isNull()) {
        // A new entity has no children, so this cannot make a cycle and cannot fail.
        CORDITE_VERIFY(world.setParent(instance, options.parent, false).has_value(),
                       "cannot put the model under its parent");
    }

    // Depth first from the roots, so that every entity exists before its children and
    // siblings keep the order they have in the model.
    struct Pending {
        uint32_t node;
        Entity parent;
    };

    std::vector<Pending> pending;
    for (const uint32_t root : model.roots | std::views::reverse) {
        pending.push_back({root, instance});
    }
    // A model made by the importer is a forest; one put together by hand may not be,
    // and must not send this into an endless loop.
    std::vector<bool> visited(model.nodes.size(), false);

    while (!pending.empty()) {
        const Pending current = pending.back();
        pending.pop_back();
        if (current.node >= model.nodes.size() || visited[current.node]) {
            continue;
        }
        visited[current.node] = true;
        const ModelNode& node = model.nodes[current.node];

        const Entity entity = world.createEntity(entityName(node.name, "Node"));
        world.transform(entity) = node.transform;
        CORDITE_VERIFY(world.setParent(entity, current.parent, false).has_value(),
                       "cannot build the hierarchy of model '{}'", model.name);

        if (!node.mesh.isNil()) {
            auto& renderer = world.add<MeshRendererComponent>(entity);
            renderer.mesh = node.mesh;
            renderer.materials = node.materials;
        }
        if (node.camera) {
            world.add<CameraComponent>(entity) = makeCamera(*node.camera);
        }

        for (const uint32_t child : node.children | std::views::reverse) {
            pending.push_back({child, entity});
        }
    }
    return instance;
}

} // namespace cordite
