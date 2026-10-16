#pragma once

/// @file
/// Light shafts: the light of the sun, scattered towards the viewer along rays from where
/// the sun is in the picture, and blocked by what stands in its way - the arithmetic of
/// it. shaders/common/LightShafts.slang has the same; LightShaftPass draws them.
///
/// @code
/// const std::optional<SceneSun>& sun = lights.sun();
/// const std::optional<Vec2> point = sunScreenPoint(view, sun->towards);   // none behind the camera
/// const float fade = lightShaftFade(view, sun->towards);
/// // What a pixel of the sky, looking along `direction`, gives to the shafts:
/// const Vec3 source = lightShaftSource(skyLight, sun->color, glm::dot(direction, sun->towards), 1.0f);
/// @endcode
///
/// Where nothing was drawn, each pixel gives the shafts the sun's own light and what of
/// the sky's light is brighter than a threshold, as much as the pixel looks towards the
/// sun; everything else gives nothing, and is what the shafts are blocked by. That is
/// blurred along the way to the sun's point in the picture, and the blur is added to the
/// light of the scene.

#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"

#include <optional>

namespace cordite {

/// How strongly a pixel of the sky feeds the shafts by its direction: the cosine of its
/// angle to the sun, to this power - half at about twelve degrees from the sun.
inline constexpr float kLightShaftConeExponent = 32.0f;

/// The most light a pixel of the sky gives to the shafts, in each channel. A sun in an
/// image of the sky is tens of thousands of times as bright as white; spread over the
/// picture, it would be all that is seen.
inline constexpr float kMaxLightShaftSource = 4.0f;

/// What a pixel of the sky - where nothing was drawn - gives to the light shafts: the
/// sun's color, and the sky's light beyond `threshold` in each channel, as much as the
/// pixel looks towards the sun (`cosToSun`, the cosine of the angle between the two), and
/// at most kMaxLightShaftSource in each channel. Nothing for a pixel that looks away from
/// the sun; numbers that are none give nothing.
[[nodiscard]] Vec3 lightShaftSource(const Vec3& skyLight, const Vec3& sunColor, float cosToSun, float threshold);

/// How much of its light shafts a view has by where its sun is: all of them while the
/// sun is within about seventy-five degrees of where the view looks, fewer towards ninety,
/// none behind - and none in a view without perspective, in which every ray would be
/// parallel to every other.
[[nodiscard]] float lightShaftFade(const CameraView& view, const Vec3& towardsSun);

/// Where the sun is in the picture of `view`, from (0, 0) at its top-left corner to (1, 1)
/// at its bottom-right - outside that when it is out of the picture - or nothing when it
/// is behind the view or level with it, or the view has no perspective.
[[nodiscard]] std::optional<Vec2> sunScreenPoint(const CameraView& view, const Vec3& towardsSun);

} // namespace cordite
