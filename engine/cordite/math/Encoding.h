#pragma once

/// @file
/// Compact encodings shared between the CPU and shaders. The shader versions live in
/// shaders/common/Encoding.slang and must stay in sync with these.

#include "cordite/math/Math.h"

namespace cordite {

/// Encodes a unit vector into two values in [0, 1] using octahedral mapping. The G-buffer
/// stores world-space normals this way in a two-channel 16-bit target.
[[nodiscard]] inline Vec2 encodeOctahedral(const Vec3& normal) {
    const float sum = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    Vec2 projected = Vec2(normal.x, normal.y) / sum;
    if (normal.z < 0.0f) {
        const Vec2 folded = Vec2(1.0f) - glm::abs(Vec2(projected.y, projected.x));
        projected = Vec2(projected.x >= 0.0f ? folded.x : -folded.x, projected.y >= 0.0f ? folded.y : -folded.y);
    }
    return projected * 0.5f + 0.5f;
}

/// Inverse of encodeOctahedral. The result is normalized.
[[nodiscard]] inline Vec3 decodeOctahedral(const Vec2& encoded) {
    const Vec2 projected = encoded * 2.0f - 1.0f;
    Vec3 normal(projected.x, projected.y, 1.0f - std::abs(projected.x) - std::abs(projected.y));
    const float fold = saturate(-normal.z);
    normal.x += normal.x >= 0.0f ? -fold : fold;
    normal.y += normal.y >= 0.0f ? -fold : fold;
    return glm::normalize(normal);
}

} // namespace cordite
