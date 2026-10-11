#pragma once

/// @file
/// Bounding volumes, planes, rays and their intersection tests.
///
/// @code
/// Aabb bounds = Aabb::fromPoints(positions);
/// const Aabb worldBounds = bounds.transformed(localToWorld);
///
/// const Ray ray{cameraPosition, glm::normalize(target - cameraPosition)};
/// if (const std::optional<float> distance = intersect(ray, worldBounds)) {
///     const Vec3 hitPoint = ray.at(*distance);
/// }
/// @endcode

#include "cordite/math/Math.h"

#include <array>
#include <limits>
#include <optional>
#include <span>

namespace cordite {

/// Axis-aligned bounding box. A default-constructed box is empty (inverted) so that
/// expanding it by the first point produces a box containing exactly that point.
struct Aabb {
    Vec3 min{std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest()};

    [[nodiscard]] static Aabb fromCenterExtents(const Vec3& center, const Vec3& halfExtents);
    [[nodiscard]] static Aabb fromPoints(std::span<const Vec3> points);

    /// True when the box contains no points at all.
    [[nodiscard]] bool isEmpty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

    [[nodiscard]] Vec3 center() const { return 0.5f * (min + max); }

    /// Half of the size along each axis.
    [[nodiscard]] Vec3 extents() const { return 0.5f * (max - min); }

    [[nodiscard]] Vec3 size() const { return max - min; }

    void expand(const Vec3& point);
    void expand(const Aabb& other);

    [[nodiscard]] bool contains(const Vec3& point) const;
    [[nodiscard]] bool contains(const Aabb& other) const;
    [[nodiscard]] bool intersects(const Aabb& other) const;

    /// Returns the axis-aligned box that encloses this box after an affine transform.
    [[nodiscard]] Aabb transformed(const Mat4& matrix) const;

    [[nodiscard]] std::array<Vec3, 8> corners() const;

    bool operator==(const Aabb& other) const { return min == other.min && max == other.max; }
};

struct Sphere {
    Vec3 center{0.0f};
    float radius = 0.0f;

    /// Returns the smallest sphere centred on the box that contains it.
    [[nodiscard]] static Sphere fromAabb(const Aabb& box);

    [[nodiscard]] bool contains(const Vec3& point) const;
    [[nodiscard]] bool intersects(const Sphere& other) const;
};

/// The plane of points p satisfying dot(normal, p) + distance == 0. Points with a
/// positive signed distance are in front of the plane.
struct Plane {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;

    [[nodiscard]] static Plane fromPointNormal(const Vec3& point, const Vec3& normal);

    /// Plane through three points; the normal follows the counter-clockwise winding.
    [[nodiscard]] static Plane fromPoints(const Vec3& a, const Vec3& b, const Vec3& c);

    [[nodiscard]] float signedDistance(const Vec3& point) const { return glm::dot(normal, point) + distance; }
};

/// A half-line. `direction` must be normalized for the returned distances to be metres.
struct Ray {
    Vec3 origin{0.0f};
    Vec3 direction = kWorldForward;

    [[nodiscard]] Vec3 at(float distance) const { return origin + direction * distance; }
};

/// Each intersect() returns the distance along the ray to the nearest hit at or in front
/// of the origin, or nullopt when the ray misses. A ray starting inside a box or sphere
/// reports distance 0.
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Aabb& box);
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Sphere& sphere);
[[nodiscard]] std::optional<float> intersect(const Ray& ray, const Plane& plane);

/// Ray against the triangle (a, b, c). With `cullBackFaces`, hits on the side where the
/// triangle appears clockwise are ignored. The edges belong to the triangle, so a ray
/// through an edge that two triangles share hits at least one of them; a triangle
/// without area is never hit.
[[nodiscard]] std::optional<float> intersectTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                                     bool cullBackFaces = false);

} // namespace cordite
