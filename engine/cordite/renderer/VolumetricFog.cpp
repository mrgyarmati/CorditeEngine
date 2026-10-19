#include "cordite/renderer/VolumetricFog.h"

#include "cordite/renderer/RectLights.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// How much of a local light is left at a distance whose square is `distanceSquared`:
/// rangeWindow() of shaders/common/Lights.slang.
float rangeWindow(float distanceSquared, float range) {
    const float ratio = distanceSquared / (range * range);
    const float window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
    return window * window;
}

/// How bright a point or spot light is at a distance whose square is `distanceSquared`:
/// distanceFalloff() of shaders/common/Lights.slang.
float distanceFalloff(float distanceSquared, float range) {
    return rangeWindow(distanceSquared, range) / std::max(distanceSquared, 1.0e-4f);
}

/// How bright the face of a rect light is, against its color, times how much of everything
/// around `position` the face fills: what reaches a point of the fog of it, within its range
/// window.
float rectLightReaching(const GpuLight& light, const Vec3& position, float distanceSquared) {
    const RectLightShape shape = rectShapeOf(light);
    const float area = 4.0f * glm::length(shape.right) * glm::length(shape.up);
    return rangeWindow(distanceSquared, light.range) * rectLightSolidAngle(shape, position) / std::max(area, 1.0e-8f);
}

/// How bright a spot light is along `travels`, the unit vector its light goes along:
/// coneFalloff() of shaders/common/Lights.slang.
float coneFalloff(const GpuLight& light, const Vec3& travels) {
    const float amount =
        std::clamp(glm::dot(travels, light.direction) * light.spotScale + light.spotOffset, 0.0f, 1.0f);
    return amount * amount;
}

GpuLightType typeOf(const GpuLight& light) {
    return static_cast<GpuLightType>(light.typeAndChannels & 0xFFu);
}

/// How much of what is `distance` along `sight` is fog; for an infinite distance, that of
/// the sky.
float amountAt(const FogSettings& fog, const FogSight& sight, float distance) {
    return std::isinf(distance) ? fogAmountTowards(fog, sight.viewer, sight.direction)
                                : fogAmount(fog, sight.viewer, sight.viewer + sight.direction * distance);
}

} // namespace

FogVolumeGrid fogVolumeGridFor(UVec2 targetSize, ScreenEffectsQuality quality) {
    if (quality == ScreenEffectsQuality::Off || targetSize.x == 0 || targetSize.y == 0) {
        return {};
    }
    const ScreenEffectsQualitySettings numbers = screenEffectsSettingsFor(quality);
    const uint32_t mostCells = std::max(numbers.fogVolumeMaxCells, 1u);
    // Square boxes, as near the size of a tile as the longer side of the picture allows.
    const auto longest = static_cast<float>(std::max(targetSize.x, targetSize.y));
    const float boxSize =
        std::max(static_cast<float>(std::max(numbers.fogVolumeTile, 1u)), longest / static_cast<float>(mostCells));
    const auto cellsAlong = [&](uint32_t pixels) {
        return std::clamp(static_cast<uint32_t>(std::ceil(static_cast<float>(pixels) / boxSize)), 1u, mostCells);
    };
    return {.columns = cellsAlong(targetSize.x), .rows = cellsAlong(targetSize.y), .slices = numbers.fogVolumeSlices};
}

float fogSliceDepth(float share, float distance) {
    // Also when the share is no number.
    const float clamped = share > 0.0f ? std::min(share, 1.0f) : 0.0f;
    return distance * clamped * clamped;
}

float fogSliceShare(float depth, float distance) {
    if (!(distance > 0.0f)) {
        return 1.0f;
    }
    const float ratio = depth / distance;
    return ratio > 0.0f ? std::sqrt(std::min(ratio, 1.0f)) : 0.0f;
}

float henyeyGreenstein(float cosine, float anisotropy) {
    const float g = std::isfinite(anisotropy) ? std::clamp(anisotropy, -kMostFogAnisotropy, kMostFogAnisotropy) : 0.0f;
    const float c = std::isfinite(cosine) ? std::clamp(cosine, -1.0f, 1.0f) : 0.0f;
    // Never less than (1 - |g|) squared: never nothing.
    const float denominator = 1.0f + g * g - 2.0f * g * c;
    return (1.0f - g * g) / (4.0f * kPi * denominator * std::sqrt(denominator));
}

Vec3 lightInFog(const GpuLight& light, const Vec3& position, const Vec3& toViewer, float anisotropy) {
    const GpuLightType type = typeOf(light);
    if (type == GpuLightType::Directional) {
        return light.fogColor * henyeyGreenstein(glm::dot(light.direction, toViewer), anisotropy);
    }
    const Vec3 offset = position - light.position;
    const float distanceSquared = glm::dot(offset, offset);
    if (!(distanceSquared < light.range * light.range)) {
        return Vec3(0.0f);
    }
    const Vec3 travels = offset / std::sqrt(std::max(distanceSquared, 1.0e-8f));
    float brightness = 0.0f;
    if (type == GpuLightType::Rect) {
        brightness = rectLightReaching(light, position, distanceSquared);
    } else {
        brightness = distanceFalloff(distanceSquared, light.range);
        if (type == GpuLightType::Spot) {
            brightness *= coneFalloff(light, travels);
        }
    }
    return light.fogColor * (brightness * henyeyGreenstein(glm::dot(travels, toViewer), anisotropy));
}

Vec3 fogLightAlong(const FogSettings& fog, std::span<const GpuLight> lights, const FogSight& sight) {
    const float cosine = glm::dot(sight.direction, sight.forward);
    if (!fog.isLitByLights() || sight.slices == 0 || !(cosine > 0.0f)) {
        return Vec3(0.0f);
    }
    const Vec3 toViewer = -sight.direction;
    const float reach = fog.volumetricDistance;
    const auto slices = static_cast<float>(sight.slices);
    // How far in front of the camera what is seen is, and in which slice, from the camera.
    const float depth = sight.distance * cosine;
    const float place = fogSliceShare(depth, reach) * slices;

    Vec3 before(0.0f);
    float amountBefore = 0.0f;
    for (uint32_t slice = 0; slice < sight.slices; ++slice) {
        const auto first = static_cast<float>(slice);
        const float middle = fogSliceDepth((first + 0.5f) / slices, reach) / cosine;
        const float end = fogSliceDepth((first + 1.0f) / slices, reach) / cosine;
        const Vec3 point = sight.viewer + sight.direction * middle;
        Vec3 thrown(0.0f);
        for (const GpuLight& light : lights) {
            thrown += lightInFog(light, point, toViewer, fog.anisotropy);
        }
        const float amount = amountAt(fog, sight, end);
        const Vec3 after = before + fog.scattering * thrown * std::max(amount - amountBefore, 0.0f);
        if (place < first + 1.0f) {
            // What is seen is inside this slice: between the sums at its ends, as far as it is in.
            return glm::mix(before, after, std::clamp(place - first, 0.0f, 1.0f));
        }
        before = after;
        amountBefore = amount;
    }

    // Beyond the grid only the directional lights light the fog, as if nothing stood in
    // their way.
    if (depth > reach) {
        Vec3 sunlight(0.0f);
        for (const GpuLight& light : lights) {
            if (typeOf(light) == GpuLightType::Directional) {
                sunlight += lightInFog(light, sight.viewer, toViewer, fog.anisotropy);
            }
        }
        const float beyond = amountAt(fog, sight, sight.distance) - amountAt(fog, sight, reach / cosine);
        before += fog.scattering * sunlight * std::max(beyond, 0.0f);
    }
    return before;
}

} // namespace cordite
