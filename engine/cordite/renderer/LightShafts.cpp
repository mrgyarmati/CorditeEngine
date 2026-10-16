#include "cordite/renderer/LightShafts.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// Where the sun counts as in front of a view: a direction level with the view's plane
/// has no point in the picture.
constexpr float kMinFacing = 1.0e-4f;

} // namespace

Vec3 lightShaftSource(const Vec3& skyLight, const Vec3& sunColor, float cosToSun, float threshold) {
    if (!(cosToSun > 0.0f)) {
        return Vec3(0.0f);
    }
    const float weight = std::pow(std::min(cosToSun, 1.0f), kLightShaftConeExponent);
    Vec3 source = (glm::max(sunColor, Vec3(0.0f)) + glm::max(skyLight - Vec3(threshold), Vec3(0.0f))) * weight;
    for (int channel = 0; channel < 3; ++channel) {
        // What is no number gives nothing; what is too bright gives the most there is.
        source[channel] = std::isnan(source[channel]) ? 0.0f : std::min(source[channel], kMaxLightShaftSource);
    }
    return source;
}

float lightShaftFade(const CameraView& view, const Vec3& towardsSun) {
    if (view.orthographic) {
        return 0.0f;
    }
    const float facing = glm::dot(view.forward, towardsSun);
    return std::isfinite(facing) ? std::clamp(facing * 4.0f, 0.0f, 1.0f) : 0.0f;
}

std::optional<Vec2> sunScreenPoint(const CameraView& view, const Vec3& towardsSun) {
    if (view.orthographic) {
        return std::nullopt;
    }
    // A direction, as a point infinitely far away: its fourth number is nothing.
    const Vec4 clip = view.viewProjection * Vec4(towardsSun, 0.0f);
    if (!(clip.w > kMinFacing) || !std::isfinite(clip.x) || !std::isfinite(clip.y)) {
        return std::nullopt;
    }
    // Clip space has y up; the picture counts its rows from the top.
    return Vec2(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
}

} // namespace cordite
