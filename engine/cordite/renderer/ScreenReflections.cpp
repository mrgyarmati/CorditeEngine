#include "cordite/renderer/ScreenReflections.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// How many times the way between the last step in front of what was drawn and the first
/// behind it is halved.
constexpr uint32_t kHalvings = 4;

/// Where the point whose clip coordinates are `clip` is in the picture: x and y from
/// (0, 0) at the top left to (1, 1), z its reversed depth.
Vec3 pictureOf(const Vec4& clip) {
    const Vec3 ndc = Vec3(clip) / clip.w;
    return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, ndc.z};
}

/// The point of the world at `uv` in a picture of `view`, at reversed depth `depth`.
Vec3 worldAt(const CameraView& view, Vec2 uv, float depth) {
    const Vec4 world = view.inverseViewProjection * Vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    return Vec3(world) / world.w;
}

/// How far in front of the camera of `view` a point is, along the way it looks, in metres.
float distanceAlongView(const CameraView& view, const Vec3& point) {
    return glm::dot(point - view.position, view.forward);
}

/// The largest share of the way across the picture from `from` to `to` that is still in
/// the picture: 1 when all of it is.
float shareInPicture(Vec2 from, Vec2 to) {
    float share = 1.0f;
    for (int axis = 0; axis < 2; ++axis) {
        const float along = to[axis] - from[axis];
        if (along > 0.0f) {
            share = std::min(share, (1.0f - from[axis]) / along);
        } else if (along < 0.0f) {
            share = std::min(share, -from[axis] / along);
        }
    }
    return std::max(share, 0.0f);
}

/// How wide a pixel of a picture of `view` that is `targetSize` pixels large is where
/// `point` is, in metres.
float pixelWidthAt(const CameraView& view, UVec2 targetSize, const Vec3& point) {
    const Vec3 at = pictureOf(view.viewProjection * Vec4(point, 1.0f));
    const Vec3 beside = worldAt(view, Vec2(at) + Vec2(1.0f / static_cast<float>(targetSize.x), 0.0f), at.z);
    return glm::distance(beside, point);
}

/// Whether the point `at` of the way - x and y in the picture, z its reversed depth - lies
/// behind what was drawn there, whose depth is `drawn`. Where nothing was drawn nothing is
/// in front of it.
bool liesBehind(const Vec3& at, float drawn) {
    return drawn > 0.0f && drawn > at.z;
}

} // namespace

ScreenReflectionSettings screenReflectionSettingsFor(const PostProcessSettings& look, ScreenEffectsQuality quality) {
    const ScreenEffectsQualitySettings numbers = screenEffectsSettingsFor(quality);
    return {.intensity = quality == ScreenEffectsQuality::Off ? 0.0f : look.reflectionIntensity,
            .maxRoughness = look.reflectionMaxRoughness,
            .maxDistance = look.reflectionMaxDistance,
            .thickness = look.reflectionThickness,
            .divisor = numbers.reflectionDivisor,
            .steps = numbers.reflectionSteps};
}

float screenEdgeFade(Vec2 uv) {
    // What is no number is nowhere in the picture - asked first, since std::min passes over
    // one that comes after a number.
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        return 0.0f;
    }
    const float toBorder = std::min({uv.x, 1.0f - uv.x, uv.y, 1.0f - uv.y});
    if (toBorder <= 0.0f) {
        return 0.0f;
    }
    return std::min(toBorder / kScreenEdgeFade, 1.0f);
}

float reflectionDistanceFade(float travelled, float maxDistance) {
    if (!(maxDistance > 0.0f) || !std::isfinite(maxDistance) || !std::isfinite(travelled)) {
        return 0.0f;
    }
    return std::clamp((maxDistance - travelled) / (0.25f * maxDistance), 0.0f, 1.0f);
}

float reflectionRoughnessFade(float roughness, float maxRoughness) {
    if (!(maxRoughness > 0.0f) || !std::isfinite(maxRoughness) || !std::isfinite(roughness)) {
        return 0.0f;
    }
    return std::clamp((maxRoughness - roughness) / (0.5f * maxRoughness), 0.0f, 1.0f);
}

ScreenReflectionHit traceScreenReflection(const CameraView& view, UVec2 targetSize,
                                          const ScreenReflectionSettings& settings, const Vec3& origin,
                                          const Vec3& normal, const Vec3& direction, const DepthAt& depthAt) {
    if (settings.steps == 0 || !(settings.maxDistance > 0.0f) || !std::isfinite(settings.maxDistance) ||
        targetSize.x == 0 || targetSize.y == 0) {
        return {};
    }
    // The way sets out a pixel's width above the surface. The depth buffer holds a surface
    // as it is at the middles of its pixels; seen at a low angle, it is there up to half a
    // pixel's width nearer than right under the reflection, which would seem to pass
    // behind it.
    const Vec3 setOut = origin + normal * pixelWidthAt(view, targetSize, origin);
    // The ends of the way: there, and maxDistance along the reflection - or, for one that
    // comes towards the camera, where it comes to the near plane. A point is in front of
    // that plane while its reversed depth is at most one: while z is at most w, which
    // changes evenly along the way, as clip coordinates do - also without perspective,
    // where w is one.
    const Vec4 clipStart = view.viewProjection * Vec4(setOut, 1.0f);
    Vec4 clipEnd = view.viewProjection * Vec4(setOut + direction * settings.maxDistance, 1.0f);
    const float marginStart = clipStart.w - clipStart.z;
    const float marginEnd = clipEnd.w - clipEnd.z;
    if (!(marginStart > 0.0f)) {
        return {};
    }
    if (marginEnd < 0.0f) {
        clipEnd = glm::mix(clipStart, clipEnd, marginStart / (marginStart - marginEnd));
    }
    const Vec3 start = pictureOf(clipStart);
    const Vec3 end = pictureOf(clipEnd);

    // Across the picture, x, y and the depth change evenly: only the part in the picture
    // is walked.
    const float inPicture = shareInPicture(Vec2(start), Vec2(end));
    const Vec2 size(static_cast<float>(targetSize.x), static_cast<float>(targetSize.y));
    if (!(glm::length((Vec2(end) - Vec2(start)) * size) * inPicture >= 1.0f)) {
        return {};
    }
    const auto pointAt = [&](float share) { return glm::mix(start, end, share); };

    float before = 0.0f;
    for (uint32_t step = 1; step <= settings.steps; ++step) {
        const float share = inPicture * static_cast<float>(step) / static_cast<float>(settings.steps);
        const Vec3 at = pointAt(share);
        if (liesBehind(at, depthAt(Vec2(at)))) {
            // The way back to the step before is halved, keeping the end that lies behind.
            float inFront = before;
            float behindEnd = share;
            for (uint32_t halving = 0; halving < kHalvings; ++halving) {
                const float middle = 0.5f * (inFront + behindEnd);
                const Vec3 point = pointAt(middle);
                if (liesBehind(point, depthAt(Vec2(point)))) {
                    behindEnd = middle;
                } else {
                    inFront = middle;
                }
            }
            const Vec3 met = pointAt(behindEnd);
            const Vec3 reflected = worldAt(view, Vec2(met), met.z);
            const Vec3 drawn = worldAt(view, Vec2(met), depthAt(Vec2(met)));
            if (distanceAlongView(view, reflected) - distanceAlongView(view, drawn) <= settings.thickness) {
                const float travelled = glm::distance(reflected, origin);
                return {.found = true,
                        .uv = Vec2(met),
                        .travelled = travelled,
                        .confidence =
                            screenEdgeFade(Vec2(met)) * reflectionDistanceFade(travelled, settings.maxDistance)};
            }
        }
        before = share;
    }
    return {};
}

} // namespace cordite
