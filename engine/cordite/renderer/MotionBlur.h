#pragma once

/// @file
/// Motion blur: what moved while the shutter of the camera was open is smeared along the
/// way it went - as worked out from the motion vectors of the picture (MotionVectors.h),
/// after its depth of field and before the rest of it is finished (MotionBlurPass), so
/// that bloom and exposure see the blur.
///
/// The arithmetic is the reconstruction filter of M. McGuire, P. Hennessy, M. Bukowski
/// and B. Osman ("A Reconstruction Filter for Plausible Motion Blur", 2012):
///
/// 1. every pixel is to be blurred along blurOf() its motion: half of how far it went
///    while the shutter was open, either way of where it is, and no further than
///    motionBlurRadius() pixels;
/// 2. the picture is cut into tiles that many pixels a side, and each tile finds the
///    longest blur of its pixels, then of the tiles around it: no pixel can be reached by
///    a blur from further away than that;
/// 3. each pixel takes points along the longest blur of its tile, either way, and blends
///    in those that reach it (motionBlurWeight()): what is in front of it and moves over
///    it, what is behind it while it moves itself - and so lets that be seen - and what
///    moves along with it.
///
/// A pixel near which nothing moves by half a pixel or more keeps its color to the bit.
/// The points are taken a little apart from pixel to pixel (motionBlurNoise()), which
/// turns the steps a long blur would show between them into a fine grain.
///
/// @code
/// const auto radius = static_cast<float>(motionBlurRadius(size.y));
/// const Vec2 blur = blurOf(motion, Vec2(size), look.motionBlur, radius);
/// // How much of a point three pixels along, blurred four either way and ten metres away,
/// // a pixel blurred six either way and two metres away takes in:
/// const float weight = motionBlurWeight({.distance = 3.0f, .speed = 4.0f, .depth = 10.0f},
///                                       {.speed = 6.0f, .depth = 2.0f});
/// @endcode
///
/// It is all written down twice, here and in shaders/common/MotionBlur.slang; tests hold
/// the pictures of the one against what the other says.

#include "cordite/math/Math.h"

#include <cstdint>

namespace cordite {

/// How far a pixel is blurred at the most, either way along its motion: one part in this
/// many of the height of the picture.
inline constexpr uint32_t kMotionBlurRadiusDivisor = 40;

/// How much nearer one point has to be than another to be wholly in front of it, as a
/// share of the depth of the nearer: closer than that, the two are taken to be the same
/// surface as much as they are close.
inline constexpr float kMotionBlurDepthSoftness = 0.02f;

/// How long, in pixels, the longest blur of a picture `height` pixels high is either way:
/// the height divided by kMotionBlurRadiusDivisor, rounded up - one pixel at the least.
/// It is also the size of the tiles the blur is planned in.
[[nodiscard]] uint32_t motionBlurRadius(uint32_t height);

/// What a pixel that moved `motion` - in parts of a picture `size` pixels large - is
/// blurred along, either way of where it is, in pixels: `amount` of how far it went, as
/// much as the shutter was open, halved, and no longer than `radius`. What is no number
/// is not blurred.
[[nodiscard]] Vec2 blurOf(Vec2 motion, Vec2 size, float amount, float radius);

/// How surely what is `depth` metres away is in front of what is `otherDepth` away: 1
/// where it is nearer, or as near, down to 0 where it is kMotionBlurDepthSoftness of the
/// nearer one further away.
[[nodiscard]] float inFrontOf(float depth, float otherDepth);

/// What a blur `speed` pixels long either way gives a point `distance` pixels away: all
/// of itself at no distance, less in a straight line, nothing at `speed`.
[[nodiscard]] float motionCone(float distance, float speed);

/// The same, all of itself up to nearly `speed` and nothing just beyond.
[[nodiscard]] float motionCylinder(float distance, float speed);

/// A point of the picture as motionBlurWeight() sees it.
struct MotionBlurPoint {
    /// How far it is from the pixel that is blurred, in pixels.
    float distance = 0.0f;
    /// How long its blur is either way, in pixels: half a pixel at the least.
    float speed = 0.5f;
    /// How far away what it shows is, in metres along the view.
    float depth = 0.0f;
};

/// How much of the point `sample` is blended into the pixel `pixel` (whose distance is
/// not used): as much as what is in front moves over what is behind, and as much as two
/// that blur both reach each other.
[[nodiscard]] float motionBlurWeight(const MotionBlurPoint& sample, const MotionBlurPoint& pixel);

/// Where along its tile's longest blur the point `index` of `count` of the pixel at `pixel`
/// is taken: from -1, the one end, to 1, the other - spread evenly, all moved along by
/// the pixel's noise (motionBlurNoise()) by up to half the step between two.
[[nodiscard]] float motionBlurSampleAt(uint32_t index, uint32_t count, UVec2 pixel);

/// A number from 0 to 1 for the pixel at `pixel` that looks like noise and has none of
/// the patterns a picture would show ("interleaved gradient noise", J. Jimenez, 2014).
[[nodiscard]] float motionBlurNoise(UVec2 pixel);

} // namespace cordite
