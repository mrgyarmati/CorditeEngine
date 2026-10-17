#pragma once

/// @file
/// Operations that prepare a mesh for rendering: normals, tangents and optimization.
///
/// Importers run them on what a file provides; they are equally usable on a mesh built
/// in code.
///
/// @code
/// MeshData mesh = loadPositionsAndTriangles();       // no normals, no tangents
/// generateFlatNormals(mesh);
/// generateTangents(mesh);
/// optimizeMesh(mesh);
/// CORDITE_TRY(validateMesh(mesh));
/// @endcode
///
/// Every function keeps the order of the triangles within each submesh's range unless it
/// says otherwise, expects a mesh that passes validateMesh(), and leaves one that does.
/// Those that change the vertices drop the levels of detail, which index them: make those
/// last, with generateLods().

#include "cordite/assets/Mesh.h"

namespace cordite {

/// Returns a unit vector perpendicular to the unit vector `normal`. Any will do where a
/// surface has no texture coordinates to define a direction; this one varies smoothly
/// with the normal except at -Z.
[[nodiscard]] Vec3 perpendicularTo(const Vec3& normal);

/// Makes every normal unit length and every tangent unit length and perpendicular to its
/// normal, with a handedness of exactly +1 or -1. A normal without direction becomes +Y,
/// a tangent without one any perpendicular. Data from files is not to be trusted to
/// satisfy what shaders assume.
void repairTangentFrames(MeshData& mesh);

/// Gives every triangle three vertices of its own, with the normal of its plane. This is
/// what glTF prescribes for a mesh that comes without normals. Tangents are left as they
/// were and have to be generated afterwards. Levels of detail are dropped.
void generateFlatNormals(MeshData& mesh);

/// Computes every tangent from positions, normals and `uv0` with MikkTSpace, the
/// algorithm normal maps are baked against. A vertex whose triangles need different
/// tangents is split, so the vertex count can grow. Where the texture coordinates define
/// no direction (a mesh without any, for one) the tangent is an arbitrary perpendicular.
/// Levels of detail are dropped.
void generateTangents(MeshData& mesh);

/// Merges vertices that are exactly equal, drops unused ones, and reorders triangles
/// (within each submesh) and vertices for the GPU's caches. The mesh looks the same
/// afterwards, and the bounds are recomputed. Levels of detail are dropped.
void optimizeMesh(MeshData& mesh);

/// A mesh with fewer triangles than this gets no levels of detail: all of it costs little.
inline constexpr size_t kMinLodTriangles = 256;

/// Makes the levels of detail of a mesh, in place of those it had: up to kMaxMeshLods
/// versions over the same vertices, each aiming at half the triangles of the one before,
/// with the error each has (meshoptimizer's simplifier). Every submesh is simplified on
/// its own, and its open edges stay where they are, so that no gap opens between two
/// submeshes or at the rim of an open surface; one of fewer than 32 triangles stays as
/// it is. The levels end where one would keep more than 85 % of the triangles of the one
/// before - a mesh that cannot give up more without tearing - or have fewer than 64; a
/// mesh of fewer than kMinLodTriangles triangles gets none. The same mesh gets the same
/// levels on every run and machine.
void generateLods(MeshData& mesh);

} // namespace cordite
