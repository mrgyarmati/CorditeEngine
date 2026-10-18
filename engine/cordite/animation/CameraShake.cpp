#include "cordite/animation/CameraShake.h"

#include <glm/gtc/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace cordite {
namespace {

/// The rows of the noise the six ways a camera moves follow - along its X, Y and Z, then
/// about them. Each lies half way between two rows of the lattice, where the noise is not
/// nothing at every whole turn, and no two share a row: they are noises of their own.
constexpr std::array<float, 6> kNoiseRows = {0.5f, 2.5f, 4.5f, 6.5f, 8.5f, 10.5f};
/// The lattice repeats after as many rows as these need, and after kCameraShakePeriod
/// turns along them.
constexpr float kNoiseRowCount = 12.0f;

/// The noise of a row at a phase, from -1 to 1. glm scales its Perlin noise to about
/// that, without holding it there: the furthest a camera goes is the furthest it may.
float noiseOf(float phase, float row) {
    const float noise = glm::perlin(Vec2(phase, row), Vec2(kCameraShakePeriod, kNoiseRowCount));
    return std::clamp(noise, -1.0f, 1.0f);
}

/// A number that may be no number, as a number of no less than nothing.
float atLeastNothing(float value) {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

/// How far along, or about, its three axes a camera goes: `furthest` of each, by
/// `amount`, by the noise of the three rows from `firstRow` on.
Vec3 shakeAlong(const Vec3& furthest, float amount, float phase, size_t firstRow) {
    Vec3 shaken(0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        const size_t row = firstRow + static_cast<size_t>(axis);
        shaken[axis] = amount * atLeastNothing(furthest[axis]) * noiseOf(phase, kNoiseRows.at(row));
    }
    return shaken;
}

} // namespace

Transform cameraShakeOffset(const CameraShakeComponent& shake) {
    const float trauma = std::min(atLeastNothing(shake.trauma), 1.0f);
    const float amount = trauma * trauma;
    if (amount <= 0.0f) {
        return {};
    }
    const float phase = std::isfinite(shake.phase) ? shake.phase : 0.0f;
    Transform offset;
    offset.position = shakeAlong(shake.maxOffset, amount, phase, 0);
    offset.rotation = quatFromEulerDegrees(shakeAlong(shake.maxRotation, amount, phase, 3));
    return offset;
}

void shakeCameras(World& world, float deltaSeconds) {
    const float step = atLeastNothing(deltaSeconds);
    if (step <= 0.0f) {
        return;
    }
    for (const auto [handle, shake] : world.registry().view<CameraShakeComponent>().each()) {
        if (!world.isEnabledInHierarchy(Entity{handle})) {
            continue;
        }
        shake.trauma =
            std::max(std::min(atLeastNothing(shake.trauma), 1.0f) - atLeastNothing(shake.decay) * step, 0.0f);
        // Round within the period: the noise repeats there, so going round is no jump.
        const float phase = (std::isfinite(shake.phase) ? shake.phase : 0.0f) + atLeastNothing(shake.frequency) * step;
        const float wrapped = phase - kCameraShakePeriod * std::floor(phase / kCameraShakePeriod);
        shake.phase = wrapped >= 0.0f && wrapped < kCameraShakePeriod ? wrapped : 0.0f;
    }
}

} // namespace cordite
