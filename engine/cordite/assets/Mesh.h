#pragma once

/// @file
/// CPU-side triangle meshes.
///
/// A MeshData is what importers and generators produce and what the renderer uploads:
/// one vertex array, one index array, and a list of submeshes that each draw a range of
/// the indices with a material of their own. A mesh that a skeleton moves (Skeleton.h)
/// has a second array beside its vertices, which says which joints each vertex follows.
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

#include <array>
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
/// A mesh has at most this many levels of detail besides itself.
inline constexpr uint32_t kMaxMeshLods = 4;

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

/// How a vertex of a skinned mesh follows a skeleton: up to four of its joints move the
/// vertex, each by its weight. Where a joint is at rest and where it is now makes a
/// matrix (see Skeleton.h); the vertex is moved by the four matrices of its joints,
/// blended by their weights.
struct MeshSkinVertex {
    /// Indices into SkeletonData::joints. One that has no weight is zero.
    std::array<uint16_t, 4> joints{};
    /// Never negative, and one in all.
    std::array<float, 4> weights{1.0f, 0.0f, 0.0f, 0.0f};

    bool operator==(const MeshSkinVertex&) const = default;
};

static_assert(sizeof(MeshSkinVertex) == 24, "MeshSkinVertex is uploaded to the GPU as is");

/// A range of a mesh's triangles that is drawn with one material.
struct Submesh {
    uint32_t firstIndex = 0;
    /// A multiple of three. May be zero for a part that turned out to have no triangles.
    uint32_t indexCount = 0;
    /// Encloses the positions this part uses.
    Aabb bounds;

    bool operator==(const Submesh&) const = default;
};

/// A range of the indices of a mesh, or of one of its levels of detail.
struct IndexRange {
    uint32_t firstIndex = 0;
    /// A multiple of three.
    uint32_t indexCount = 0;

    bool operator==(const IndexRange&) const = default;
};

/// A coarser version of a mesh, for where it is seen too small for all of its triangles
/// to matter: fewer triangles, over the same vertices. generateLods() (MeshProcessing.h)
/// makes them.
struct MeshLod {
    /// Three per triangle, indexing the vertices of the mesh.
    std::vector<uint32_t> indices;
    /// One for each submesh of the mesh, in order: its triangles at this level. The
    /// ranges follow each other and together cover `indices` exactly.
    std::vector<IndexRange> submeshes;
    /// How far the surface of this version is from that of the mesh, at the most, in the
    /// units of the mesh's own space: what decides where it is drawn instead.
    float error = 0.0f;

    bool operator==(const MeshLod&) const = default;
};

struct MeshData {
    std::string name;
    std::vector<MeshVertex> vertices;
    /// For a mesh that a skeleton moves: how each vertex follows it, one entry for every
    /// vertex. Empty for a mesh that is rigid.
    std::vector<MeshSkinVertex> skin;
    /// Three per triangle, indexing `vertices`.
    std::vector<uint32_t> indices;
    /// Submesh `i` is drawn with material `i` of whatever renders the mesh. The ranges
    /// follow each other and together cover `indices` exactly.
    std::vector<Submesh> submeshes;
    /// Encloses every position.
    Aabb bounds;
    /// Coarser versions of the mesh, each with fewer triangles than the one before and an
    /// error no smaller (generateLods()). Empty for a mesh that has none. They index the
    /// vertices as they are: whatever changes the vertices or the submeshes drops them,
    /// so they are made last.
    std::vector<MeshLod> lods;

    [[nodiscard]] size_t triangleCount() const { return indices.size() / 3; }

    [[nodiscard]] bool isSkinned() const { return !skin.empty(); }

    bool operator==(const MeshData&) const = default;
};

/// Checks everything the renderer relies on: the limits above, indices that refer to
/// existing vertices, submeshes that tile the index array, and finite numbers; for a
/// skinned mesh also that every vertex has its joints, with weights that are not
/// negative and one in all. The error names the mesh and the first thing that is wrong.
/// Bounds are not checked; call updateBounds() to make them right. Nor is it checked
/// that a skeleton has the joints the vertices name: a mesh does not know its skeleton
/// (jointsUsed() says how many it must have).
[[nodiscard]] Status validateMesh(const MeshData& mesh);

/// Recomputes the bounds of the mesh and of each submesh from the positions in use.
void updateBounds(MeshData& mesh);

/// Appends a submesh made of the given triangles and returns its index. `indices` refer
/// to `vertices`; both are copied behind what the mesh already holds. Levels of detail are
/// dropped: they have no range for the new submesh.
uint32_t appendSubmesh(MeshData& mesh, std::span<const MeshVertex> vertices, std::span<const uint32_t> indices);

/// The same for a skinned mesh: `skin` has an entry for each of `vertices`. A mesh is
/// skinned as a whole or not at all: every submesh of one that is must be given its skin.
uint32_t appendSubmesh(MeshData& mesh, std::span<const MeshVertex> vertices, std::span<const MeshSkinVertex> skin,
                       std::span<const uint32_t> indices);

/// How many joints a skeleton must have for the mesh: one more than the highest joint a
/// vertex follows with a weight. Zero for a mesh that is not skinned.
[[nodiscard]] uint32_t jointsUsed(const MeshData& mesh);

/// For every joint the mesh uses - jointsUsed() of them - a box around the vertices that
/// follow it with a weight, in the space of the mesh; an empty box for a joint that
/// nothing follows. A pose moves a vertex to somewhere between the places its joints
/// would each take it to, so the boxes, each moved by its joint, enclose the mesh in
/// every pose (skinnedBounds() in animation/Pose.h). No boxes for a mesh that is not
/// skinned.
[[nodiscard]] std::vector<Aabb> jointBounds(const MeshData& mesh);

/// Makes the weights of every vertex what validateMesh() asks for: what is negative or
/// no number becomes nothing, the rest is scaled to one in all, and a joint without
/// weight becomes joint zero. A vertex that is left with no weight at all follows its
/// first joint alone. Returns how many vertices that was: more than none says that the
/// mesh did not come with what it needs.
uint32_t normalizeSkinWeights(MeshData& mesh);

} // namespace cordite
