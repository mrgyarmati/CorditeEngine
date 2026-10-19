#include "cordite/editor/PosedMesh.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/Pose.h"
#include "cordite/renderer/RenderComponents.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace cordite {

std::optional<PosedMesh> posedMeshOf(const World& world, AssetManager& assets, Entity entity) {
    const auto* renderer = world.tryGet<MeshRendererComponent>(entity);
    if (renderer == nullptr || renderer->mesh.isNil()) {
        return std::nullopt;
    }
    // Loaded already if it is on screen; a mesh that cannot be loaded was reported by
    // the renderer and is simply not there for the editor either.
    Result<std::shared_ptr<const MeshData>> loaded = assets.loadMesh(renderer->mesh);
    if (!loaded) {
        return std::nullopt;
    }
    PosedMesh posed{.mesh = std::move(*loaded)};
    const MeshData& mesh = *posed.mesh;
    posed.bounds = mesh.bounds;

    // As the renderer decides it: in a pose only when the pose has every joint the
    // mesh follows.
    const SkeletonPoseComponent* pose = mesh.isSkinned() ? findSkeletonPose(world, entity) : nullptr;
    if (pose == nullptr || pose->skinning.size() < jointsUsed(mesh)) {
        return posed;
    }
    posed.positions.reserve(mesh.vertices.size());
    posed.bounds = Aabb();
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        const Vec3 position = skinPosition(mesh.vertices[index].position, mesh.skin[index], pose->skinning);
        // A joint whose matrix is no number moves nothing on the GPU either.
        const bool isFinite = std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
        posed.positions.push_back(isFinite ? position : mesh.vertices[index].position);
        posed.bounds.expand(posed.positions.back());
    }
    return posed;
}

std::optional<MeshHit> intersect(const Ray& ray, const PosedMesh& mesh, const Mat4& localToWorld) {
    if (std::abs(glm::determinant(localToWorld)) < std::numeric_limits<float>::min()) {
        // Flattened to nothing: there is no surface to hit, and no inverse to hit it with.
        return std::nullopt;
    }
    // Tested in the mesh's own space, where the ray's direction has another length:
    // `scale` local units for every metre along the world ray.
    const Mat4 worldToLocal = glm::inverse(localToWorld);
    const Ray local{Vec3(worldToLocal * Vec4(ray.origin, 1.0f)), Vec3(worldToLocal * Vec4(ray.direction, 0.0f))};

    const float scale = glm::length(local.direction);
    if (scale <= 0.0f) {
        return std::nullopt;
    }
    const Ray unit{local.origin, local.direction / scale};
    if (!intersect(unit, mesh.bounds)) {
        return std::nullopt;
    }

    // The submeshes of a mesh that was checked tile its indices; one that was not is
    // read no further than its indices go.
    const std::vector<uint32_t>& indices = mesh.mesh->indices;
    const std::vector<Submesh>& submeshes = mesh.mesh->submeshes;
    std::optional<MeshHit> nearest;
    for (size_t submesh = 0; submesh < submeshes.size(); ++submesh) {
        const size_t end = std::min(static_cast<size_t>(submeshes[submesh].firstIndex) + submeshes[submesh].indexCount,
                                    indices.size());
        for (size_t first = submeshes[submesh].firstIndex; first + 2 < end; first += 3) {
            const std::optional<float> hit =
                intersectTriangle(unit, mesh.position(indices[first]), mesh.position(indices[first + 1]),
                                  mesh.position(indices[first + 2]));
            if (hit && (!nearest || *hit < nearest->distance)) {
                nearest = MeshHit{.distance = *hit, .submesh = static_cast<uint32_t>(submesh)};
            }
        }
    }
    if (nearest) {
        // Back from a distance along the local ray to one along the world ray.
        nearest->distance /= scale;
    }
    return nearest;
}

std::optional<WorldMeshHit> raycastMeshes(const World& world, AssetManager& assets, const Ray& ray) {
    std::optional<WorldMeshHit> nearest;
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        if (const std::optional<PosedMesh> mesh = posedMeshOf(world, assets, entity)) {
            const std::optional<MeshHit> hit = intersect(ray, *mesh, world.worldMatrix(entity));
            if (hit && (!nearest || hit->distance < nearest->hit.distance)) {
                nearest = WorldMeshHit{.entity = entity, .hit = *hit};
            }
        }
    });
    return nearest;
}

} // namespace cordite
