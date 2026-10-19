#include "cordite/engine/ModelInstance.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/EntityPath.h"

#include <fmt/format.h>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The far plane of a camera whose file gives none.
constexpr float kDefaultFarPlane = 1000.0f;

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

/// A node of a model on its way to becoming an entity: which one, and the node it was
/// reached from - none for a root.
struct Visit {
    uint32_t node = 0;
    int64_t parent = -1;
};

/// The nodes of a model in the order their entities are made: depth first from the
/// roots, every node before its children, siblings in the order the model has them. A
/// model made by the importer is a forest; one put together by hand may not be, so each
/// node is taken once, where it is reached first, and what does not exist is left out.
std::vector<Visit> visitOrder(const ModelData& model) {
    std::vector<Visit> order;
    std::vector<Visit> pending;
    for (const uint32_t root : model.roots | std::views::reverse) {
        pending.push_back({.node = root});
    }
    std::vector<bool> visited(model.nodes.size(), false);
    while (!pending.empty()) {
        const Visit current = pending.back();
        pending.pop_back();
        if (current.node >= model.nodes.size() || visited[current.node]) {
            continue;
        }
        visited[current.node] = true;
        order.push_back(current);
        for (const uint32_t child : model.nodes[current.node].children | std::views::reverse) {
            pending.push_back({.node = child, .parent = current.node});
        }
    }
    return order;
}

/// Which nodes become entities: all that are no joints, and the joints that carry
/// something a pose cannot - a mesh or a camera of their own, or a node below them that
/// becomes an entity.
std::vector<bool> nodesWithEntities(const ModelData& model, const std::vector<Visit>& order) {
    std::vector<bool> kept(model.nodes.size(), false);
    // Children come after their parents in `order`, so backwards every node is
    // settled before the node it hangs on.
    for (const Visit& visit : order | std::views::reverse) {
        const ModelNode& node = model.nodes[visit.node];
        if (!node.joint || !node.mesh.isNil() || node.camera) {
            kept[visit.node] = true;
        }
        // A skinned mesh does not stay where its node is, and so keeps nothing above
        // it (see instantiateModel).
        const bool staysBelow = node.mesh.isNil() || node.skeleton.isNil();
        if (kept[visit.node] && staysBelow && visit.parent >= 0) {
            kept[static_cast<size_t>(visit.parent)] = true;
        }
    }
    return kept;
}

} // namespace

Entity instantiateModel(World& world, const ModelData& model, const ModelInstanceOptions& options) {
    const Entity instance =
        world.createEntity(entityNameFrom(options.name.empty() ? model.name : options.name, "Model"));
    if (!options.parent.isNull()) {
        // A new entity has no children, so this cannot make a cycle and cannot fail.
        CORDITE_VERIFY(world.setParent(instance, options.parent, false).has_value(),
                       "cannot put the model under its parent");
    }
    if (options.position) {
        CORDITE_ASSERT(isFinite(*options.position), "model '{}' is to be placed at a position that is not finite",
                       model.name);
        // Without assertions, a position that is no place leaves the model at the origin.
        if (isFinite(*options.position)) {
            world.transform(instance).position = *options.position;
        }
    }
    const auto putUnder = [&](Entity entity, Entity parent) {
        CORDITE_VERIFY(world.setParent(entity, parent, false).has_value(), "cannot build the hierarchy of model '{}'",
                       model.name);
    };

    const std::vector<Visit> order = visitOrder(model);
    const std::vector<bool> kept = nodesWithEntities(model, order);
    std::vector<Entity> entityOfNode(model.nodes.size());
    // The entities that pose the skeletons of the model, by skeleton, in the order
    // they were needed.
    std::vector<std::pair<Uuid, Entity>> animators;
    uint32_t jointsWithEntities = 0;

    for (const Visit& visit : order) {
        if (!kept[visit.node]) {
            continue;
        }
        const ModelNode& node = model.nodes[visit.node];
        const Entity entity = world.createEntity(entityNameFrom(node.name, "Node"));
        entityOfNode[visit.node] = entity;
        jointsWithEntities += node.joint ? 1 : 0;

        const bool skinned = !node.mesh.isNil() && !node.skeleton.isNil();
        if (skinned) {
            // A skinned mesh is placed by its joints, which are placed in the model:
            // wherever its node is in the file, its entity hangs, unmoved, under what
            // poses its skeleton - one entity for every skeleton, directly under the
            // instance.
            auto animator = std::ranges::find(animators, node.skeleton, &std::pair<Uuid, Entity>::first);
            if (animator == animators.end()) {
                const Entity created = world.createEntity(
                    animators.empty() ? std::string("Skeleton") : fmt::format("Skeleton {}", animators.size() + 1));
                putUnder(created, instance);
                world.add<AnimatorComponent>(created).skeleton = node.skeleton;
                animator = animators.insert(animators.end(), {node.skeleton, created});
            }
            putUnder(entity, animator->second);
        } else {
            world.transform(entity) = node.transform;
            putUnder(entity, visit.parent >= 0 ? entityOfNode[static_cast<size_t>(visit.parent)] : instance);
        }

        if (!node.mesh.isNil()) {
            auto& renderer = world.add<MeshRendererComponent>(entity);
            renderer.mesh = node.mesh;
            renderer.materials = node.materials;
        }
        if (node.camera) {
            world.add<CameraComponent>(entity) = makeCamera(*node.camera);
        }
    }

    if (jointsWithEntities > 0) {
        CORDITE_LOG_WARN("Engine",
                         "the model '{}' has something hanging on {} of its joints: it is placed where the joints "
                         "rest, and does not follow a pose",
                         model.name, jointsWithEntities);
    }
    return instance;
}

std::vector<Uuid> materialsOfMesh(const ModelData& model, const Uuid& mesh) {
    if (mesh.isNil()) {
        // Every node that draws nothing would have it.
        return {};
    }
    const auto drawn = std::ranges::find(model.nodes, mesh, &ModelNode::mesh);
    return drawn != model.nodes.end() ? drawn->materials : std::vector<Uuid>{};
}

} // namespace cordite
