#pragma once

/// @file
/// What the shake of a camera does: how it moves the view at a moment, and how it goes on
/// while time passes.
///
/// @code
/// // Something happens near the camera: it is shaken, and more so the harder it was.
/// if (CameraShakeComponent* shake = world.tryGet<CameraShakeComponent>(camera)) {
///     shake->trauma = std::min(shake->trauma + 0.5f, 1.0f);
/// }
///
/// // Every frame of a game: the shakes go on, and die down.
/// shakeCameras(world, frameSeconds);
///
/// // Where the view is seen from: the camera's place, moved by its shake.
/// const Mat4 eye = placementWithoutScale * cameraShakeOffset(shake).toMatrix();
/// @endcode
///
/// A shake is smooth noise of its phase - Perlin noise, another row of it for each of the
/// six ways a camera can move - times the furthest the camera may go that way, times its
/// trauma squared (after Squirrel Eiserloh, "Juicing your cameras with math", GDC 2016).
/// Noise is not a wobble: it never repeats within a period, and the six ways do not
/// keep time with each other. The view of a camera is made with it (makeCameraView of an
/// entity, renderer/CameraView.h), so a game's view and a scene capture shake alike.

#include "cordite/animation/AnimationComponents.h"
#include "cordite/math/Transform.h"
#include "cordite/world/World.h"

namespace cordite {

/// How a shake moves its camera now, in the camera's own space: along and about its axes,
/// as far as `maxOffset` and `maxRotation` say at the most, by trauma squared, by the
/// noise at `phase`. Exactly no move and no turn - the Transform a new one is - for a
/// trauma of nothing. A trauma beyond 1 is 1, and what is no number counts as nothing.
[[nodiscard]] Transform cameraShakeOffset(const CameraShakeComponent& shake);

/// Lets `deltaSeconds` pass for the shake of every camera of a world that is enabled,
/// with everything above it: its trauma falls by its decay, to no lower than nothing, and
/// its phase moves on by its frequency, round within kCameraShakePeriod. Less than no
/// time, and a time that is no number, is no time: nothing is written then.
void shakeCameras(World& world, float deltaSeconds);

} // namespace cordite
