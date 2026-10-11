#pragma once

/// @file
/// Translation, rotation and scale.
///
/// @code
/// Transform transform;
/// transform.position = {0.0f, 2.0f, -5.0f};
/// transform.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
/// const Mat4 localToParent = transform.toMatrix();
/// const Vec3 facing = transform.forward();
/// @endcode

#include "cordite/math/Math.h"

namespace cordite {

/// A local transform applied as scale, then rotation, then translation.
///
/// World-space placement is kept as a Mat4, because composing rotated, non-uniformly
/// scaled transforms produces shear that a Transform cannot represent.
struct Transform {
    Vec3 position{0.0f};
    Quat rotation = kIdentityQuat;
    Vec3 scale{1.0f};

    [[nodiscard]] Mat4 toMatrix() const;

    /// Decomposes an affine matrix. Shear and perspective are discarded, and a negative
    /// determinant is folded into the X scale.
    [[nodiscard]] static Transform fromMatrix(const Mat4& matrix);

    /// Unit vectors of the rotated local axes.
    [[nodiscard]] Vec3 forward() const { return rotation * kWorldForward; }

    [[nodiscard]] Vec3 right() const { return rotation * kWorldRight; }

    [[nodiscard]] Vec3 up() const { return rotation * kWorldUp; }

    /// Applies scale, rotation and translation to a point.
    [[nodiscard]] Vec3 transformPoint(const Vec3& point) const { return position + rotation * (scale * point); }

    /// Applies rotation only.
    [[nodiscard]] Vec3 transformDirection(const Vec3& direction) const { return rotation * direction; }

    /// Interpolates position and scale linearly and rotation along the shortest arc.
    [[nodiscard]] static Transform interpolate(const Transform& from, const Transform& to, float t);

    bool operator==(const Transform& other) const {
        return position == other.position && rotation == other.rotation && scale == other.scale;
    }
};

/// Builds a rotation from Euler angles in degrees, applied in Y (yaw), X (pitch),
/// Z (roll) order. This is the order the editor inspector shows.
Quat quatFromEulerDegrees(const Vec3& degrees);

/// Returns Euler angles in degrees matching quatFromEulerDegrees. Near the poles
/// (pitch of +-90 degrees) roll is reported as zero.
Vec3 eulerDegreesFromQuat(const Quat& rotation);

/// Returns the rotation that makes -Z point along `direction`, keeping +Y as close to
/// `up` as possible. Falls back to a stable frame when `direction` is parallel to `up`.
Quat lookRotation(const Vec3& direction, const Vec3& up = kWorldUp);

} // namespace cordite
