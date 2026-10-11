#pragma once

/// @file
/// CPU-side triangle meshes.
///
/// A MeshData is what importers and generators produce and what the renderer uploads:
/// one vertex array, one index array, and a list of submeshes that each draw a range of
/// the indices with a material of their own.
///
/// @code
/// MeshData mesh = makeBox(Vec3(2.0f, 1.0f, 1.0f));
/// CORDITE_TRY(validateMesh(mesh));                   // always, for data that came from a file
///
/// for (const Submesh& part : mesh.submeshes) {
///     drawIndexed(part.firstIndex, part.indexCount, materials[indexOf(part)]);
/// }
/// @endcode
///
/// Conventions: triangles are lists of three indices, front faces wind counter-clockwise,
/// positions are in metres in the mesh's own space, and texture coordinates have their
/// origin at the top-left corner of the image (as in glTF, Vulkan and Direct3D).

#include "cordite/core/Result.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cordite {

/// Limits on what a mesh may hold. Files are untrusted, and a few bytes of JSON can
/// claim billions of vertices; nothing beyond these is ever allocated.
inline constexpr uint32_t kMaxMeshVertices = 1u << 24;
inline constexpr uint32_t kMaxMeshIndices = 3u * (1u << 24);

struct MeshVertex {
    Vec3 position{0.0f};
    /// Unit length.
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// xyz: the unit direction in which `uv0.x` grows, perpendicular to the normal.
    /// w: +1 or -1, so that the bitangent is `cross(normal, tangent.xyz) * tangent.w`.
    /// This is the tangent space of glTF and of MikkTSpace, in which normal maps are baked.
    Vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    Vec2 uv0{0.0f};
    /// A second set of texture coordinates, for textures laid out differently from the
    /// first: ambient occlusion now, lightmaps later. Zero when the mesh has none.
    Vec2 uv1{0.0f};

    bool operator==(const MeshVertex&) const = default;
};

static_assert(sizeof(MeshVertex) == 56, "MeshVertex is uploaded to the GPU and written to files as is");

/// A range of a mesh's triangles that is drawn with one material.
struct Submesh {
    uint32_t firstIndex = 0;
    /// A multiple of three. May be zero for a part that turned out to have no triangles.
    uint32_t indexCount = 0;
    /// Encloses the positions this part uses.
    Aabb bounds;

    bool operator==(const Submesh&) const = default;
};

struct MeshData {
    std::string name;
    std::vector<MeshVertex> vertices;
    /// Three per triangle, indexing `vertices`.
    std::vector<uint32_t> indices;
    /// Submesh `i` is drawn with material `i` of whatever renders the mesh. The ranges
    /// follow each other and together cover `indices` exactly.
    std::vector<Submesh> submeshes;
    /// Encloses every position.
    Aabb bounds;

    [[nodiscard]] size_t triangleCount() const { return indices.size() / 3; }

    bool operator==(const MeshData&) const = default;
};

/// Checks everything the renderer relies on: the limits above, indices that refer to
/// existing vertices, submeshes that tile the index array, and finite numbers. The error
/// names the mesh and the first thing that is wrong. Bounds are not checked; call
/// updateBounds() to make them right.
[[nodiscard]] Status validateMesh(const MeshData& mesh);

/// Recomputes the bounds of the mesh and of each submesh from the positions in use.
void updateBounds(MeshData& mesh);

/// Appends a submesh made of the given triangles and returns its index. `indices` refer
/// to `vertices`; both are copied behind what the mesh already holds.
uint32_t appendSubmesh(MeshData& mesh, std::span<const MeshVertex> vertices, std::span<const uint32_t> indices);

} // namespace cordite
