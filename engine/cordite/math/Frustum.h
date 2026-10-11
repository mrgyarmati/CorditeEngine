#pragma once

/// @file
/// View frustum for visibility culling.
///
/// @code
/// const Frustum frustum = Frustum::fromViewProjection(projection * view);
/// if (frustum.intersects(worldBounds)) {
///     submit(mesh);
/// }
/// @endcode

#include "cordite/math/Geometry.h"

#include <array>

namespace cordite {

/// Six inward-facing planes extracted from a view-projection matrix.
///
/// The tests are conservative: they never reject something visible, but may accept a box
/// that lies just outside a frustum corner.
class Frustum {
public:
    static constexpr size_t kPlaneCount = 6;

    /// Extracts the planes from a matrix that maps world space to clip space with depth
    /// in [0, w]. Works for standard and reversed depth, and for projections with an
    /// infinite far plane (the missing plane then accepts everything).
    [[nodiscard]] static Frustum fromViewProjection(const Mat4& viewProjection);

    [[nodiscard]] bool intersects(const Aabb& box) const;
    [[nodiscard]] bool intersects(const Sphere& sphere) const;
    [[nodiscard]] bool contains(const Vec3& point) const;

    /// Planes in the order left, right, bottom, top, depth 0, depth 1.
    [[nodiscard]] const std::array<Plane, kPlaneCount>& planes() const { return m_planes; }

private:
    std::array<Plane, kPlaneCount> m_planes;
};

/// Returns the eight world-space corners of the volume a view-projection matrix maps to
/// the clip-space cube. The first four corners are at clip depth 0 and the last four at
/// depth 1; within each group the order is (-x,-y), (+x,-y), (-x,+y), (+x,+y).
[[nodiscard]] std::array<Vec3, 8> frustumCorners(const Mat4& inverseViewProjection);

} // namespace cordite
