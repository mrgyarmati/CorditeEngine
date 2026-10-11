#include "cordite/math/Equirectangular.h"

#include <algorithm>
#include <cmath>

namespace cordite {

Vec2 equirectUv(const Vec3& direction) {
    const Vec3 unit = isFinite(direction) ? normalizeOr(direction, kWorldForward) : kWorldForward;
    // How far round from straight ahead, to the right, and how far down from straight up.
    // Straight up and straight down are every way round at once: the middle will do.
    const float around = unit.x != 0.0f || unit.z != 0.0f ? std::atan2(unit.x, -unit.z) : 0.0f;
    const float down = std::acos(std::clamp(unit.y, -1.0f, 1.0f));
    return {around / kTwoPi + 0.5f, down / kPi};
}

Vec3 equirectDirection(const Vec2& uv) {
    const float around = (uv.x - 0.5f) * kTwoPi;
    const float down = uv.y * kPi;
    const float level = std::sin(down);
    return {level * std::sin(around), std::cos(down), -level * std::cos(around)};
}

Vec3 turnedAboutVertical(const Vec3& direction, float radians) {
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    return {direction.x * cosine + direction.z * sine, direction.y, direction.z * cosine - direction.x * sine};
}

} // namespace cordite
