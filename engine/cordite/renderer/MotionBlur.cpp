#include "cordite/renderer/MotionBlur.h"

#include <algorithm>
#include <cmath>

namespace cordite {

uint32_t motionBlurRadius(uint32_t height) {
    return std::max<uint32_t>(1, (height + kMotionBlurRadiusDivisor - 1) / kMotionBlurRadiusDivisor);
}

Vec2 blurOf(Vec2 motion, Vec2 size, float amount, float radius) {
    const Vec2 blur = motion * size * (amount * 0.5f);
    const float length = glm::length(blur);
    if (!(length > 0.0f) || !std::isfinite(length)) {
        return Vec2(0.0f);
    }
    return length > radius ? blur * (radius / length) : blur;
}

float inFrontOf(float depth, float otherDepth) {
    // At no depth at all - which no view has - a point is as near as the other or not.
    const float softness = std::max(std::min(depth, otherDepth) * kMotionBlurDepthSoftness, 1.0e-4f);
    return std::clamp(1.0f - (depth - otherDepth) / softness, 0.0f, 1.0f);
}

float motionCone(float distance, float speed) {
    return std::clamp(1.0f - distance / speed, 0.0f, 1.0f);
}

float motionCylinder(float distance, float speed) {
    return 1.0f - glm::smoothstep(0.95f * speed, 1.05f * speed, distance);
}

float motionBlurWeight(const MotionBlurPoint& sample, const MotionBlurPoint& pixel) {
    // What is in front and moves over the pixel; what is behind, where the pixel moves
    // itself and so lets it be seen; and what moves along with the pixel, both reaching
    // the other.
    return inFrontOf(sample.depth, pixel.depth) * motionCone(sample.distance, sample.speed) +
           inFrontOf(pixel.depth, sample.depth) * motionCone(sample.distance, pixel.speed) +
           motionCylinder(sample.distance, sample.speed) * motionCylinder(sample.distance, pixel.speed) * 2.0f;
}

float motionBlurSampleAt(uint32_t index, uint32_t count, UVec2 pixel) {
    const float jitter = motionBlurNoise(pixel) - 0.5f;
    return glm::mix(-1.0f, 1.0f, (static_cast<float>(index) + jitter + 1.0f) / (static_cast<float>(count) + 1.0f));
}

float motionBlurNoise(UVec2 pixel) {
    const float first = 0.06711056f * static_cast<float>(pixel.x) + 0.00583715f * static_cast<float>(pixel.y);
    const float second = 52.9829189f * (first - std::floor(first));
    return second - std::floor(second);
}

} // namespace cordite
