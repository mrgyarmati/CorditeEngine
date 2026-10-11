#include "cordite/math/Color.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

using namespace cordite;

TEST_SUITE("Color") {
    TEST_CASE("sRGB conversion matches the standard at reference points") {
        CHECK(srgbToLinear(0.0f) == 0.0f);
        CHECK(srgbToLinear(1.0f) == doctest::Approx(1.0f));
        CHECK(srgbToLinear(0.5f) == doctest::Approx(0.21404f).epsilon(1e-4));
        CHECK(srgbToLinear(0.04045f) == doctest::Approx(0.04045f / 12.92f));

        CHECK(linearToSrgb(0.0f) == 0.0f);
        CHECK(linearToSrgb(1.0f) == doctest::Approx(1.0f));
        CHECK(linearToSrgb(0.21404f) == doctest::Approx(0.5f).epsilon(1e-4));
        CHECK(linearToSrgb(0.18f) == doctest::Approx(0.46136f).epsilon(1e-4));
    }

    TEST_CASE("sRGB conversion round-trips and clamps") {
        for (int step = 0; step <= 100; ++step) {
            const float value = static_cast<float>(step) / 100.0f;
            CHECK(linearToSrgb(srgbToLinear(value)) == doctest::Approx(value).epsilon(1e-4));
        }
        CHECK(linearToSrgb(-0.5f) == 0.0f);
        CHECK(linearToSrgb(4.0f) == doctest::Approx(1.0f));

        CHECK_NEAR(linearToSrgb(srgbToLinear(Vec3(0.2f, 0.5f, 0.8f))), Vec3(0.2f, 0.5f, 0.8f), 1e-5f);
    }

    TEST_CASE("luminance uses Rec. 709 weights") {
        CHECK(luminance(Vec3(1.0f)) == doctest::Approx(1.0f));
        CHECK(luminance(Vec3(0.0f)) == 0.0f);
        CHECK(luminance(Vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(0.2126f));
        CHECK(luminance(Vec3(0.0f, 1.0f, 0.0f)) == doctest::Approx(0.7152f));
        CHECK(luminance(Vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(0.0722f));
    }

    TEST_CASE("hsvToRgb hits the primary and secondary colors") {
        CHECK_NEAR(hsvToRgb(Vec3(0.0f, 1.0f, 1.0f)), Vec3(1.0f, 0.0f, 0.0f), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(1.0f / 6.0f, 1.0f, 1.0f)), Vec3(1.0f, 1.0f, 0.0f), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(1.0f / 3.0f, 1.0f, 1.0f)), Vec3(0.0f, 1.0f, 0.0f), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(0.5f, 1.0f, 1.0f)), Vec3(0.0f, 1.0f, 1.0f), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(2.0f / 3.0f, 1.0f, 1.0f)), Vec3(0.0f, 0.0f, 1.0f), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(5.0f / 6.0f, 1.0f, 1.0f)), Vec3(1.0f, 0.0f, 1.0f), 1e-5f);

        // Zero saturation is grey; hue wraps around.
        CHECK_NEAR(hsvToRgb(Vec3(0.3f, 0.0f, 0.5f)), Vec3(0.5f), 1e-6f);
        CHECK_NEAR(hsvToRgb(Vec3(1.25f, 1.0f, 1.0f)), hsvToRgb(Vec3(0.25f, 1.0f, 1.0f)), 1e-5f);
        CHECK_NEAR(hsvToRgb(Vec3(-0.75f, 1.0f, 1.0f)), hsvToRgb(Vec3(0.25f, 1.0f, 1.0f)), 1e-5f);
    }

    TEST_CASE("HSV conversion round-trips") {
        Random random(2024);
        for (int iteration = 0; iteration < 500; ++iteration) {
            const Vec3 rgb(random.nextFloat(), random.nextFloat(), random.nextFloat());
            CHECK_NEAR(hsvToRgb(rgbToHsv(rgb)), rgb, 1e-4f);
        }

        const Vec3 grey = rgbToHsv(Vec3(0.4f));
        CHECK(grey.x == 0.0f);
        CHECK(grey.y == 0.0f);
        CHECK(grey.z == doctest::Approx(0.4f));
        CHECK(rgbToHsv(Vec3(0.0f)) == Vec3(0.0f));
    }

    TEST_CASE("color temperature runs from warm to cool") {
        const Vec3 candle = colorTemperatureToRgb(1900.0f);
        CHECK(candle.r == doctest::Approx(1.0f));
        CHECK(candle.r > candle.g);
        CHECK(candle.g > candle.b);

        const Vec3 daylight = colorTemperatureToRgb(6500.0f);
        CHECK(daylight.r > 0.9f);
        CHECK(daylight.g > 0.9f);
        CHECK(daylight.b > 0.9f);

        const Vec3 sky = colorTemperatureToRgb(12000.0f);
        CHECK(sky.b == doctest::Approx(1.0f));
        CHECK(sky.b > sky.g);
        CHECK(sky.g > sky.r);

        // Out-of-range input is clamped rather than producing garbage.
        CHECK_NEAR(colorTemperatureToRgb(-50.0f), colorTemperatureToRgb(1000.0f), 0.0f);
        CHECK_NEAR(colorTemperatureToRgb(1.0e9f), colorTemperatureToRgb(40000.0f), 0.0f);
        CHECK(isFinite(colorTemperatureToRgb(1000.0f)));
    }

    TEST_CASE("packRgba8 stores red in the low byte") {
        CHECK(packRgba8(Vec4(1.0f, 0.0f, 0.0f, 1.0f)) == 0xff0000ffu);
        CHECK(packRgba8(Vec4(0.0f, 1.0f, 0.0f, 0.0f)) == 0x0000ff00u);
        CHECK(packRgba8(Vec4(0.0f, 0.0f, 1.0f, 0.0f)) == 0x00ff0000u);
        CHECK(packRgba8(Vec4(2.0f, -1.0f, 0.5f, 1.0f)) == 0xff8000ffu);

        const Vec4 color(0.2f, 0.4f, 0.6f, 0.8f);
        CHECK_NEAR(unpackRgba8(packRgba8(color)), color, 1.0f / 255.0f);
    }

    TEST_CASE("parseHexColor converts sRGB hex to linear") {
        const std::optional<Vec4> white = parseHexColor("#ffffff");
        REQUIRE(white.has_value());
        CHECK_NEAR(*white, Vec4(1.0f), 1e-5f);

        const std::optional<Vec4> black = parseHexColor("#000000");
        REQUIRE(black.has_value());
        CHECK_NEAR(*black, Vec4(0.0f, 0.0f, 0.0f, 1.0f), 1e-6f);

        const std::optional<Vec4> grey = parseHexColor("#80808080");
        REQUIRE(grey.has_value());
        const float linearGrey = srgbToLinear(128.0f / 255.0f);
        CHECK_NEAR(*grey, Vec4(linearGrey, linearGrey, linearGrey, 128.0f / 255.0f), 1e-5f);

        const std::optional<Vec4> upper = parseHexColor("#FF8000");
        REQUIRE(upper.has_value());
        CHECK(upper->r == doctest::Approx(1.0f));
        CHECK(upper->b == 0.0f);
    }

    TEST_CASE("parseHexColor rejects malformed text") {
        CHECK_FALSE(parseHexColor("").has_value());
        CHECK_FALSE(parseHexColor("ffffff").has_value());
        CHECK_FALSE(parseHexColor("#fff").has_value());
        CHECK_FALSE(parseHexColor("#ffffffff0").has_value());
        CHECK_FALSE(parseHexColor("#gg0000").has_value());
        CHECK_FALSE(parseHexColor("#ffffffzz").has_value());
    }

    TEST_CASE("toHexColor round-trips with parseHexColor") {
        for (const char* text : {"#000000ff", "#ffffffff", "#8a3b1cff", "#12345678"}) {
            const std::optional<Vec4> parsed = parseHexColor(text);
            REQUIRE(parsed.has_value());
            CHECK(toHexColor(*parsed) == text);
        }
    }
}
