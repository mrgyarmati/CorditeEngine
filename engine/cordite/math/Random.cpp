#include "cordite/math/Random.h"

#include <algorithm>

namespace cordite {

Random::Random(uint64_t seed, uint64_t stream) : m_increment((stream << 1u) | 1u) {
    // Seeding sequence from the PCG reference implementation.
    (void)nextU32();
    m_state += seed;
    (void)nextU32();
}

uint32_t Random::nextU32() {
    const uint64_t previous = m_state;
    m_state = previous * 6364136223846793005ull + m_increment;

    const auto xorShifted = static_cast<uint32_t>(((previous >> 18u) ^ previous) >> 27u);
    const auto rotation = static_cast<uint32_t>(previous >> 59u);
    return (xorShifted >> rotation) | (xorShifted << ((32u - rotation) & 31u));
}

float Random::nextFloat() {
    // 24 random bits fill a float mantissa exactly, so the result is uniform and < 1.
    return static_cast<float>(nextU32() >> 8u) * (1.0f / 16777216.0f);
}

float Random::range(float minimum, float maximum) {
    return minimum + (maximum - minimum) * nextFloat();
}

int32_t Random::rangeInt(int32_t minimum, int32_t maximumInclusive) {
    if (maximumInclusive <= minimum) {
        return minimum;
    }

    const auto span = static_cast<uint32_t>(static_cast<int64_t>(maximumInclusive) - minimum) + 1u;
    if (span == 0) {
        // The full 32-bit range.
        return static_cast<int32_t>(nextU32());
    }

    // Rejection sampling: discard the low values that would make some results more
    // likely than others.
    const uint32_t threshold = (0u - span) % span;
    while (true) {
        const uint32_t value = nextU32();
        if (value >= threshold) {
            return static_cast<int32_t>(static_cast<int64_t>(minimum) + (value % span));
        }
    }
}

Vec3 Random::onUnitSphere() {
    const float z = range(-1.0f, 1.0f);
    const float angle = range(0.0f, kTwoPi);
    const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {radius * std::cos(angle), radius * std::sin(angle), z};
}

Vec3 Random::insideUnitSphere() {
    return onUnitSphere() * std::cbrt(nextFloat());
}

Vec2 Random::insideUnitCircle() {
    const float angle = range(0.0f, kTwoPi);
    const float radius = std::sqrt(nextFloat());
    return {radius * std::cos(angle), radius * std::sin(angle)};
}

} // namespace cordite
