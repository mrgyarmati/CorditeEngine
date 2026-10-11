#pragma once

/// @file
/// Deterministic pseudo-random numbers.
///
/// Random produces the same sequence for the same seed on every platform and compiler,
/// unlike the distributions in <random>. Gameplay, procedural content and tests rely on
/// that. It is not suitable for cryptography.
///
/// @code
/// Random random(levelSeed);
/// const Vec3 scatter = spawnPoint + random.insideUnitSphere() * 2.0f;
/// const float delay = random.range(0.5f, 1.5f);
/// @endcode

#include "cordite/math/Math.h"

#include <cstdint>

namespace cordite {

/// PCG32 generator (O'Neill, pcg-random.org, "XSH RR" variant).
class Random {
public:
    explicit Random(uint64_t seed = 0x853c49e6748fea9bull, uint64_t stream = 0xda3e39cb94b95bdbull);

    [[nodiscard]] uint32_t nextU32();

    /// Uniform in [0, 1).
    [[nodiscard]] float nextFloat();

    /// Uniform in [minimum, maximum).
    [[nodiscard]] float range(float minimum, float maximum);

    /// Uniform integer in [minimum, maximumInclusive], without modulo bias.
    [[nodiscard]] int32_t rangeInt(int32_t minimum, int32_t maximumInclusive);

    /// Uniformly distributed point on the surface of the unit sphere.
    [[nodiscard]] Vec3 onUnitSphere();

    /// Uniformly distributed point inside the unit sphere.
    [[nodiscard]] Vec3 insideUnitSphere();

    /// Uniformly distributed point inside the unit circle.
    [[nodiscard]] Vec2 insideUnitCircle();

private:
    uint64_t m_state = 0;
    uint64_t m_increment = 0;
};

} // namespace cordite
