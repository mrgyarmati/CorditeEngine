#include "cordite/assets/Mipmaps.h"

#include "cordite/math/Color.h"

#include "AssetTestSupport.h"

#include <cmath>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

Image makeImage(uint32_t width, uint32_t height, ImageFormat format, const Vec4& color) {
    Image image = unwrap(Image::create(width, height, format));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            image.setPixel(x, y, color);
        }
    }
    return image;
}

/// A 2x2 image whose columns have the two given colors.
Image makeColumns(ImageFormat format, const Vec4& left, const Vec4& right) {
    Image image = unwrap(Image::create(2, 2, format));
    for (uint32_t y = 0; y < 2; ++y) {
        image.setPixel(0, y, left);
        image.setPixel(1, y, right);
    }
    return image;
}

} // namespace

TEST_SUITE("Mipmaps") {
    TEST_CASE("the number of levels follows the larger side") {
        CHECK(mipLevelCount(1, 1) == 1);
        CHECK(mipLevelCount(2, 2) == 2);
        CHECK(mipLevelCount(256, 256) == 9);
        CHECK(mipLevelCount(5, 3) == 3);
        CHECK(mipLevelCount(1, 8) == 4);
        CHECK(mipLevelCount(1024, 1) == 11);
        CHECK(mipLevelCount(16384, 16384) == 15);
        // Not an image, but not a reason to return zero levels either.
        CHECK(mipLevelCount(0, 0) == 1);
    }

    TEST_CASE("each level is half the one above, rounded down, and never smaller than one") {
        CHECK(mipLevelSize(8, 4, 0) == UVec2(8, 4));
        CHECK(mipLevelSize(8, 4, 1) == UVec2(4, 2));
        CHECK(mipLevelSize(8, 4, 2) == UVec2(2, 1));
        CHECK(mipLevelSize(8, 4, 3) == UVec2(1, 1));
        CHECK(mipLevelSize(5, 3, 1) == UVec2(2, 1));
        CHECK(mipLevelSize(8, 4, 40) == UVec2(1, 1));
    }

    TEST_CASE("a chain goes from half the image down to one pixel, in the image's format") {
        for (const ImageFormat format : {ImageFormat::R8, ImageFormat::Rg8, ImageFormat::Rgba8, ImageFormat::Rgba32F}) {
            CAPTURE(nameOf(format));
            const Image image = makeImage(8, 4, format, Vec4(0.25f, 0.5f, 0.75f, 1.0f));
            const std::vector<Image> levels = unwrap(generateMipChain(image, ColorSpace::Linear));

            REQUIRE(levels.size() == 3);
            CHECK(levels[0].width() == 4);
            CHECK(levels[0].height() == 2);
            CHECK(levels[1].width() == 2);
            CHECK(levels[1].height() == 1);
            CHECK(levels[2].width() == 1);
            CHECK(levels[2].height() == 1);
            for (const Image& level : levels) {
                CHECK(level.format() == format);
            }
        }
    }

    TEST_CASE("a uniform image stays uniform at every level, in either color space") {
        const Vec4 color(0.2f, 0.4f, 0.6f, 0.8f);
        for (const ColorSpace colorSpace : {ColorSpace::Linear, ColorSpace::Srgb}) {
            CAPTURE(nameOf(colorSpace));
            const Image image = makeImage(16, 16, ImageFormat::Rgba8, color);
            const Vec4 stored = image.pixel(0, 0);
            for (const Image& level : unwrap(generateMipChain(image, colorSpace))) {
                for (uint32_t y = 0; y < level.height(); ++y) {
                    for (uint32_t x = 0; x < level.width(); ++x) {
                        // One step of an 8-bit channel is 1/255.
                        CHECK_NEAR(level.pixel(x, y), stored, 1.5f / 255.0f);
                    }
                }
            }
        }
    }

    TEST_CASE("a single pixel has no smaller levels") {
        const Image image = makeImage(1, 1, ImageFormat::Rgba8, Vec4(1.0f));
        CHECK(unwrap(generateMipChain(image, ColorSpace::Srgb)).empty());
    }

    TEST_CASE("an image that is not square keeps shrinking its longer side") {
        const Image image = makeImage(1, 8, ImageFormat::Rgba8, Vec4(0.5f));
        const std::vector<Image> levels = unwrap(generateMipChain(image, ColorSpace::Linear));
        REQUIRE(levels.size() == 3);
        CHECK(levels[0].height() == 4);
        CHECK(levels[2].height() == 1);
        for (const Image& level : levels) {
            CHECK(level.width() == 1);
        }
    }

    TEST_CASE("colors are averaged as light, data as numbers") {
        const Image image = makeColumns(ImageFormat::Rgba8, Vec4(0.0f, 0.0f, 0.0f, 1.0f), Vec4(1.0f));

        // As numbers, black and white average to the middle value.
        const Vec4 asData = unwrap(generateMipChain(image, ColorSpace::Linear))[0].pixel(0, 0);
        CHECK(asData.r == doctest::Approx(0.5f).epsilon(0.02));

        // As light, half of white is brighter than the middle value once encoded again.
        const Vec4 asColor = unwrap(generateMipChain(image, ColorSpace::Srgb))[0].pixel(0, 0);
        CHECK(asColor.r == doctest::Approx(linearToSrgb(0.5f)).epsilon(0.02));
        CHECK(asColor.r > asData.r + 0.2f);
        CHECK(asColor.a == doctest::Approx(1.0f));
    }

    TEST_CASE("transparent pixels do not bleed their color into a color texture") {
        // Opaque red next to fully transparent green.
        const Image image = makeColumns(ImageFormat::Rgba8, Vec4(1.0f, 0.0f, 0.0f, 1.0f), Vec4(0.0f, 1.0f, 0.0f, 0.0f));

        const Vec4 asColor = unwrap(generateMipChain(image, ColorSpace::Srgb))[0].pixel(0, 0);
        CHECK(asColor.r == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(asColor.g == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(asColor.a == doctest::Approx(0.5f).epsilon(0.02));

        // In a data texture the fourth channel is just a number, and all four are averaged.
        const Vec4 asData = unwrap(generateMipChain(image, ColorSpace::Linear))[0].pixel(0, 0);
        CHECK(asData.r == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(asData.g == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(asData.a == doctest::Approx(0.5f).epsilon(0.02));
    }

    TEST_CASE("floating-point images are averaged without being clamped") {
        const Image image =
            makeColumns(ImageFormat::Rgba32F, Vec4(0.0f, 0.0f, 0.0f, 1.0f), Vec4(16.0f, 8.0f, 4.0f, 1.0f));
        // "Color" or not, the values are already linear.
        for (const ColorSpace colorSpace : {ColorSpace::Linear, ColorSpace::Srgb}) {
            const Vec4 average = unwrap(generateMipChain(image, colorSpace))[0].pixel(0, 0);
            CHECK(average.r == doctest::Approx(8.0f).epsilon(0.01));
            CHECK(average.g == doctest::Approx(4.0f).epsilon(0.01));
            CHECK(average.b == doctest::Approx(2.0f).epsilon(0.01));
        }
    }

    TEST_CASE("a single channel is always data") {
        const Image image = makeColumns(ImageFormat::R8, Vec4(0.0f), Vec4(1.0f));
        const float asColor = unwrap(generateMipChain(image, ColorSpace::Srgb))[0].pixel(0, 0).r;
        CHECK(asColor == doctest::Approx(0.5f).epsilon(0.02));
    }

    TEST_CASE("an empty image has no mipmaps to make") {
        const Result<std::vector<Image>> levels = generateMipChain(Image(), ColorSpace::Linear);
        REQUIRE_FALSE(levels.has_value());
        CHECK(levels.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("the names of the color spaces") {
        CHECK(nameOf(ColorSpace::Linear) == "linear");
        CHECK(nameOf(ColorSpace::Srgb) == "srgb");
    }
}
