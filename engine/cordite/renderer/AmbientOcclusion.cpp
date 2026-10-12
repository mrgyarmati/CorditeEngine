#include "cordite/renderer/AmbientOcclusion.h"

#include <algorithm>
#include <cmath>

namespace cordite {

SliceNormal projectIntoSlice(const Vec3& normal, const Vec3& toViewer, const Vec3& sliceDirection) {
    // The slice is the plane of `toViewer` and `sliceDirection`; `axis` stands at a
    // right angle to it, and `across` lies in it at a right angle to `toViewer`.
    const Vec3 axis = normalizeOr(glm::cross(sliceDirection, toViewer), Vec3(0.0f));
    const Vec3 across = sliceDirection - toViewer * glm::dot(sliceDirection, toViewer);
    const Vec3 shadow = normal - axis * glm::dot(normal, axis);
    const float length = glm::length(shadow);
    if (length <= kEpsilon) {
        return {};
    }
    const float cosine = std::clamp(glm::dot(shadow, toViewer) / length, -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    return {.angle = glm::dot(across, shadow) < 0.0f ? -angle : angle, .length = length};
}

float sliceVisibility(float normalAngle, float horizonBefore, float horizonAfter) {
    // Further round than a right angle from the normal, the surface itself is in the way.
    const float before = normalAngle + std::clamp(horizonBefore - normalAngle, -kHalfPi, kHalfPi);
    const float after = normalAngle + std::clamp(horizonAfter - normalAngle, -kHalfPi, kHalfPi);
    const float cosine = std::cos(normalAngle);
    const float sine = std::sin(normalAngle);
    // The light between the direction to the viewer and a horizon, each direction
    // weighted by the cosine of its angle to the normal and by how much of the half
    // sphere a step of the angle sweeps there.
    const auto arc = [&](float horizon) {
        return (cosine + 2.0f * horizon * sine - std::cos(2.0f * horizon - normalAngle)) * 0.25f;
    };
    return arc(before) + arc(after);
}

float openSliceVisibility(float normalAngle) {
    return std::cos(normalAngle) + normalAngle * std::sin(normalAngle);
}

float riseAboveSurface(const Vec3& towards, const Vec3& normal, const Vec3& toViewer) {
    const float alongNormal = glm::dot(towards, normal);
    const float alongView = glm::dot(towards, toViewer);
    // In the slice, with the direction to the viewer as one axis and the side `towards`
    // is on as the other: the sine and the cosine of the angle between `towards` and
    // the surface's edge on that side, both times the same positive number.
    const float sideways = std::sqrt(std::max(1.0f - alongView * alongView, 0.0f));
    const float sine = alongNormal * sideways;
    const float cosine = glm::dot(normal, toViewer) - alongView * alongNormal;
    if (std::abs(sine) <= kEpsilon * kEpsilon && std::abs(cosine) <= kEpsilon * kEpsilon) {
        return 0.0f;
    }
    return std::atan2(sine, cosine);
}

AmbientOcclusionSettings ambientOcclusionSettingsFor(const PostProcessSettings& look, ScreenEffectsQuality quality) {
    const ScreenEffectsQualitySettings numbers = screenEffectsSettingsFor(quality);
    return {.intensity = quality == ScreenEffectsQuality::Off ? 0.0f : look.ambientOcclusionIntensity,
            .radius = look.ambientOcclusionRadius,
            .bias = toRadians(look.ambientOcclusionBias),
            .divisor = numbers.ambientOcclusionDivisor,
            .slices = numbers.ambientOcclusionSlices,
            .steps = numbers.ambientOcclusionSteps};
}

} // namespace cordite
