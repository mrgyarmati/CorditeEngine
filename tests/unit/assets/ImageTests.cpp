#include "cordite/assets/Image.h"

#include "cordite/core/FileSystem.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::AssertionFailure;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

std::vector<std::byte> bytesOf(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

/// A 4x3 image with a different value in every channel of every pixel.
Image makeGradient(ImageFormat format) {
    Image image = unwrap(Image::create(4, 3, format));
    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            image.setPixel(x, y, Vec4(static_cast<float>(x) / 3.0f, static_cast<float>(y) / 2.0f, 0.25f, 0.75f));
        }
    }
    return image;
}

/// The first bytes of a PNG file claiming the given dimensions. Enough for a decoder to
/// read the size; there is no pixel data behind it.
std::vector<std::byte> pngHeader(uint32_t width, uint32_t height) {
    std::vector<uint8_t> data = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (const uint32_t value : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            data.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    // Bit depth 8, RGBA, default compression, filter and interlace; then a dummy CRC.
    for (const uint8_t value : std::initializer_list<uint8_t>{8, 6, 0, 0, 0, 0, 0, 0, 0}) {
        data.push_back(value);
    }

    std::vector<std::byte> bytes(data.size());
    std::memcpy(bytes.data(), data.data(), data.size());
    return bytes;
}

} // namespace

TEST_SUITE("Image") {
    TEST_CASE("format properties") {
        CHECK(channelCount(ImageFormat::R8) == 1);
        CHECK(channelCount(ImageFormat::Rg8) == 2);
        CHECK(channelCount(ImageFormat::Rgba8) == 4);
        CHECK(channelCount(ImageFormat::Rgba32F) == 4);
        CHECK(bytesPerPixel(ImageFormat::R8) == 1);
        CHECK(bytesPerPixel(ImageFormat::Rg8) == 2);
        CHECK(bytesPerPixel(ImageFormat::Rgba8) == 4);
        CHECK(bytesPerPixel(ImageFormat::Rgba32F) == 16);
        CHECK(nameOf(ImageFormat::Rgba32F) == "Rgba32F");
    }

    TEST_CASE("a default image is empty") {
        const Image image;
        CHECK(image.isEmpty());
        CHECK(image.width() == 0);
        CHECK(image.height() == 0);
        CHECK(image.pixels().empty());
    }

    TEST_CASE("create makes a zero-filled image of the right size") {
        const Result<Image> image = Image::create(5, 3, ImageFormat::Rgba8);
        REQUIRE(image.has_value());
        CHECK(image->width() == 5);
        CHECK(image->height() == 3);
        CHECK(image->rowBytes() == 20);
        CHECK(image->pixels().size() == 60);
        CHECK(image->pixel(4, 2) == Vec4(0.0f));
    }

    TEST_CASE("create rejects zero and oversized dimensions") {
        const Result<Image> zero = Image::create(0, 10, ImageFormat::Rgba8);
        REQUIRE_FALSE(zero.has_value());
        CHECK(zero.error().code == ErrorCode::InvalidArgument);

        const Result<Image> huge = Image::create(kMaxImageDimension + 1, 1, ImageFormat::R8);
        REQUIRE_FALSE(huge.has_value());
        CHECK(huge.error().code == ErrorCode::OutOfRange);

        CHECK(Image::create(kMaxImageDimension, 1, ImageFormat::R8).has_value());
    }

    TEST_CASE("fromPixels requires exactly the right amount of data") {
        CHECK(Image::fromPixels(2, 2, ImageFormat::Rgba8, std::vector<std::byte>(16)).has_value());

        const Result<Image> tooSmall = Image::fromPixels(2, 2, ImageFormat::Rgba8, std::vector<std::byte>(15));
        REQUIRE_FALSE(tooSmall.has_value());
        CHECK(tooSmall.error().code == ErrorCode::InvalidArgument);

        CHECK_FALSE(Image::fromPixels(2, 2, ImageFormat::Rgba8, std::vector<std::byte>(17)).has_value());
        CHECK_FALSE(Image::fromPixels(0, 2, ImageFormat::Rgba8, {}).has_value());
    }

    TEST_CASE("pixels round-trip through setPixel and pixel in every format") {
        Image rgba = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        rgba.setPixel(1, 0, Vec4(1.0f, 0.5f, 0.0f, 0.25f));
        CHECK_NEAR(rgba.pixel(1, 0), Vec4(1.0f, 128.0f / 255.0f, 0.0f, 64.0f / 255.0f), 1e-6f);
        CHECK(rgba.pixel(0, 0) == Vec4(0.0f));

        // Formats without blue or alpha read those channels as 0 and 1.
        Image red = unwrap(Image::create(1, 1, ImageFormat::R8));
        red.setPixel(0, 0, Vec4(1.0f, 0.3f, 0.3f, 0.3f));
        CHECK(red.pixel(0, 0) == Vec4(1.0f, 0.0f, 0.0f, 1.0f));

        Image redGreen = unwrap(Image::create(1, 1, ImageFormat::Rg8));
        redGreen.setPixel(0, 0, Vec4(0.0f, 1.0f, 0.3f, 0.3f));
        CHECK(redGreen.pixel(0, 0) == Vec4(0.0f, 1.0f, 0.0f, 1.0f));

        // Float images keep values outside [0, 1].
        Image hdr = unwrap(Image::create(1, 1, ImageFormat::Rgba32F));
        hdr.setPixel(0, 0, Vec4(12.5f, -1.0f, 0.001f, 2.0f));
        CHECK(hdr.pixel(0, 0) == Vec4(12.5f, -1.0f, 0.001f, 2.0f));
    }

    TEST_CASE("8-bit pixels are clamped") {
        Image image = unwrap(Image::create(1, 1, ImageFormat::Rgba8));
        image.setPixel(0, 0, Vec4(2.0f, -1.0f, 0.5f, 99.0f));
        CHECK_NEAR(image.pixel(0, 0), Vec4(1.0f, 0.0f, 128.0f / 255.0f, 1.0f), 1e-6f);
    }

    TEST_CASE("PNG encoding and decoding preserves Rgba8 pixels exactly") {
        const Image original = makeGradient(ImageFormat::Rgba8);
        const Result<std::vector<std::byte>> encoded = original.encodePng();
        REQUIRE(encoded.has_value());
        CHECK(encoded->size() > 8);
        CHECK((*encoded)[1] == std::byte{'P'});

        const Result<Image> decoded = Image::decode(*encoded, "memory.png");
        REQUIRE_MESSAGE(decoded.has_value(), test::errorOf(decoded));
        CHECK(decoded->format() == ImageFormat::Rgba8);
        CHECK(decoded->width() == 4);
        CHECK(decoded->height() == 3);
        CHECK(std::equal(decoded->pixels().begin(), decoded->pixels().end(), original.pixels().begin(),
                         original.pixels().end()));
    }

    TEST_CASE("one- and two-channel PNGs decode to grey and grey-alpha Rgba8") {
        Image grey = unwrap(Image::create(2, 1, ImageFormat::R8));
        grey.setPixel(0, 0, Vec4(0.0f));
        grey.setPixel(1, 0, Vec4(1.0f));
        const Image decodedGrey = *Image::decode(*grey.encodePng(), "grey.png");
        CHECK(decodedGrey.format() == ImageFormat::Rgba8);
        CHECK(decodedGrey.pixel(0, 0) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        CHECK(decodedGrey.pixel(1, 0) == Vec4(1.0f));

        Image greyAlpha = unwrap(Image::create(1, 1, ImageFormat::Rg8));
        greyAlpha.setPixel(0, 0, Vec4(1.0f, 0.0f, 0.0f, 0.0f));
        const Image decodedGreyAlpha = *Image::decode(*greyAlpha.encodePng(), "grey-alpha.png");
        CHECK(decodedGreyAlpha.pixel(0, 0) == Vec4(1.0f, 1.0f, 1.0f, 0.0f));
    }

    TEST_CASE("images save to and load from disk") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "textures" / "gradient.png";
        const Image original = makeGradient(ImageFormat::Rgba8);

        REQUIRE(original.savePng(file).has_value());
        const Result<Image> loaded = Image::load(file);
        REQUIRE_MESSAGE(loaded.has_value(), test::errorOf(loaded));
        CHECK(std::equal(loaded->pixels().begin(), loaded->pixels().end(), original.pixels().begin(),
                         original.pixels().end()));
    }

    TEST_CASE("Radiance HDR data decodes to linear floats") {
        // Two flat (non run-length encoded) RGBE pixels: (1, 1, 1) and (0.5, 0.25, 0).
        std::vector<std::byte> data = bytesOf("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
        for (const uint8_t value : std::initializer_list<uint8_t>{128, 128, 128, 129, 128, 64, 0, 128}) {
            data.push_back(static_cast<std::byte>(value));
        }

        const Result<Image> image = Image::decode(data, "tiny.hdr");
        REQUIRE_MESSAGE(image.has_value(), test::errorOf(image));
        CHECK(image->format() == ImageFormat::Rgba32F);
        CHECK(image->width() == 2);
        CHECK(image->height() == 1);
        CHECK_NEAR(image->pixel(0, 0), Vec4(1.0f, 1.0f, 1.0f, 1.0f), 1e-5f);
        CHECK_NEAR(image->pixel(1, 0), Vec4(0.5f, 0.25f, 0.0f, 1.0f), 1e-5f);
    }

    TEST_CASE("Radiance HDR encoding and decoding preserves light to within a part in a hundred") {
        // From deep shadow to a sun, in every color: far more than eight bits hold.
        Image image = unwrap(Image::create(4, 2, ImageFormat::Rgba32F));
        const std::vector<Vec3> lights = {Vec3(0.0f),
                                          Vec3(0.001f, 0.002f, 0.003f),
                                          Vec3(0.18f),
                                          Vec3(1.0f, 0.5f, 0.25f),
                                          Vec3(12.5f, 3.0f, 0.75f),
                                          Vec3(1000.0f, 800.0f, 600.0f),
                                          Vec3(0.0f, 0.0f, 50000.0f),
                                          Vec3(65000.0f)};
        for (uint32_t index = 0; index < 8; ++index) {
            image.setPixel(index % 4, index / 4, Vec4(lights[index], 0.5f));
        }

        const Result<std::vector<std::byte>> encoded = image.encodeHdr();
        REQUIRE(encoded.has_value());
        const Result<Image> decoded = Image::decode(*encoded, "light.hdr");
        REQUIRE(decoded.has_value());
        CHECK(decoded->width() == 4);
        CHECK(decoded->height() == 2);
        CHECK(decoded->format() == ImageFormat::Rgba32F);
        for (uint32_t index = 0; index < 8; ++index) {
            CAPTURE(index);
            const Vec4 pixel = decoded->pixel(index % 4, index / 4);
            // A pixel has one exponent for its three colors: each is right to a 256th of
            // the largest of them.
            const float largest = std::max({lights[index].r, lights[index].g, lights[index].b});
            CHECK_NEAR(Vec3(pixel), lights[index], largest / 128.0f + 1.0e-6f);
            // Alpha is not stored; it comes back as "all there".
            CHECK(pixel.a == 1.0f);
        }
    }

    TEST_CASE("Radiance HDR files are saved and loaded, and refuse what they cannot hold") {
        const TempDirectory directory;
        Image image = unwrap(Image::create(2, 1, ImageFormat::Rgba32F));
        image.setPixel(0, 0, Vec4(4.0f, 2.0f, 1.0f, 1.0f));
        // Less than nothing is stored as nothing.
        image.setPixel(1, 0, Vec4(-3.0f, 0.5f, -0.25f, 1.0f));
        const std::filesystem::path path = directory.path() / "sky.hdr";
        REQUIRE(image.saveHdr(path).has_value());
        const Result<Image> loaded = Image::load(path);
        REQUIRE(loaded.has_value());
        CHECK_NEAR(Vec3(loaded->pixel(0, 0)), Vec3(4.0f, 2.0f, 1.0f), 0.04f);
        CHECK_NEAR(Vec3(loaded->pixel(1, 0)), Vec3(0.0f, 0.5f, 0.0f), 0.005f);

        // Not an image of light, no image at all, and light that is no number.
        const Result<std::vector<std::byte>> eightBit = unwrap(Image::create(2, 2, ImageFormat::Rgba8)).encodeHdr();
        REQUIRE_FALSE(eightBit.has_value());
        CHECK(eightBit.error().code == ErrorCode::Unsupported);
        const Result<std::vector<std::byte>> empty = Image().encodeHdr();
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == ErrorCode::InvalidState);
        for (const float broken : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity()}) {
            Image bad = unwrap(Image::create(3, 2, ImageFormat::Rgba32F));
            bad.setPixel(2, 1, Vec4(1.0f, broken, 1.0f, 1.0f));
            const Result<std::vector<std::byte>> refused = bad.encodeHdr();
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == ErrorCode::InvalidArgument);
            CHECK(refused.error().message.find("(2, 1)") != std::string::npos);
            CHECK_FALSE(bad.saveHdr(directory.path() / "bad.hdr").has_value());
        }
        // Where a directory is, no file can be written.
        CHECK_FALSE(image.saveHdr(directory.path()).has_value());
    }

    TEST_CASE("encodePng refuses what PNG cannot store") {
        const Result<std::vector<std::byte>> empty = Image().encodePng();
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == ErrorCode::InvalidState);

        const Image hdr = unwrap(Image::create(1, 1, ImageFormat::Rgba32F));
        const Result<std::vector<std::byte>> floatImage = hdr.encodePng();
        REQUIRE_FALSE(floatImage.has_value());
        CHECK(floatImage.error().code == ErrorCode::Unsupported);
    }

    TEST_CASE("decoding rejects data that is not an image") {
        const Result<Image> empty = Image::decode({}, "empty.png");
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error().code == ErrorCode::ParseError);
        CHECK(empty.error().message.find("empty.png") != std::string::npos);

        const Result<Image> text = Image::decode(bytesOf("this is not an image at all"), "notes.txt");
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error().code == ErrorCode::ParseError);
        CHECK(text.error().message.find("notes.txt") != std::string::npos);
    }

    TEST_CASE("truncated PNG data fails cleanly at every length") {
        const std::vector<std::byte> encoded = *makeGradient(ImageFormat::Rgba8).encodePng();
        for (size_t length = 0; length < encoded.size(); ++length) {
            const Result<Image> result =
                Image::decode(std::span<const std::byte>(encoded.data(), length), "truncated.png");
            // A prefix may happen to decode (the trailing chunks are optional), but it
            // must never crash, and whatever it returns must be self-consistent.
            if (result) {
                CHECK(result->pixels().size() == static_cast<size_t>(result->width()) * result->height() * 4);
            }
        }
    }

    TEST_CASE("an image claiming enormous dimensions is rejected before allocating") {
        const Result<Image> wide = Image::decode(pngHeader(kMaxImageDimension + 1, 16), "wide.png");
        REQUIRE_FALSE(wide.has_value());

        const Result<Image> gigantic = Image::decode(pngHeader(0x7fffffff, 0x7fffffff), "gigantic.png");
        REQUIRE_FALSE(gigantic.has_value());
    }

    TEST_CASE("loading a missing file reports NotFound") {
        TempDirectory directory;
        const Result<Image> image = Image::load(directory.path() / "missing.png");
        REQUIRE_FALSE(image.has_value());
        CHECK(image.error().code == ErrorCode::NotFound);
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("out-of-range pixel access is a programmer error") {
        Image image = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        CHECK_THROWS_AS((void)image.pixel(2, 0), AssertionFailure);
        CHECK_THROWS_AS(image.setPixel(0, 2, Vec4(1.0f)), AssertionFailure);
    }
#endif
}
