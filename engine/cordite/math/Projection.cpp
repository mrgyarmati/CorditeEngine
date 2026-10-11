#include "cordite/math/Projection.h"

#include "cordite/core/Assert.h"

#include <algorithm>

namespace cordite {

// All matrices below assume a right-handed view space looking down -Z, and produce
// Direct3D-style clip space: x, y in [-w, w] with +Y up, z in [0, w].

Mat4 perspectiveReverseZ(float verticalFov, float aspect, float nearPlane, float farPlane) {
    CORDITE_ASSERT(nearPlane > 0.0f && farPlane > nearPlane, "invalid planes: near {}, far {}", nearPlane, farPlane);
    CORDITE_ASSERT(aspect > 0.0f && verticalFov > 0.0f, "invalid fov {} or aspect {}", verticalFov, aspect);

    const float focal = 1.0f / std::tan(verticalFov * 0.5f);
    const float range = farPlane - nearPlane;

    Mat4 matrix(0.0f);
    matrix[0][0] = focal / aspect;
    matrix[1][1] = focal;
    matrix[2][2] = nearPlane / range;
    matrix[2][3] = -1.0f;
    matrix[3][2] = nearPlane * farPlane / range;
    return matrix;
}

Mat4 perspectiveInfiniteReverseZ(float verticalFov, float aspect, float nearPlane) {
    CORDITE_ASSERT(nearPlane > 0.0f, "near plane must be positive, got {}", nearPlane);
    CORDITE_ASSERT(aspect > 0.0f && verticalFov > 0.0f, "invalid fov {} or aspect {}", verticalFov, aspect);

    const float focal = 1.0f / std::tan(verticalFov * 0.5f);

    Mat4 matrix(0.0f);
    matrix[0][0] = focal / aspect;
    matrix[1][1] = focal;
    matrix[2][3] = -1.0f;
    matrix[3][2] = nearPlane;
    return matrix;
}

Mat4 orthographicReverseZ(float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    CORDITE_ASSERT(right != left && top != bottom && farPlane != nearPlane, "degenerate orthographic volume");

    const float range = farPlane - nearPlane;

    Mat4 matrix(1.0f);
    matrix[0][0] = 2.0f / (right - left);
    matrix[1][1] = 2.0f / (top - bottom);
    matrix[2][2] = 1.0f / range;
    matrix[3][0] = -(right + left) / (right - left);
    matrix[3][1] = -(top + bottom) / (top - bottom);
    matrix[3][2] = farPlane / range;
    return matrix;
}

Mat4 orthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane) {
    CORDITE_ASSERT(right != left && top != bottom && farPlane != nearPlane, "degenerate orthographic volume");
    return glm::orthoRH_ZO(left, right, bottom, top, nearPlane, farPlane);
}

Mat4 perspective(float verticalFov, float aspect, float nearPlane, float farPlane) {
    CORDITE_ASSERT(nearPlane > 0.0f && farPlane > nearPlane, "invalid planes: near {}, far {}", nearPlane, farPlane);
    return glm::perspectiveRH_ZO(verticalFov, aspect, nearPlane, farPlane);
}

Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    return glm::lookAtRH(eye, target, up);
}

float linearizeReverseZ(float depth, float nearPlane, float farPlane) {
    // Inverse of the depth mapping in perspectiveReverseZ.
    return nearPlane * farPlane / (nearPlane + depth * (farPlane - nearPlane));
}

float halton(uint32_t index, uint32_t base) {
    CORDITE_ASSERT(base >= 2, "halton base must be at least 2, got {}", base);
    base = std::max(base, 2u);

    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

Vec2 temporalJitter(uint32_t frameIndex, uint32_t sampleCount) {
    const uint32_t sample = (frameIndex % std::max(sampleCount, 1u)) + 1;
    return {halton(sample, 2) - 0.5f, halton(sample, 3) - 0.5f};
}

} // namespace cordite
