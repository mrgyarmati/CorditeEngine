#pragma once

/// @file
/// A point of view, in the form the renderer needs it: matrices instead of settings.
///
/// @code
/// const Entity camera = findPrimaryCamera(world);
/// if (!camera.isNull()) {
///     // What the game sees through it, shaken where it shakes:
///     const CameraView view = makeCameraView(world, camera, static_cast<float>(width) / static_cast<float>(height));
///     const Ray ray = screenRay(view, cursorPixel, Vec2(width, height));   // for picking
/// }
/// // A camera that is no entity's, such as an editor's own:
/// const CameraView other = makeCameraView(cameraToWorld, CameraComponent{}, 16.0f / 9.0f);
/// @endcode
///
/// View space is right-handed with the camera looking along -Z. The projection maps
/// depth to [0, 1] reversed: 1 at the near plane and 0 at the far plane, which keeps
/// floating-point precision where it is needed.

#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include <optional>

namespace cordite {

struct CameraView {
    /// World space to view space.
    Mat4 view{1.0f};
    /// View space to clip space.
    Mat4 projection{1.0f};
    Mat4 viewProjection{1.0f};
    Mat4 inverseViewProjection{1.0f};
    /// The camera's position and viewing direction in world space.
    Vec3 position{0.0f};
    Vec3 forward = kWorldForward;
    /// Without perspective: everything is seen along `forward`, wherever it is.
    bool orthographic = false;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
};

/// Builds the view of a camera placed by `cameraToWorld`. Scale in that matrix is
/// ignored: a camera is a position and an orientation. `aspectRatio` is width over
/// height of the target. Settings that would produce a degenerate projection (planes
/// in the wrong order, a field of view of zero) are clamped to the nearest usable value.
[[nodiscard]] CameraView makeCameraView(const Mat4& cameraToWorld, const CameraComponent& camera, float aspectRatio);

/// Where the camera of an entity sees from, as makeCameraView takes it: where the entity
/// is in the world, moved and turned in its own space by its CameraShake, when it has one
/// (cameraShakeOffset, animation/CameraShake.h). The shake moves the view alone, and in
/// metres of the world however the entity is scaled; a shake at rest changes nothing, to
/// the bit. The world's transforms are taken as they are.
[[nodiscard]] Mat4 cameraPlacement(const World& world, Entity camera);

/// The view through the Camera of an entity, from cameraPlacement: what a game sees
/// through it. The entity must have a CameraComponent (asserted); one without is seen
/// through a camera as a new CameraComponent is.
[[nodiscard]] CameraView makeCameraView(const World& world, Entity camera, float aspectRatio);

/// Returns the camera a game renders from: the first entity in hierarchy order that is
/// enabled, along with all its ancestors, and has a CameraComponent marked primary. Null
/// when there is none. The world's transforms need not be up to date.
[[nodiscard]] Entity findPrimaryCamera(const World& world);

/// The ray from the camera through a pixel of a target `targetSize` pixels large, with
/// (0, 0) at the top-left corner. It starts on the near plane.
[[nodiscard]] Ray screenRay(const CameraView& view, Vec2 pixel, Vec2 targetSize);

/// Where a world-space point appears on a target `targetSize` pixels large, or nothing
/// when the point is behind the camera.
[[nodiscard]] std::optional<Vec2> worldToScreen(const CameraView& view, const Vec3& point, Vec2 targetSize);

} // namespace cordite
