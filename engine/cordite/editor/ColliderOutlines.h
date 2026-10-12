#pragma once

/// @file
/// What colliders that are made of meshes look like, as lines.
///
/// A collider that is a box, a ball or a capsule is drawn from its numbers. One that is
/// the hull around a mesh, or the triangles of one, is drawn from what the physics has
/// made of the mesh - so that what is seen is what things collide with, not what the
/// mesh looks like. The physics is asked once for each, and the answer is kept.
///
/// @code
/// ColliderOutlines outlines;
/// if (const ColliderOutline* outline = outlines.find(engine.physics(), collider.mesh, collider.shape)) {
///     for (size_t edge = 0; edge + 1 < outline->edges.size(); edge += 2) {
///         lines.line(place(outline->edges[edge]), place(outline->edges[edge + 1]), color);
///     }
/// }
/// @endcode

#include "cordite/core/Uuid.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace cordite {

/// The most edges an outline is made of. A shape of more is shown by the box around it:
/// that many lines would be a blot, and cost what a frame takes.
inline constexpr size_t kMaxColliderOutlineEdges = 8192;

/// The shape of a collider that is made of a mesh, in the space of the mesh.
struct ColliderOutline {
    /// Its edges, two points each. Empty for a shape of more than
    /// kMaxColliderOutlineEdges.
    std::vector<Vec3> edges;
    /// The box around it.
    Aabb bounds;
};

class ColliderOutlines {
public:
    /// The outline of what colliders of `shape` - a convex hull, or a mesh - of the mesh
    /// asset `mesh` are made of. Null where the physics has prepared nothing for them:
    /// before the first update that met such a collider, and for a mesh of which no
    /// shape can be made. What is returned is valid until the next call.
    [[nodiscard]] const ColliderOutline* find(const PhysicsSystem& physics, const Uuid& mesh, ShapeType shape);

    /// How many outlines are kept.
    [[nodiscard]] size_t size() const { return m_outlines.size(); }

private:
    std::unordered_map<GeometryId, ColliderOutline> m_outlines;
};

} // namespace cordite
