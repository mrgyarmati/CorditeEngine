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

    TEST_CASE("a sun in a floating-point image leaves no ring of less than no light around it") {
        // A dim sky with one pixel tens of thousands of times brighter: a sharpening
        // filter would overshoot beside it.
        Image image = unwrap(Image::create(64, 32, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < image.height(); ++y) {
            for (uint32_t x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, Vec4(0.01f, 0.02f, 0.04f, 1.0f));
            }
        }
        image.setPixel(21, 9, Vec4(60000.0f, 50000.0f, 40000.0f, 1.0f));
        const double total = 60000.0 + 0.01 * (64.0 * 32.0 - 1.0);

        for (const ColorSpace colorSpace : {ColorSpace::Linear, ColorSpace::Srgb}) {
            CAPTURE(nameOf(colorSpace));
            uint32_t levelNumber = 1;
            for (const Image& level : unwrap(generateMipChain(image, colorSpace))) {
                CAPTURE(levelNumber);
                double sum = 0.0;
                for (uint32_t y = 0; y < level.height(); ++y) {
                    for (uint32_t x = 0; x < level.width(); ++x) {
                        const Vec4 pixel = level.pixel(x, y);
                        // Nowhere darker than the sky.
                        CHECK(pixel.r >= 0.01f * 0.999f);
                        CHECK(pixel.g >= 0.02f * 0.999f);
                        CHECK(pixel.b >= 0.04f * 0.999f);
                        sum += pixel.r;
                    }
                }
                // And none of the light is lost: a pixel of a level stands for four,
                // sixteen, ... of the image.
                const double pixelsEach = (64.0 * 32.0) / (static_cast<double>(level.width()) * level.height());
                CHECK(sum * pixelsEach == doctest::Approx(total).epsilon(1.0e-4));
                ++levelNumber;
            }
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

TEST_SUITE("ResizeImage") {
    TEST_CASE("an image is resized as its levels are made: colors averaged as light") {
        const Image image = makeColumns(ImageFormat::Rgba8, Vec4(0.0f, 0.0f, 0.0f, 1.0f), Vec4(1.0f));
        const Image one = unwrap(resizeImage(image, 1, 1, ColorSpace::Srgb));
        CHECK(one.width() == 1);
        CHECK(one.height() == 1);
        CHECK(one.format() == ImageFormat::Rgba8);
        // Half of white, as light: brighter than the middle number once encoded again.
        CHECK(one.pixel(0, 0).r == doctest::Approx(linearToSrgb(0.5f)).epsilon(0.02));
        CHECK(unwrap(resizeImage(image, 1, 1, ColorSpace::Linear)).pixel(0, 0).r ==
              doctest::Approx(0.5f).epsilon(0.02));

        // Larger, and of another shape: the left still black, the right still white.
        const Image wide = unwrap(resizeImage(image, 16, 4, ColorSpace::Srgb));
        CHECK(wide.width() == 16);
        CHECK(wide.height() == 4);
        CHECK(wide.pixel(0, 2).r < 0.05f);
        CHECK(wide.pixel(15, 2).r > 0.95f);
    }

    TEST_CASE("an image that is empty, or a size of nothing, cannot be resized") {
        const Image image = makeImage(4, 4, ImageFormat::Rgba8, Vec4(1.0f));
        const auto refused = [](const Result<Image>& result) {
            return !result.has_value() && result.error().code == ErrorCode::InvalidArgument;
        };
        CHECK(refused(resizeImage(Image(), 2, 2, ColorSpace::Srgb)));
        CHECK(refused(resizeImage(image, 0, 2, ColorSpace::Srgb)));
        CHECK(refused(resizeImage(image, 2, 0, ColorSpace::Linear)));
    }
}
