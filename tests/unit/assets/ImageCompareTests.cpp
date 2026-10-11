#include "cordite/assets/ImageCompare.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <string>

using namespace cordite;

namespace {

/// A 10x10 opaque image of one color given as 8-bit channel values.
Image makeSolid(uint8_t red, uint8_t green, uint8_t blue) {
    Image image = *Image::create(10, 10, ImageFormat::Rgba8);
    for (size_t offset = 0; offset < image.pixels().size(); offset += 4) {
        image.pixels()[offset + 0] = static_cast<std::byte>(red);
        image.pixels()[offset + 1] = static_cast<std::byte>(green);
        image.pixels()[offset + 2] = static_cast<std::byte>(blue);
        image.pixels()[offset + 3] = std::byte{255};
    }
    return image;
}

void setRed(Image& image, uint32_t x, uint32_t y, uint8_t value) {
    image.pixels()[(static_cast<size_t>(y) * image.width() + x) * 4] = static_cast<std::byte>(value);
}

} // namespace

TEST_SUITE("ImageCompare") {
    TEST_CASE("identical images match with no differences") {
        const Image image = makeSolid(100, 150, 200);
        const Result<ImageComparison> result = compareImages(image, image);
        REQUIRE(result.has_value());
        CHECK(result->matches);
        CHECK(result->maxChannelDifference == 0);
        CHECK(result->differingPixels == 0);
        CHECK(result->totalPixels == 100);
        CHECK(result->meanDifference == 0.0);
        CHECK(result->differingRatio() == 0.0);
    }

    TEST_CASE("differences within the channel tolerance are not counted") {
        const Image expected = makeSolid(100, 100, 100);
        const Image actual = makeSolid(102, 98, 100);

        const ImageComparison tolerant = *compareImages(actual, expected, {.channelTolerance = 2});
        CHECK(tolerant.matches);
        CHECK(tolerant.differingPixels == 0);
        CHECK(tolerant.maxChannelDifference == 2);

        const ImageComparison strict = *compareImages(actual, expected, {.channelTolerance = 1});
        CHECK_FALSE(strict.matches);
        CHECK(strict.differingPixels == 100);
    }

    TEST_CASE("statistics describe the size of the difference") {
        const Image expected = makeSolid(0, 0, 0);
        Image actual = makeSolid(0, 0, 0);
        setRed(actual, 3, 4, 200);
        setRed(actual, 9, 9, 40);

        const ImageComparison result = *compareImages(actual, expected, {.channelTolerance = 0});
        CHECK_FALSE(result.matches);
        CHECK(result.differingPixels == 2);
        CHECK(result.maxChannelDifference == 200);
        CHECK(result.differingRatio() == doctest::Approx(0.02));
        // 240 steps of difference spread over 100 pixels of 4 channels.
        CHECK(result.meanDifference == doctest::Approx(0.6));
    }

    TEST_CASE("a share of differing pixels can be allowed") {
        const Image expected = makeSolid(0, 0, 0);
        Image actual = makeSolid(0, 0, 0);
        setRed(actual, 0, 0, 255);
        setRed(actual, 1, 0, 255);

        CHECK((*compareImages(actual, expected, {.channelTolerance = 0, .maxDifferingRatio = 0.02})).matches);
        CHECK_FALSE((*compareImages(actual, expected, {.channelTolerance = 0, .maxDifferingRatio = 0.019})).matches);
    }

    TEST_CASE("alpha differences count like any other channel") {
        const Image expected = makeSolid(10, 10, 10);
        Image actual = makeSolid(10, 10, 10);
        actual.pixels()[3] = std::byte{0};

        const ImageComparison result = *compareImages(actual, expected, {.channelTolerance = 0});
        CHECK(result.differingPixels == 1);
        CHECK(result.maxChannelDifference == 255);
    }

    TEST_CASE("images that cannot be compared are an error, not a mismatch") {
        const Image small = *Image::create(4, 4, ImageFormat::Rgba8);
        const Image large = *Image::create(8, 4, ImageFormat::Rgba8);
        const Image grey = *Image::create(4, 4, ImageFormat::R8);

        const Result<ImageComparison> sizes = compareImages(small, large);
        REQUIRE_FALSE(sizes.has_value());
        CHECK(sizes.error().code == ErrorCode::InvalidArgument);
        CHECK(sizes.error().message.find("4x4") != std::string::npos);

        const Result<ImageComparison> formats = compareImages(small, grey);
        REQUIRE_FALSE(formats.has_value());
        CHECK(formats.error().code == ErrorCode::InvalidArgument);

        CHECK_FALSE(compareImages(Image(), Image()).has_value());
    }

    TEST_CASE("visualizeDifference marks differing pixels in red") {
        const Image expected = makeSolid(200, 200, 200);
        Image actual = makeSolid(200, 200, 200);
        setRed(actual, 5, 5, 0);

        const Result<Image> visual = visualizeDifference(actual, expected, 4);
        REQUIRE(visual.has_value());
        CHECK(visual->width() == 10);
        CHECK(visual->height() == 10);

        // The differing pixel is pure red at full strength (128 + 200 saturates).
        CHECK(visual->pixel(5, 5) == Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        // Matching pixels are a dimmed copy of the expected image.
        CHECK_NEAR(visual->pixel(0, 0), Vec4(50.0f / 255.0f, 50.0f / 255.0f, 50.0f / 255.0f, 1.0f), 1e-6f);

        CHECK_FALSE(visualizeDifference(actual, *Image::create(2, 2, ImageFormat::Rgba8), 4).has_value());
    }
}
