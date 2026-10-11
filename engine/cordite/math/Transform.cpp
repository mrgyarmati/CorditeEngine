#include "cordite/math/Transform.h"

#include <algorithm>

namespace cordite {

Mat4 Transform::toMatrix() const {
    Mat4 matrix = glm::mat4_cast(rotation);
    matrix[0] *= scale.x;
    matrix[1] *= scale.y;
    matrix[2] *= scale.z;
    matrix[3] = Vec4(position, 1.0f);
    return matrix;
}

Transform Transform::fromMatrix(const Mat4& matrix) {
    Transform result;
    result.position = Vec3(matrix[3]);

    Vec3 axisX = Vec3(matrix[0]);
    Vec3 axisY = Vec3(matrix[1]);
    Vec3 axisZ = Vec3(matrix[2]);

    result.scale = Vec3(glm::length(axisX), glm::length(axisY), glm::length(axisZ));
    if (glm::dot(glm::cross(axisX, axisY), axisZ) < 0.0f) {
        result.scale.x = -result.scale.x;
    }

    // A zero scale leaves that axis without a direction; substitute the world axis so
    // the rotation stays well defined.
    axisX = std::abs(result.scale.x) > kEpsilon ? axisX / result.scale.x : kWorldRight;
    axisY = std::abs(result.scale.y) > kEpsilon ? axisY / result.scale.y : kWorldUp;
    axisZ = std::abs(result.scale.z) > kEpsilon ? axisZ / result.scale.z : Vec3(0.0f, 0.0f, 1.0f);

    result.rotation = glm::normalize(glm::quat_cast(Mat3(axisX, axisY, axisZ)));
    return result;
}

Transform Transform::interpolate(const Transform& from, const Transform& to, float t) {
    Transform result;
    result.position = glm::mix(from.position, to.position, t);
    result.rotation = glm::slerp(from.rotation, to.rotation, t);
    result.scale = glm::mix(from.scale, to.scale, t);
    return result;
}

Quat quatFromEulerDegrees(const Vec3& degrees) {
    const Quat yaw = glm::angleAxis(toRadians(degrees.y), kWorldUp);
    const Quat pitch = glm::angleAxis(toRadians(degrees.x), kWorldRight);
    const Quat roll = glm::angleAxis(toRadians(degrees.z), Vec3(0.0f, 0.0f, 1.0f));
    return glm::normalize(yaw * pitch * roll);
}

Vec3 eulerDegreesFromQuat(const Quat& rotation) {
    // For R = Ry(yaw) * Rx(pitch) * Rz(roll), the third column is
    // (sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)).
    const Mat3 matrix = glm::mat3_cast(glm::normalize(rotation));
    const float sinPitch = std::clamp(-matrix[2][1], -1.0f, 1.0f);
    const float pitch = std::asin(sinPitch);

    float yaw = 0.0f;
    float roll = 0.0f;
    constexpr float kGimbalThreshold = 0.99999f;
    if (std::abs(sinPitch) < kGimbalThreshold) {
        yaw = std::atan2(matrix[2][0], matrix[2][2]);
        roll = std::atan2(matrix[0][1], matrix[1][1]);
    } else {
        // Looking straight up or down: yaw and roll rotate about the same axis.
        yaw = std::atan2(-matrix[0][2], matrix[0][0]);
    }
    return {toDegrees(pitch), toDegrees(yaw), toDegrees(roll)};
}

Quat lookRotation(const Vec3& direction, const Vec3& up) {
    const Vec3 forward = normalizeOr(direction, kWorldForward);

    Vec3 right = glm::cross(forward, up);
    if (glm::dot(right, right) < kEpsilon) {
        // `direction` is parallel to `up`; pick any axis that is not.
        const Vec3 alternative = std::abs(forward.y) < 0.99f ? kWorldUp : kWorldRight;
        right = glm::cross(forward, alternative);
    }
    right = glm::normalize(right);
    const Vec3 correctedUp = glm::cross(right, forward);

    return glm::normalize(glm::quat_cast(Mat3(right, correctedUp, -forward)));
}

} // namespace cordite
