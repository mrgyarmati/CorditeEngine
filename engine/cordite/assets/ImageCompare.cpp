#include "cordite/assets/ImageCompare.h"

#include <algorithm>
#include <cstdlib>

namespace cordite {
namespace {

constexpr size_t kChannels = 4;

Status validateComparable(const Image& actual, const Image& expected) {
    if (actual.format() != ImageFormat::Rgba8 || expected.format() != ImageFormat::Rgba8) {
        return makeError(ErrorCode::InvalidArgument, "only Rgba8 images can be compared, got {} and {}",
                         nameOf(actual.format()), nameOf(expected.format()));
    }
    if (actual.isEmpty() || expected.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "cannot compare an empty image");
    }
    if (actual.width() != expected.width() || actual.height() != expected.height()) {
        return makeError(ErrorCode::InvalidArgument, "image sizes differ: {}x{} and {}x{}", actual.width(),
                         actual.height(), expected.width(), expected.height());
    }
    return {};
}

/// Largest channel difference between two pixels starting at `offset`, and their sum.
struct PixelDifference {
    uint32_t largest = 0;
    uint32_t sum = 0;
};

PixelDifference pixelDifference(std::span<const std::byte> a, std::span<const std::byte> b, size_t offset) {
    PixelDifference result;
    for (size_t channel = 0; channel < kChannels; ++channel) {
        const int difference =
            std::abs(std::to_integer<int>(a[offset + channel]) - std::to_integer<int>(b[offset + channel]));
        result.largest = std::max(result.largest, static_cast<uint32_t>(difference));
        result.sum += static_cast<uint32_t>(difference);
    }
    return result;
}

} // namespace

Result<ImageComparison> compareImages(const Image& actual, const Image& expected, const ImageCompareOptions& options) {
    CORDITE_TRY(validateComparable(actual, expected));

    const std::span<const std::byte> actualPixels = actual.pixels();
    const std::span<const std::byte> expectedPixels = expected.pixels();

    ImageComparison comparison;
    comparison.totalPixels = static_cast<uint64_t>(actual.width()) * actual.height();

    uint64_t differenceSum = 0;
    for (size_t offset = 0; offset < actualPixels.size(); offset += kChannels) {
        const PixelDifference difference = pixelDifference(actualPixels, expectedPixels, offset);
        comparison.maxChannelDifference = std::max(comparison.maxChannelDifference, difference.largest);
        differenceSum += difference.sum;
        if (difference.largest > options.channelTolerance) {
            ++comparison.differingPixels;
        }
    }

    comparison.meanDifference =
        static_cast<double>(differenceSum) / (static_cast<double>(comparison.totalPixels) * kChannels);
    comparison.matches = comparison.differingRatio() <= options.maxDifferingRatio;
    return comparison;
}

Result<Image> visualizeDifference(const Image& actual, const Image& expected, uint32_t channelTolerance) {
    CORDITE_TRY(validateComparable(actual, expected));
    CORDITE_TRY_ASSIGN(Image output, Image::create(actual.width(), actual.height(), ImageFormat::Rgba8));

    const std::span<const std::byte> actualPixels = actual.pixels();
    const std::span<const std::byte> expectedPixels = expected.pixels();
    const std::span<std::byte> outputPixels = output.pixels();

    for (size_t offset = 0; offset < actualPixels.size(); offset += kChannels) {
        const PixelDifference difference = pixelDifference(actualPixels, expectedPixels, offset);
        if (difference.largest > channelTolerance) {
            // Start at half brightness so that even small differences are easy to spot.
            const uint32_t intensity = std::min(255u, 128u + difference.largest);
            outputPixels[offset + 0] = static_cast<std::byte>(intensity);
            outputPixels[offset + 1] = std::byte{0};
            outputPixels[offset + 2] = std::byte{0};
        } else {
            for (size_t channel = 0; channel < 3; ++channel) {
                outputPixels[offset + channel] =
                    static_cast<std::byte>(std::to_integer<uint32_t>(expectedPixels[offset + channel]) / 4);
            }
        }
        outputPixels[offset + 3] = std::byte{255};
    }
    return output;
}

} // namespace cordite
