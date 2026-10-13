#pragma once

/// @file
/// Core math types and conventions.
///
/// Cordite uses glm for vectors, matrices and quaternions and adds its own geometric
/// types on top.
///
/// Conventions (every module relies on these):
/// - Right-handed world space, +Y up, metres. An object with identity rotation faces -Z.
/// - Matrices are column-major and transform column vectors: `world = parent * local`.
/// - Angles are in radians in code and in degrees in user-facing data (editor, JSON).
/// - Clip space is Direct3D-style: x and y in [-1, 1] with +Y up, depth in [0, 1].
///   Scene rendering uses reversed depth (near = 1, far = 0) for precision.
/// - Colors are linear RGB unless a name says sRGB.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <numbers>

namespace cordite {

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;
using UVec2 = glm::uvec2;
using UVec3 = glm::uvec3;
using UVec4 = glm::uvec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;
inline constexpr float kEpsilon = 1e-6f;

inline constexpr Vec3 kWorldRight{1.0f, 0.0f, 0.0f};
inline constexpr Vec3 kWorldUp{0.0f, 1.0f, 0.0f};
inline constexpr Vec3 kWorldForward{0.0f, 0.0f, -1.0f};

/// Identity rotation. glm's quaternion constructor takes (w, x, y, z).
inline constexpr Quat kIdentityQuat{1.0f, 0.0f, 0.0f, 0.0f};

constexpr float toRadians(float degrees) noexcept {
    return degrees * (kPi / 180.0f);
}

constexpr float toDegrees(float radians) noexcept {
    return radians * (180.0f / kPi);
}

/// Clamps to [0, 1].
constexpr float saturate(float value) noexcept {
    if (value < 0.0f) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

inline bool nearlyEqual(float a, float b, float tolerance = 1e-5f) noexcept {
    return std::abs(a - b) <= tolerance;
}

inline bool nearlyEqual(const Vec3& a, const Vec3& b, float tolerance = 1e-5f) noexcept {
    return nearlyEqual(a.x, b.x, tolerance) && nearlyEqual(a.y, b.y, tolerance) && nearlyEqual(a.z, b.z, tolerance);
}

inline bool isFinite(const Vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// Whether four numbers can be made a rotation, by bringing them to unit length: they
/// are finite, not too large to be squared, and not all zero or as good as.
inline bool isRotation(const Quat& rotation) noexcept {
    const float lengthSquared = glm::dot(rotation, rotation);
    return std::isfinite(lengthSquared) && lengthSquared > kEpsilon * kEpsilon;
}

/// Makes four numbers a rotation: a quaternion of length one. One that has that length
/// already - as every rotation the engine has saved or computed has - is returned bit
/// for bit: normalizing it again would nudge its last digits each time, and a rotation
/// that goes through a file, or from one entity to another, would never settle. Returns
/// the rotation that turns nothing when isRotation() says the numbers are none.
inline Quat normalizeRotation(const Quat& rotation) noexcept {
    if (!isRotation(rotation)) {
        return kIdentityQuat;
    }
    constexpr float kUnitLengthTolerance = 1e-5f;
    const float lengthSquared = glm::dot(rotation, rotation);
    return std::abs(lengthSquared - 1.0f) <= kUnitLengthTolerance ? rotation : rotation / std::sqrt(lengthSquared);
}

/// Returns the normalized vector, or `fallback` when the input is too short to normalize.
inline Vec3 normalizeOr(const Vec3& value, const Vec3& fallback) noexcept {
    const float lengthSquared = glm::dot(value, value);
    return lengthSquared > kEpsilon * kEpsilon ? value / std::sqrt(lengthSquared) : fallback;
}

} // namespace cordite
