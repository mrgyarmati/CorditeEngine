#pragma once

/// @file
/// The mesh of an entity as it is seen: for a skinned mesh, in the pose it is drawn in.
///
/// The editor has to know where a mesh is without drawing it - to tell what the pointer
/// is over, and to put a box around what is selected. For a mesh that joints move, that
/// is not where it was modelled.
///
/// @code
/// if (const std::optional<PosedMesh> mesh = posedMeshOf(world, assets, entity)) {
///     const std::optional<MeshHit> hit = intersect(ray, *mesh, world.worldMatrix(entity));
///     selectionLines.box(mesh->bounds, world.worldMatrix(entity), color);
/// }
/// // Or what of the whole world the pointer is over:
/// if (const std::optional<WorldMeshHit> under = raycastMeshes(world, assets, ray)) {
///     const Vec3 point = ray.origin + ray.direction * under->hit.distance;
/// }
/// @endcode
///
/// The pose is the one the renderer uses (findSkeletonPose), under the same conditions:
/// a mesh that is not skinned, has no pose, or follows more joints than its pose has, is
/// where it was modelled.

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Mesh.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cordite {

/// A mesh, and where its vertices are.
struct PosedMesh {
    /// Never null.
    std::shared_ptr<const MeshData> mesh;
    /// Where each vertex of the mesh is in its pose, in the space of the mesh. Empty for
    /// a mesh that is where it was modelled: its vertices say where they are.
    std::vector<Vec3> positions;
    /// Encloses the mesh as it is, in its own space.
    Aabb bounds;

    /// Where vertex `index` is.
    [[nodiscard]] const Vec3& position(size_t index) const {
        return positions.empty() ? mesh->vertices[index].position : positions[index];
    }
};

/// The mesh an entity draws, as it is drawn; nothing for an entity without a
/// MeshRenderer, without a mesh, or with one that cannot be loaded. The world must have
/// been brought up to date (Engine::updateWorld), which is what poses its skeletons.
[[nodiscard]] std::optional<PosedMesh> posedMeshOf(const World& world, AssetManager& assets, Entity entity);

/// Where a ray meets a mesh.
struct MeshHit {
    /// How far along the ray, in lengths of its direction: metres, for a direction of
    /// length one.
    float distance = 0.0f;
    /// The submesh the triangle that was met belongs to: its place in MeshData::submeshes,
    /// which is its place in MeshRendererComponent::materials as well.
    uint32_t submesh = 0;
};

/// Where a ray first hits a mesh that `localToWorld` places in the world; nothing when it
/// misses, and for a mesh that is flattened to nothing.
[[nodiscard]] std::optional<MeshHit> intersect(const Ray& ray, const PosedMesh& mesh, const Mat4& localToWorld);

/// The entity whose mesh a ray hits first, and where.
struct WorldMeshHit {
    Entity entity;
    MeshHit hit;
};

/// Which mesh of the world a ray hits first, as the meshes are drawn (posedMeshOf), of the
/// entities that are enabled; nothing when it hits none. The world must have been brought
/// up to date, as for posedMeshOf().
[[nodiscard]] std::optional<WorldMeshHit> raycastMeshes(const World& world, AssetManager& assets, const Ray& ray);

} // namespace cordite
