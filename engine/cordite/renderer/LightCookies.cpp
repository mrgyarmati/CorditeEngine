#include "cordite/renderer/LightCookies.h"

#include <algorithm>
#include <cmath>

namespace cordite {

Vec3 cookieAxisOf(const SceneLight& light) {
    if (light.cookie.isNil() || light.type == GpuLightType::Point) {
        return Vec3(0.0f);
    }
    const Vec3 direction = normalizeOr(light.direction, -kWorldUp);
    // At right angles to the way the light shines, whatever the transform of the entity
    // has done to its axes; a right that runs along it says nothing of which way round.
    const Vec3 right = normalizeOr(light.right - direction * glm::dot(light.right, direction),
                                   normalizeOr(glm::cross(direction, kWorldUp), kWorldRight));
    if (light.type == GpuLightType::Spot) {
        const float angle = std::isfinite(light.outerConeAngle)
                                ? std::clamp(light.outerConeAngle, kMinCookieConeAngle, kMaxCookieConeAngle)
                                : kMaxCookieConeAngle;
        return right / std::tan(angle);
    }
    const float size = std::isfinite(light.cookieSize) ? std::max(light.cookieSize, kMinCookieSize) : kMinCookieSize;
    return right / size;
}

std::optional<Vec2> cookieUvOf(const GpuLight& light, const Vec3& point) {
    const auto type = static_cast<GpuLightType>(light.typeAndChannels & 0xFFu);
    if (type == GpuLightType::Point) {
        return std::nullopt;
    }
    const Vec3 offset = point - light.position;
    // Of the same length as the right: a cookie is not stretched.
    const Vec3 up = glm::cross(light.cookieRight, light.direction);
    const Vec2 across(glm::dot(offset, light.cookieRight), -glm::dot(offset, up));
    if (type == GpuLightType::Directional) {
        return across;
    }
    const float depth = glm::dot(offset, light.direction);
    if (!(depth > 0.0f)) {
        return std::nullopt;
    }
    return Vec2(0.5f) + 0.5f * across / depth;
}

} // namespace cordite
