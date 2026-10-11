#include "cordite/editor/EditorCamera.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// Looking straight up or down would make "left" and "right" meaningless.
constexpr float kMaxPitchDegrees = 89.0f;

/// Metres slid per pixel of pointer movement, and metres travelled per wheel notch, as
/// fractions of the flying speed.
constexpr float kPanSpeedFactor = 0.002f;
constexpr float kWheelSpeedFactor = 0.2f;

float wrapDegrees(float degrees) {
    const float wrapped = std::fmod(degrees + 180.0f, 360.0f);
    return wrapped < 0.0f ? wrapped + 180.0f : wrapped - 180.0f;
}

} // namespace

Quat EditorCamera::rotation() const {
    return glm::angleAxis(toRadians(m_yawDegrees), kWorldUp) * glm::angleAxis(toRadians(m_pitchDegrees), kWorldRight);
}

Vec3 EditorCamera::forward() const {
    return rotation() * kWorldForward;
}

Mat4 EditorCamera::cameraToWorld() const {
    return glm::translate(Mat4(1.0f), m_position) * glm::mat4_cast(rotation());
}

CameraView EditorCamera::view(float aspectRatio) const {
    return makeCameraView(cameraToWorld(), m_lens, aspectRatio);
}

void EditorCamera::setOrientation(float yawDegrees, float pitchDegrees) {
    m_yawDegrees = wrapDegrees(yawDegrees);
    m_pitchDegrees = std::clamp(pitchDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
}

void EditorCamera::lookAt(const Vec3& position, const Vec3& target) {
    m_position = position;
    const Vec3 direction = normalizeOr(target - position, kWorldForward);

    // Inverse of forward(): yaw turns -Z towards -X, pitch lifts it towards +Y.
    const float yaw = toDegrees(std::atan2(-direction.x, -direction.z));
    const float pitch = toDegrees(std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
    setOrientation(yaw, pitch);
}

void EditorCamera::frame(const Vec3& center, float radius) {
    // The distance at which a sphere just fits the vertical field of view, with a margin.
    const float halfAngle = 0.5f * toRadians(std::clamp(m_lens.verticalFovDegrees, 1.0f, 179.0f));
    const float distance = std::max(radius, 0.01f) / std::sin(halfAngle) * 1.2f;
    m_position = center - forward() * distance;
}

void EditorCamera::update(const Input& input, float deltaSeconds, bool viewportHovered) {
    // A movement starts with a press over the viewport and lasts until the release.
    if (viewportHovered && input.wasMouseButtonPressed(MouseButton::Right)) {
        m_looking = true;
    }
    if (viewportHovered && input.wasMouseButtonPressed(MouseButton::Middle)) {
        m_panning = true;
    }
    m_looking = m_looking && input.isMouseButtonDown(MouseButton::Right);
    m_panning = m_panning && input.isMouseButtonDown(MouseButton::Middle);

    const Vec2 pointerDelta = input.mouseDelta();
    if (m_looking) {
        setOrientation(m_yawDegrees - pointerDelta.x * lookSensitivity,
                       m_pitchDegrees - pointerDelta.y * lookSensitivity);
    }

    const Quat orientation = rotation();
    const Vec3 right = orientation * kWorldRight;
    const Vec3 up = orientation * kWorldUp;
    const Vec3 ahead = orientation * kWorldForward;

    if (m_looking) {
        Vec3 direction(0.0f);
        direction += input.isKeyDown(Key::W) ? ahead : Vec3(0.0f);
        direction -= input.isKeyDown(Key::S) ? ahead : Vec3(0.0f);
        direction += input.isKeyDown(Key::D) ? right : Vec3(0.0f);
        direction -= input.isKeyDown(Key::A) ? right : Vec3(0.0f);
        direction += input.isKeyDown(Key::E) ? kWorldUp : Vec3(0.0f);
        direction -= input.isKeyDown(Key::Q) ? kWorldUp : Vec3(0.0f);

        const bool fast = input.isKeyDown(Key::LeftShift) || input.isKeyDown(Key::RightShift);
        const float speed = moveSpeed * (fast ? fastMultiplier : 1.0f);
        m_position += normalizeOr(direction, Vec3(0.0f)) * speed * deltaSeconds;
    }

    if (m_panning) {
        // The scene follows the pointer: dragging right slides the camera left.
        const float metresPerPixel = moveSpeed * kPanSpeedFactor;
        m_position += (-right * pointerDelta.x + up * pointerDelta.y) * metresPerPixel;
    }

    if (viewportHovered && input.scrollDelta().y != 0.0f) {
        m_position += ahead * input.scrollDelta().y * moveSpeed * kWheelSpeedFactor;
    }
}

} // namespace cordite
