#pragma once

/// @file
/// The camera through which the editor viewport looks at the scene.
///
/// It is not an entity: it belongs to the editor, is never saved with the scene and is
/// steered by the pointer and the keyboard.
///
/// @code
/// EditorCamera camera;
/// camera.lookAt({6.0f, 4.0f, 8.0f}, {0.0f, 0.5f, 0.0f});
///
/// camera.update(engine.input(), engine.deltaSeconds(), viewportIsHovered);
/// const CameraView view = camera.view(aspectRatio);
/// @endcode
///
/// Controls, active while the pointer is over the viewport:
/// - hold the right button and move the pointer to look around; with it held, W A S D
///   move forwards, left, backwards and right, E and Q up and down, Shift moves faster;
/// - hold the middle button and move the pointer to slide sideways and up or down;
/// - turn the wheel to move forwards and backwards.

#include "cordite/math/Math.h"
#include "cordite/platform/Input.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/world/Components.h"

namespace cordite {

class EditorCamera {
public:
    /// Reads the pointer and the keyboard and moves the camera. `viewportHovered` says
    /// whether the pointer is over the viewport: a movement can only begin there, but one
    /// that has begun continues wherever the pointer goes until the button is released.
    void update(const Input& input, float deltaSeconds, bool viewportHovered);

    /// True while a button that steers the camera is held. The viewport hides and locks
    /// the pointer for that time.
    [[nodiscard]] bool isNavigating() const { return m_looking || m_panning; }

    /// Puts the camera at `position`, looking at `target`.
    void lookAt(const Vec3& position, const Vec3& target);

    /// Moves the camera backwards or forwards along its viewing direction until a sphere
    /// around `center` fills the view.
    void frame(const Vec3& center, float radius);

    [[nodiscard]] const Vec3& position() const { return m_position; }

    void setPosition(const Vec3& position) { m_position = position; }

    /// Rotation about the world's up axis; zero looks along -Z and positive turns left.
    [[nodiscard]] float yawDegrees() const { return m_yawDegrees; }

    /// Rotation up (positive) or down from the horizon, limited to just short of vertical.
    [[nodiscard]] float pitchDegrees() const { return m_pitchDegrees; }

    void setOrientation(float yawDegrees, float pitchDegrees);

    [[nodiscard]] Vec3 forward() const;
    [[nodiscard]] Mat4 cameraToWorld() const;

    /// The view for a viewport whose width divided by its height is `aspectRatio`.
    [[nodiscard]] CameraView view(float aspectRatio) const;

    /// Lens settings; the `primary` flag has no meaning here.
    [[nodiscard]] CameraComponent& lens() { return m_lens; }

    [[nodiscard]] const CameraComponent& lens() const { return m_lens; }

    /// Metres per second while flying; Shift multiplies it.
    float moveSpeed = 5.0f;
    float fastMultiplier = 4.0f;
    /// Degrees turned per pixel of pointer movement.
    float lookSensitivity = 0.15f;

private:
    [[nodiscard]] Quat rotation() const;

    Vec3 m_position{6.0f, 4.0f, 8.0f};
    float m_yawDegrees = 36.87f;
    float m_pitchDegrees = -19.3f;
    CameraComponent m_lens{.verticalFovDegrees = 60.0f, .nearPlane = 0.05f, .farPlane = 2000.0f};
    bool m_looking = false;
    bool m_panning = false;
};

} // namespace cordite
