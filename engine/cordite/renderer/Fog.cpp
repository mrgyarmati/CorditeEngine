#include "cordite/renderer/Fog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cordite {
namespace {

/// The largest numbers a fog is given: more than any scene asks for, and small enough
/// that nothing worked out from them stops being a number.
constexpr float kMostDensity = 100.0f;
constexpr float kMostFalloff = 100.0f;
constexpr float kMostDistance = 1.0e6f;
constexpr float kMostLight = 65504.0f;
constexpr float kMostSunExponent = 256.0f;
constexpr float kLeastVolumetricDistance = 1.0f;
constexpr float kMostVolumetricDistance = 1000.0f;

/// A line of sight that climbs less than this for every e-th of the density it would
/// take to leave the fog behind does not leave it.
constexpr float kLeastThinning = 1.0e-6f;

float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

Vec3 lightOr(const Vec3& value, const Vec3& fallback) {
    return isFinite(value) ? glm::clamp(value, Vec3(0.0f), Vec3(kMostLight)) : fallback;
}

/// The mean of e to the minus x for x from nothing to `span`: (1 - e^-span) / span.
float meanDecayOver(float span) {
    return span > 0.0f ? -std::expm1(-span) / span : 1.0f;
}

/// The mean density of the fog on a straight way from one height to another.
float meanDensityBetween(const FogSettings& fog, float startHeight, float endHeight) {
    // From here on heights are above where the fog is densest.
    const float start = startHeight - fog.height;
    const float end = endHeight - fog.height;
    const float lowest = std::min(start, end);
    const float highest = std::max(start, end);

    // The part of the way that is below that height, where the fog is the same
    // everywhere, ...
    float below = 0.0f;
    if (lowest < 0.0f) {
        below = highest > 0.0f ? -lowest / (highest - lowest) : 1.0f;
    }
    // ... and the mean of what it thins out to on the rest: every e-th it loses takes
    // as long as the one before.
    const float lowestAbove = std::max(lowest, 0.0f);
    const float highestAbove = std::max(highest, 0.0f);
    const float above =
        std::exp(-fog.heightFalloff * lowestAbove) * meanDecayOver(fog.heightFalloff * (highestAbove - lowestAbove));
    return fog.density * (below + (1.0f - below) * above);
}

/// What part of what is behind a fog of the given optical depth is fog.
float amountOf(const FogSettings& fog, float opticalDepth) {
    return std::min(-std::expm1(-opticalDepth), fog.maxOpacity);
}

} // namespace

FogSettings sanitized(const FogSettings& fog) {
    FogSettings result;
    result.color = lightOr(fog.color, Vec3(0.0f));
    result.density = std::clamp(finiteOr(fog.density, 0.0f), 0.0f, kMostDensity);
    result.height = std::clamp(finiteOr(fog.height, 0.0f), -kMostDistance, kMostDistance);
    result.heightFalloff = std::clamp(finiteOr(fog.heightFalloff, 0.0f), 0.0f, kMostFalloff);
    result.startDistance = std::clamp(finiteOr(fog.startDistance, 0.0f), 0.0f, kMostDistance);
    result.maxOpacity = std::clamp(finiteOr(fog.maxOpacity, 1.0f), 0.0f, 1.0f);
    result.sunColor = lightOr(fog.sunColor, Vec3(0.0f));
    result.towardsSun = isFinite(fog.towardsSun) ? normalizeOr(fog.towardsSun, kWorldUp) : kWorldUp;
    result.sunExponent = std::clamp(finiteOr(fog.sunExponent, 8.0f), 1.0f, kMostSunExponent);
    result.volumetric = fog.volumetric;
    result.scattering = isFinite(fog.scattering) ? glm::clamp(fog.scattering, Vec3(0.0f), Vec3(1.0f)) : Vec3(1.0f);
    result.anisotropy = std::clamp(finiteOr(fog.anisotropy, 0.2f), -kMostFogAnisotropy, kMostFogAnisotropy);
    result.volumetricDistance =
        std::clamp(finiteOr(fog.volumetricDistance, 50.0f), kLeastVolumetricDistance, kMostVolumetricDistance);
    return result;
}

float fogOpticalDepth(const FogSettings& fog, const Vec3& from, const Vec3& to) {
    if (!fog.isEnabled()) {
        return 0.0f;
    }
    const Vec3 way = to - from;
    const float length = glm::length(way);
    // Also when the length is no number.
    if (!(length > fog.startDistance)) {
        return 0.0f;
    }
    const float startHeight = from.y + way.y * (fog.startDistance / length);
    return meanDensityBetween(fog, startHeight, to.y) * (length - fog.startDistance);
}

float fogOpticalDepthTowards(const FogSettings& fog, const Vec3& from, const Vec3& direction) {
    if (!fog.isEnabled()) {
        return 0.0f;
    }
    // By how many e-ths the density falls with every metre along the line. Where it
    // does not fall - the line is level or goes down, or the fog does not thin out -
    // the line never comes out of the fog.
    const float thinning = fog.heightFalloff * direction.y;
    if (!(thinning > kLeastThinning)) {
        return std::numeric_limits<float>::infinity();
    }
    // What is below the height the fog is densest up to is crossed first, at its full
    // density; then the fog thins out for good.
    const float start = from.y + direction.y * fog.startDistance - fog.height;
    const float below = std::max(-start, 0.0f) / direction.y;
    const float above = std::exp(-fog.heightFalloff * std::max(start, 0.0f)) / thinning;
    return fog.density * (below + above);
}

float fogAmount(const FogSettings& fog, const Vec3& from, const Vec3& to) {
    return fog.isEnabled() ? amountOf(fog, fogOpticalDepth(fog, from, to)) : 0.0f;
}

float fogAmountTowards(const FogSettings& fog, const Vec3& from, const Vec3& direction) {
    return fog.isEnabled() ? amountOf(fog, fogOpticalDepthTowards(fog, from, direction)) : 0.0f;
}

Vec3 fogColorAlong(const FogSettings& fog, const Vec3& direction) {
    const float towardsSun = std::max(glm::dot(direction, fog.towardsSun), 0.0f);
    return fog.color + fog.sunColor * std::pow(towardsSun, std::max(fog.sunExponent, 1.0f));
}

Vec3 applyFog(const FogSettings& fog, const Vec3& light, const Vec3& from, const Vec3& to) {
    if (!fog.isEnabled()) {
        return light;
    }
    const Vec3 direction = normalizeOr(to - from, kWorldForward);
    return glm::mix(light, fogColorAlong(fog, direction), fogAmount(fog, from, to));
}

Vec3 applyFogTowards(const FogSettings& fog, const Vec3& light, const Vec3& from, const Vec3& direction) {
    if (!fog.isEnabled()) {
        return light;
    }
    return glm::mix(light, fogColorAlong(fog, direction), fogAmountTowards(fog, from, direction));
}

} // namespace cordite
