#include "cordite/renderer/EnvironmentLight.h"

#include "cordite/assets/Image.h"
#include "cordite/math/Color.h"
#include "cordite/math/Equirectangular.h"
#include "cordite/math/SphericalHarmonics.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>

using namespace cordite;
using cordite::test::unwrap;

namespace {

/// An image of everything around in which each direction holds the light `light` says.
Image makeEnvironment(uint32_t width, uint32_t height, const std::function<Vec3(const Vec3&)>& light,
                      ImageFormat format = ImageFormat::Rgba32F) {
    Image image = unwrap(Image::create(width, height, format));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
            image.setPixel(x, y, Vec4(light(equirectDirection(uv)), 1.0f));
        }
    }
    return image;
}

const Vec3 kAhead(0.0f, 0.0f, -1.0f);
const Vec3 kRight(1.0f, 0.0f, 0.0f);
const Vec3 kLeft(-1.0f, 0.0f, 0.0f);
const Vec3 kBehind(0.0f, 0.0f, 1.0f);

} // namespace

TEST_SUITE("EnvironmentLight") {
    TEST_CASE("an even image lights every surface with its own light") {
        const Vec3 light(0.25f, 0.5f, 2.0f);
        const auto even = [&](const Vec3& /*direction*/) { return light; };
        for (const UVec2 size : {UVec2(64, 32), UVec2(1, 1), UVec2(7, 5), UVec2(16, 64), UVec2(1500, 700)}) {
            CAPTURE(size);
            const SphericalHarmonics9 projected = projectEnvironment(makeEnvironment(size.x, size.y, even));
            for (const Vec3& normal : {kWorldUp, -kWorldUp, kAhead, kRight, glm::normalize(Vec3(1.0f, 2.0f, -3.0f))}) {
                CHECK_NEAR(projected.irradiance(normal) / kPi, light, 2.0e-3f);
            }
        }
    }

    TEST_CASE("a sky over nothing lights a surface by how much of the sky it faces") {
        const Vec3 sky(1.0f, 0.5f, 0.25f);
        const SphericalHarmonics9 projected = projectEnvironment(
            makeEnvironment(256, 128, [&](const Vec3& direction) { return direction.y > 0.0f ? sky : Vec3(0.0f); }));
        CHECK_NEAR(projected.irradiance(kWorldUp) / kPi, sky, 5.0e-3f);
        CHECK_NEAR(projected.irradiance(kAhead) / kPi, sky * 0.5f, 5.0e-3f);
        CHECK_NEAR(projected.irradiance(kRight) / kPi, sky * 0.5f, 5.0e-3f);
        CHECK_NEAR(projected.irradiance(-kWorldUp) / kPi, Vec3(0.0f), 5.0e-3f);
    }

    TEST_CASE("light from one side of the image lights what faces that side") {
        // Light from the right half of the world (+X) only.
        const SphericalHarmonics9 projected = projectEnvironment(
            makeEnvironment(256, 128, [](const Vec3& direction) { return Vec3(direction.x > 0.0f ? 1.0f : 0.0f); }));
        CHECK(projected.irradiance(kRight).g / kPi == doctest::Approx(1.0f).epsilon(0.01));
        CHECK(projected.irradiance(kLeft).g / kPi == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(projected.irradiance(kAhead).g / kPi == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(projected.irradiance(kWorldUp).g / kPi == doctest::Approx(0.5f).epsilon(0.01));
        // And from ahead (-Z) only.
        const SphericalHarmonics9 ahead = projectEnvironment(
            makeEnvironment(256, 128, [](const Vec3& direction) { return Vec3(direction.z < 0.0f ? 1.0f : 0.0f); }));
        CHECK(ahead.irradiance(kAhead).g / kPi == doctest::Approx(1.0f).epsilon(0.01));
        CHECK(ahead.irradiance(kBehind).g / kPi == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(ahead.irradiance(kRight).g / kPi == doctest::Approx(0.5f).epsilon(0.01));
    }

    TEST_CASE("a large image is taken in whole: a sun of a single pixel is not skipped") {
        // 4096 pixels wide: read as cells of sixteen by sixteen pixels.
        Image image = unwrap(Image::create(4096, 2048, ImageFormat::Rgba32F));
        const uint32_t sunX = 3001;
        const uint32_t sunY = 517;
        image.setPixel(sunX, sunY, Vec4(50000.0f, 25000.0f, 0.0f, 1.0f));
        const SphericalHarmonics9 projected = projectEnvironment(image);

        // What the pixel sends: its light times the part of the sphere it covers.
        const float top = static_cast<float>(sunY) / 2048.0f * kPi;
        const float bottom = static_cast<float>(sunY + 1) / 2048.0f * kPi;
        const float solidAngle = (std::cos(top) - std::cos(bottom)) * kTwoPi / 4096.0f;
        const float flux = 50000.0f * solidAngle;
        // Facing it, a surface gets that much, and a sixteenth more as nine numbers
        // tell it (SphericalHarmonicsTests).
        const Vec3 towardsSun = equirectDirection(
            Vec2((static_cast<float>(sunX) + 0.5f) / 4096.0f, (static_cast<float>(sunY) + 0.5f) / 2048.0f));
        const Vec3 facing = projected.irradiance(towardsSun);
        CHECK(facing.r == doctest::Approx(flux * 1.0625f).epsilon(0.02));
        CHECK(facing.g == doctest::Approx(flux * 0.5f * 1.0625f).epsilon(0.02));
        CHECK(facing.b == 0.0f);
    }

    TEST_CASE("an image of 8-bit colors is taken as sRGB") {
        const auto even = [](const Vec3& /*direction*/) { return Vec3(128.0f / 255.0f, 1.0f, 0.0f); };
        const SphericalHarmonics9 projected = projectEnvironment(makeEnvironment(32, 16, even, ImageFormat::Rgba8));
        const Vec3 light = srgbToLinear(Vec3(128.0f / 255.0f, 1.0f, 0.0f));
        CHECK(light.r == doctest::Approx(0.2158f).epsilon(0.001));
        CHECK_NEAR(projected.irradiance(kWorldUp) / kPi, light, 2.0e-3f);
        CHECK_NEAR(projected.irradiance(kLeft) / kPi, light, 2.0e-3f);
    }

    TEST_CASE("what is no light counts as none: no image, and pixels that are not numbers or less than nothing") {
        const SphericalHarmonics9 ofNothing = projectEnvironment(Image());
        for (const Vec3& coefficient : ofNothing.coefficients) {
            CHECK(coefficient == Vec3(0.0f));
        }

        Image image = unwrap(Image::create(64, 32, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < image.height(); ++y) {
            for (uint32_t x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, Vec4(0.5f, 0.5f, 0.5f, 1.0f));
            }
        }
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        const float infinite = std::numeric_limits<float>::infinity();
        // A pixel with one bad number is left out as a whole; a negative number is nothing.
        image.setPixel(10, 10, Vec4(notANumber, 0.5f, 0.5f, 1.0f));
        image.setPixel(20, 12, Vec4(0.5f, infinite, 0.5f, 1.0f));
        image.setPixel(30, 14, Vec4(0.5f, 0.5f, -infinite, 1.0f));
        image.setPixel(40, 16, Vec4(-1000.0f, -1000.0f, -1000.0f, 1.0f));
        const SphericalHarmonics9 projected = projectEnvironment(image);
        for (const Vec3& coefficient : projected.coefficients) {
            CHECK(isFinite(coefficient));
        }
        // Four pixels of two thousand are dark: the rest lights as before, nearly.
        for (const Vec3& normal : {kWorldUp, kAhead, kBehind, -kWorldUp}) {
            const Vec3 light = projected.irradiance(normal) / kPi;
            CHECK(light.r <= 0.5f + 1.0e-3f);
            CHECK(light.r > 0.49f);
            CHECK(light.g > 0.49f);
            CHECK(light.b > 0.49f);
        }
    }
}
