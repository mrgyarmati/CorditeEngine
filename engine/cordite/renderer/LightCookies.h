#pragma once

/// @file
/// Light cookies: pictures a light shines through, as through a slide in a projector - a
/// grille, the panes of a window, leaves, clouds. A spot light throws its cookie across its
/// cone; a directional light lays its cookie out over the world, again and again. The
/// colors of a cookie filter the light: white lets it all through, black none of it.
///
/// @code
/// // Where on a light's cookie a point is - what the shaders work out for every pixel:
/// const std::optional<Vec2> uv = cookieUvOf(gpuLight, surfacePoint);
/// @endcode
///
/// The cookies of a view are the layers of one texture array, kLightCookieSize pixels
/// square with all their mip levels (RenderResources::cookieLayer). Where on a cookie a
/// point is is written down twice, here and in `cookieFilter` of shaders/common/Lights.slang:
/// change both.

#include "cordite/math/Math.h"
#include "cordite/renderer/LightClusters.h"

#include <cstdint>
#include <optional>

namespace cordite {

/// The size of a cookie in the cookie array, in pixels, across and down: what a cookie of
/// another size is brought to.
inline constexpr uint32_t kLightCookieSize = 256;
/// The most cookies a renderer holds at once: the layers of the cookie array.
inline constexpr uint32_t kMaxLightCookies = 16;
/// The widest angle from its axis to which a spot light throws its cookie, in radians: 85
/// degrees. A picture cannot be spread over half of everything.
inline constexpr float kMaxCookieConeAngle = 1.4835298f;
/// The smallest angle and size a cookie is laid out at, in radians and in metres.
inline constexpr float kMinCookieConeAngle = 0.01f;
inline constexpr float kMinCookieSize = 0.01f;

/// What GpuLight::cookieRight is for a light: its right at right angles to the way it
/// shines, divided by the tangent of a spot light's outer cone - at most
/// kMaxCookieConeAngle - or by the size of a directional light's cookie. Nothing for a
/// light without a cookie, and for a point light.
[[nodiscard]] Vec3 cookieAxisOf(const SceneLight& light);

/// Where on its cookie a point is, as the shaders find it, from (0, 0) at the top left of
/// the picture to (1, 1) at its bottom right - seen from behind the light, looking the way
/// it shines. A spot light's picture fills a square around its outer cone; a directional
/// light's begins at the light's position and repeats, so any place counts. Nothing for a
/// point light, and for a point behind a spot light.
[[nodiscard]] std::optional<Vec2> cookieUvOf(const GpuLight& light, const Vec3& point);

} // namespace cordite
