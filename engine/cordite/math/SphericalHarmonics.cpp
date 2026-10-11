#include "cordite/math/SphericalHarmonics.h"

namespace cordite {
namespace {

/// What adding up a hemisphere of light, weighted by the cosine, makes of each band of
/// the light: the bands of the "clamped cosine", pi, two thirds of pi and a quarter of pi.
constexpr std::array<float, kSphericalHarmonicsCount> kCosineBands = {
    kPi,        2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, kPi / 4.0f,
    kPi / 4.0f, kPi / 4.0f,        kPi / 4.0f,        kPi / 4.0f,
};

} // namespace

std::array<float, kSphericalHarmonicsCount> sphericalHarmonicsBasis(const Vec3& direction) {
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;
    return {
        0.282095f,
        0.488603f * y,
        0.488603f * z,
        0.488603f * x,
        1.092548f * x * y,
        1.092548f * y * z,
        0.315392f * (3.0f * z * z - 1.0f),
        1.092548f * x * z,
        0.546274f * (x * x - y * y),
    };
}

void SphericalHarmonics9::add(const Vec3& direction, const Vec3& radiance, float solidAngle) {
    const std::array<float, kSphericalHarmonicsCount> basis = sphericalHarmonicsBasis(direction);
    for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
        coefficients[index] += radiance * (basis[index] * solidAngle);
    }
}

Vec3 SphericalHarmonics9::radiance(const Vec3& direction) const {
    const std::array<float, kSphericalHarmonicsCount> basis = sphericalHarmonicsBasis(direction);
    Vec3 result(0.0f);
    for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
        result += coefficients[index] * basis[index];
    }
    return result;
}

Vec3 SphericalHarmonics9::irradiance(const Vec3& normal) const {
    const std::array<float, kSphericalHarmonicsCount> basis = sphericalHarmonicsBasis(normal);
    Vec3 result(0.0f);
    for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
        result += coefficients[index] * (basis[index] * kCosineBands[index]);
    }
    return glm::max(result, Vec3(0.0f));
}

std::array<Vec3, kSphericalHarmonicsCount> SphericalHarmonics9::diffuseCoefficients() const {
    std::array<Vec3, kSphericalHarmonicsCount> scaled{};
    for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
        scaled[index] = coefficients[index] * (kCosineBands[index] / kPi);
    }
    return scaled;
}

} // namespace cordite
