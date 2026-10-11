#pragma once

/// @file
/// Projection and view matrices following the conventions in Math.h.
///
/// Scene cameras use the reversed-depth variants: the near plane maps to depth 1 and the
/// far plane to depth 0, which spreads floating-point precision evenly over distance
/// and removes most z-fighting. Depth tests therefore use "greater".
///
/// @code
/// const Mat4 projection = perspectiveReverseZ(toRadians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
/// const Mat4 view = lookAt(eye, target, kWorldUp);
/// const Mat4 viewProjection = projection * view;
/// @endcode

#include "cordite/math/Math.h"

#include <cstdint>

namespace cordite {

/// Perspective projection with reversed depth. `verticalFov` is in radians.
[[nodiscard]] Mat4 perspectiveReverseZ(float verticalFov, float aspect, float nearPlane, float farPlane);

/// Perspective projection with reversed depth and no far clipping plane.
[[nodiscard]] Mat4 perspectiveInfiniteReverseZ(float verticalFov, float aspect, float nearPlane);

/// Orthographic projection with reversed depth.
[[nodiscard]] Mat4 orthographicReverseZ(float left, float right, float bottom, float top, float nearPlane,
                                        float farPlane);

/// Orthographic projection with conventional depth (near = 0, far = 1), used for shadow maps.
[[nodiscard]] Mat4 orthographic(float left, float right, float bottom, float top, float nearPlane, float farPlane);

/// Perspective projection with conventional depth (near = 0, far = 1), used for shadow maps.
[[nodiscard]] Mat4 perspective(float verticalFov, float aspect, float nearPlane, float farPlane);

/// World-to-view matrix for a camera at `eye` looking at `target`.
[[nodiscard]] Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up = kWorldUp);

/// Converts a reversed depth-buffer value back to a positive view-space distance.
[[nodiscard]] float linearizeReverseZ(float depth, float nearPlane, float farPlane);

/// Returns element `index` (starting at 1) of the Halton low-discrepancy sequence for a
/// prime `base`, in [0, 1). Used for temporal anti-aliasing jitter.
[[nodiscard]] float halton(uint32_t index, uint32_t base);

/// Sub-pixel jitter in [-0.5, 0.5] pixels for frame `frameIndex`, cycling every
/// `sampleCount` frames.
[[nodiscard]] Vec2 temporalJitter(uint32_t frameIndex, uint32_t sampleCount = 8);

} // namespace cordite
