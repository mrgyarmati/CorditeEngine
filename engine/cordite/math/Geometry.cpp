#include "cordite/math/Geometry.h"

#include <algorithm>

namespace cordite {

Aabb Aabb::fromCenterExtents(const Vec3& center, const Vec3& halfExtents) {
    Aabb box;
    box.min = center - halfExtents;
    box.max = center + halfExtents;
    return box;
}

Aabb Aabb::fromPoints(std::span<const Vec3> points) {
    Aabb box;
    for (const Vec3& point : points) {
        box.expand(point);
    }
    return box;
}

void Aabb::expand(const Vec3& point) {
    min = glm::min(min, point);
    max = glm::max(max, point);
}

void Aabb::expand(const Aabb& other) {
    if (other.isEmpty()) {
        return;
    }
    min = glm::min(min, other.min);
    max = glm::max(max, other.max);
}

bool Aabb::contains(const Vec3& point) const {
    return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y && point.z >= min.z &&
           point.z <= max.z;
}

bool Aabb::contains(const Aabb& other) const {
    return !other.isEmpty() && contains(other.min) && contains(other.max);
}

bool Aabb::intersects(const Aabb& other) const {
    if (isEmpty() || other.isEmpty()) {
        return false;
    }
    return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
           min.z <= other.max.z && max.z >= other.min.z;
}

Aabb Aabb::transformed(const Mat4& matrix) const {
    if (isEmpty()) {
        return {};
    }

    // Arvo's method: transform the centre, and transform the extents by the absolute
    // value of the linear part. This avoids transforming all eight corners.
    const Vec3 boxCenter = center();
    const Vec3 boxExtents = extents();

    const Vec3 newCenter = Vec3(matrix * Vec4(boxCenter, 1.0f));
    const Mat3 linear(matrix);
    const Mat3 absolute(glm::abs(linear[0]), glm::abs(linear[1]), glm::abs(linear[2]));
    const Vec3 newExtents = absolute * boxExtents;

    return fromCenterExtents(newCenter, newExtents);
}

std::array<Vec3, 8> Aabb::corners() const {
    return {{
        {min.x, min.y, min.z},
        {max.x, min.y, min.z},
        {min.x, max.y, min.z},
        {max.x, max.y, min.z},
        {min.x, min.y, max.z},
        {max.x, min.y, max.z},
        {min.x, max.y, max.z},
        {max.x, max.y, max.z},
    }};
}

Sphere Sphere::fromAabb(const Aabb& box) {
    if (box.isEmpty()) {
        return {};
    }
    return {box.center(), glm::length(box.extents())};
}

bool Sphere::contains(const Vec3& point) const {
    const Vec3 offset = point - center;
    return glm::dot(offset, offset) <= radius * radius;
}

bool Sphere::intersects(const Sphere& other) const {
    const Vec3 offset = other.center - center;
    const float reach = radius + other.radius;
    return glm::dot(offset, offset) <= reach * reach;
}

Plane Plane::fromPointNormal(const Vec3& point, const Vec3& normal) {
    const Vec3 unitNormal = normalizeOr(normal, kWorldUp);
    return {unitNormal, -glm::dot(unitNormal, point)};
}

Plane Plane::fromPoints(const Vec3& a, const Vec3& b, const Vec3& c) {
    return fromPointNormal(a, glm::cross(b - a, c - a));
}

std::optional<float> intersect(const Ray& ray, const Aabb& box) {
    if (box.isEmpty()) {
        return std::nullopt;
    }

    // Slab method. Division by a zero direction component yields +-infinity, which the
    // min/max below handle correctly as long as the origin is not exactly on a slab
    // boundary, so that case is treated separately.
    float nearDistance = 0.0f;
    float farDistance = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(ray.direction[axis]) < kEpsilon) {
            if (ray.origin[axis] < box.min[axis] || ray.origin[axis] > box.max[axis]) {
                return std::nullopt;
            }
            continue;
        }

        const float inverse = 1.0f / ray.direction[axis];
        float entry = (box.min[axis] - ray.origin[axis]) * inverse;
        float exit = (box.max[axis] - ray.origin[axis]) * inverse;
        if (entry > exit) {
            std::swap(entry, exit);
        }
        nearDistance = std::max(nearDistance, entry);
        farDistance = std::min(farDistance, exit);
        if (nearDistance > farDistance) {
            return std::nullopt;
        }
    }
    return nearDistance;
}

std::optional<float> intersect(const Ray& ray, const Sphere& sphere) {
    const Vec3 toOrigin = ray.origin - sphere.center;
    const float halfB = glm::dot(toOrigin, ray.direction);
    const float c = glm::dot(toOrigin, toOrigin) - sphere.radius * sphere.radius;
    if (c <= 0.0f) {
        return 0.0f;
    }

    const float a = glm::dot(ray.direction, ray.direction);
    const float discriminant = halfB * halfB - a * c;
    if (a < kEpsilon || discriminant < 0.0f) {
        return std::nullopt;
    }

    const float distance = (-halfB - std::sqrt(discriminant)) / a;
    if (distance < 0.0f) {
        return std::nullopt;
    }
    return distance;
}

std::optional<float> intersect(const Ray& ray, const Plane& plane) {
    const float denominator = glm::dot(plane.normal, ray.direction);
    if (std::abs(denominator) < kEpsilon) {
        return std::nullopt;
    }

    const float distance = -plane.signedDistance(ray.origin) / denominator;
    if (distance < 0.0f) {
        return std::nullopt;
    }
    return distance;
}

std::optional<float> intersectTriangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                       bool cullBackFaces) {
    // Moller-Trumbore.
    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 p = glm::cross(ray.direction, edge2);
    const float determinant = glm::dot(edge1, p);

    if (cullBackFaces ? determinant < kEpsilon : std::abs(determinant) < kEpsilon) {
        return std::nullopt;
    }

    const float inverseDeterminant = 1.0f / determinant;
    const Vec3 toOrigin = ray.origin - a;
    const float u = glm::dot(toOrigin, p) * inverseDeterminant;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }

    const Vec3 q = glm::cross(toOrigin, edge1);
    const float v = glm::dot(ray.direction, q) * inverseDeterminant;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }

    const float distance = glm::dot(edge2, q) * inverseDeterminant;
    if (distance < 0.0f) {
        return std::nullopt;
    }
    return distance;
}

} // namespace cordite
