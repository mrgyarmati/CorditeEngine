#include "cordite/math/SphericalHarmonics.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>

using namespace cordite;

namespace {

/// Calls `visit` with the directions of a grid over the whole sphere and the solid angle
/// each of them stands for: rings of equal height, which all have the same area.
void overTheSphere(const std::function<void(const Vec3& direction, float solidAngle)>& visit) {
    // Few enough that a sum of them in single precision is still exact to four digits.
    constexpr int kRings = 64;
    constexpr int kSteps = 128;
    const float solidAngle = 4.0f * kPi / static_cast<float>(kRings * kSteps);
    for (int ring = 0; ring < kRings; ++ring) {
        const float height = 1.0f - 2.0f * (static_cast<float>(ring) + 0.5f) / static_cast<float>(kRings);
        const float radius = std::sqrt(1.0f - height * height);
        for (int step = 0; step < kSteps; ++step) {
            const float angle = kTwoPi * (static_cast<float>(step) + 0.5f) / static_cast<float>(kSteps);
            visit(Vec3(radius * std::cos(angle), height, radius * std::sin(angle)), solidAngle);
        }
    }
}

/// Some directions, the same on every run.
std::array<Vec3, 12> someDirections() {
    Random random(7);
    std::array<Vec3, 12> directions{};
    directions[0] = Vec3(0.0f, 1.0f, 0.0f);
    directions[1] = Vec3(0.0f, -1.0f, 0.0f);
    directions[2] = Vec3(1.0f, 0.0f, 0.0f);
    directions[3] = Vec3(0.0f, 0.0f, -1.0f);
    for (size_t index = 4; index < directions.size(); ++index) {
        directions[index] =
            glm::normalize(Vec3(random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f)));
    }
    return directions;
}

} // namespace

TEST_SUITE("SphericalHarmonics") {
    TEST_CASE("the nine basis functions are of one size and stand at right angles to one another") {
        // The mean product of any two over the sphere: one over four pi for a function
        // with itself, nothing for two different ones.
        std::array<std::array<double, kSphericalHarmonicsCount>, kSphericalHarmonicsCount> products{};
        overTheSphere([&](const Vec3& direction, float solidAngle) {
            const std::array<float, kSphericalHarmonicsCount> basis = sphericalHarmonicsBasis(direction);
            for (size_t a = 0; a < kSphericalHarmonicsCount; ++a) {
                for (size_t b = 0; b < kSphericalHarmonicsCount; ++b) {
                    products[a][b] += static_cast<double>(basis[a]) * basis[b] * solidAngle;
                }
            }
        });
        for (size_t a = 0; a < kSphericalHarmonicsCount; ++a) {
            for (size_t b = 0; b < kSphericalHarmonicsCount; ++b) {
                CAPTURE(a);
                CAPTURE(b);
                CHECK(products[a][b] == doctest::Approx(a == b ? 1.0 : 0.0).epsilon(0.001));
            }
        }
        // The first is the same everywhere.
        CHECK(sphericalHarmonicsBasis(Vec3(0.0f, 1.0f, 0.0f))[0] == sphericalHarmonicsBasis(Vec3(1.0f, 0.0f, 0.0f))[0]);
        CHECK(sphericalHarmonicsBasis(Vec3(0.0f, 1.0f, 0.0f))[0] == doctest::Approx(0.5f / std::sqrt(kPi)));
    }

    TEST_CASE("light that is the same from everywhere lights every surface alike") {
        const Vec3 sky(0.25f, 0.5f, 1.0f);
        SphericalHarmonics9 light;
        overTheSphere([&](const Vec3& direction, float solidAngle) { light.add(direction, sky, solidAngle); });

        for (const Vec3& direction : someDirections()) {
            CAPTURE(direction);
            CHECK_NEAR(light.radiance(direction), sky, 2.0e-3f);
            // A white, matte surface under an even sky is as bright as the sky.
            CHECK_NEAR(light.irradiance(direction) / kPi, sky, 2.0e-3f);
        }
        // All of it is in the first of the nine.
        for (size_t index = 1; index < kSphericalHarmonicsCount; ++index) {
            CHECK_NEAR(light.coefficients[index], Vec3(0.0f), 2.0e-3f);
        }
    }

    TEST_CASE("under a sky above and nothing below, a surface gets what it sees of the sky") {
        const Vec3 sky(1.0f, 0.5f, 0.25f);
        SphericalHarmonics9 light;
        overTheSphere([&](const Vec3& direction, float solidAngle) {
            if (direction.y > 0.0f) {
                light.add(direction, sky, solidAngle);
            }
        });
        // Exactly: half of one plus the cosine of the angle to straight up. Facing up,
        // all of it; facing the horizon, half; facing down, none.
        CHECK_NEAR(light.irradiance(Vec3(0.0f, 1.0f, 0.0f)) / kPi, sky, 0.01f);
        CHECK_NEAR(light.irradiance(Vec3(1.0f, 0.0f, 0.0f)) / kPi, sky * 0.5f, 0.01f);
        CHECK_NEAR(light.irradiance(Vec3(0.0f, -1.0f, 0.0f)) / kPi, Vec3(0.0f), 0.01f);
        for (const Vec3& direction : someDirections()) {
            CAPTURE(direction);
            CHECK_NEAR(light.irradiance(direction) / kPi, sky * (0.5f * (1.0f + direction.y)), 0.01f);
        }
    }

    TEST_CASE("one small light is caught to within a few percent, and never lights from behind by much") {
        // All of it from one direction: a sun.
        const Vec3 towardsSun = glm::normalize(Vec3(0.3f, 0.8f, -0.5f));
        SphericalHarmonics9 light;
        light.add(towardsSun, Vec3(1000.0f), 0.001f);

        // Facing it, a surface gets all of what arrives - here one - and a sixteenth too
        // much: that is how well nine numbers hold a point.
        CHECK(light.irradiance(towardsSun).g == doctest::Approx(1.0625f).epsilon(0.001));
        // Edge-on it should get nothing, and gets a tenth; facing away, a sixteenth.
        const Vec3 sideways = glm::normalize(glm::cross(towardsSun, Vec3(0.0f, 1.0f, 0.0f)));
        CHECK(light.irradiance(sideways).g == doctest::Approx(0.09375f).epsilon(0.01));
        CHECK(light.irradiance(-towardsSun).g == doctest::Approx(0.0625f).epsilon(0.01));
        // Where the sum dips below nothing, it is nothing.
        for (const Vec3& direction : someDirections()) {
            const Vec3 irradiance = light.irradiance(direction);
            CHECK(irradiance.r >= 0.0f);
            CHECK(irradiance.r <= 1.07f);
        }
        const Vec3 behindAndBeside =
            glm::normalize(-0.533f * towardsSun + std::sqrt(1.0f - 0.533f * 0.533f) * sideways);
        CHECK(light.irradiance(behindAndBeside) == Vec3(0.0f));
    }

    TEST_CASE("lights add up") {
        SphericalHarmonics9 first;
        first.add(Vec3(0.0f, 1.0f, 0.0f), Vec3(2.0f, 0.0f, 0.0f), 0.5f);
        SphericalHarmonics9 second;
        second.add(Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 3.0f), 0.25f);
        SphericalHarmonics9 both = first;
        both.add(Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 3.0f), 0.25f);

        const Vec3 facing = glm::normalize(Vec3(1.0f, 1.0f, 0.2f));
        CHECK_NEAR(both.irradiance(facing), first.irradiance(facing) + second.irradiance(facing), 1.0e-5f);
        CHECK_NEAR(both.radiance(facing), first.radiance(facing) + second.radiance(facing), 1.0e-5f);
        // No light at all lights nothing.
        CHECK(SphericalHarmonics9().irradiance(facing) == Vec3(0.0f));
        CHECK(SphericalHarmonics9().radiance(facing) == Vec3(0.0f));
    }

    TEST_CASE("the coefficients for shaders give the light of a matte surface in one sum") {
        SphericalHarmonics9 light;
        Random random(11);
        for (int index = 0; index < 40; ++index) {
            const Vec3 direction =
                glm::normalize(Vec3(random.range(-1.0f, 1.0f), random.range(0.0f, 1.0f), random.range(-1.0f, 1.0f)));
            light.add(direction, Vec3(random.range(0.0f, 2.0f), random.range(0.0f, 2.0f), random.range(0.0f, 2.0f)),
                      0.1f);
        }
        const std::array<Vec3, kSphericalHarmonicsCount> scaled = light.diffuseCoefficients();
        for (const Vec3& normal : someDirections()) {
            CAPTURE(normal);
            const std::array<float, kSphericalHarmonicsCount> basis = sphericalHarmonicsBasis(normal);
            Vec3 sum(0.0f);
            for (size_t index = 0; index < kSphericalHarmonicsCount; ++index) {
                sum += scaled[index] * basis[index];
            }
            CHECK_NEAR(glm::max(sum, Vec3(0.0f)), light.irradiance(normal) / kPi, 1.0e-4f);
        }
    }
}
