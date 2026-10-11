#include "cordite/math/Frustum.h"

namespace cordite {
namespace {

/// Builds a normalized plane from the coefficients (a, b, c, d) of ax + by + cz + d >= 0.
Plane makePlane(const Vec4& coefficients) {
    const Vec3 normal(coefficients);
    const float length = glm::length(normal);
    if (length < kEpsilon) {
        // A projection with an infinite far plane has no far plane to extract. A zero
        // normal with a positive distance accepts every point.
        return {Vec3(0.0f), 1.0f};
    }
    return {normal / length, coefficients.w / length};
}

} // namespace

Frustum Frustum::fromViewProjection(const Mat4& viewProjection) {
    // Gribb-Hartmann plane extraction. glm matrices are column-major, so row i of the
    // matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
    const Mat4 transposed = glm::transpose(viewProjection);
    const Vec4& rowX = transposed[0];
    const Vec4& rowY = transposed[1];
    const Vec4& rowZ = transposed[2];
    const Vec4& rowW = transposed[3];

    Frustum frustum;
    frustum.m_planes[0] = makePlane(rowW + rowX); // -w <= x
    frustum.m_planes[1] = makePlane(rowW - rowX); //  x <= w
    frustum.m_planes[2] = makePlane(rowW + rowY); // -w <= y
    frustum.m_planes[3] = makePlane(rowW - rowY); //  y <= w
    frustum.m_planes[4] = makePlane(rowZ);        //  0 <= z
    frustum.m_planes[5] = makePlane(rowW - rowZ); //  z <= w
    return frustum;
}

bool Frustum::intersects(const Aabb& box) const {
    if (box.isEmpty()) {
        return false;
    }

    const Vec3 center = box.center();
    const Vec3 extents = box.extents();
    for (const Plane& plane : m_planes) {
        // Distance of the box corner furthest along the plane normal.
        const float reach = glm::dot(glm::abs(plane.normal), extents);
        if (plane.signedDistance(center) + reach < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersects(const Sphere& sphere) const {
    for (const Plane& plane : m_planes) {
        if (plane.signedDistance(sphere.center) < -sphere.radius) {
            return false;
        }
    }
    return true;
}

bool Frustum::contains(const Vec3& point) const {
    for (const Plane& plane : m_planes) {
        if (plane.signedDistance(point) < 0.0f) {
            return false;
        }
    }
    return true;
}

std::array<Vec3, 8> frustumCorners(const Mat4& inverseViewProjection) {
    std::array<Vec3, 8> corners;
    size_t index = 0;
    for (const float z : {0.0f, 1.0f}) {
        for (const float y : {-1.0f, 1.0f}) {
            for (const float x : {-1.0f, 1.0f}) {
                const Vec4 world = inverseViewProjection * Vec4(x, y, z, 1.0f);
                corners[index++] = Vec3(world) / world.w;
            }
        }
    }
    return corners;
}

} // namespace cordite
