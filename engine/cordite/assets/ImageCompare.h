#pragma once

/// @file
/// Image comparison for rendering tests and for checking automation output.
///
/// Rendered frames differ slightly between GPUs and driver versions, so tests compare
/// against a reference image with a tolerance rather than byte for byte.
///
/// @code
/// CORDITE_TRY_ASSIGN(const ImageComparison result, compareImages(rendered, reference, {.channelTolerance = 4}));
/// if (!result.matches) {
///     CORDITE_TRY(visualizeDifference(rendered, reference, 4)->savePng(outputDir / "diff.png"));
/// }
/// @endcode

#include "cordite/assets/Image.h"
#include "cordite/core/Result.h"

#include <cstdint>

namespace cordite {

struct ImageCompareOptions {
    /// A pixel differs when any channel is further apart than this many 8-bit steps.
    uint32_t channelTolerance = 2;
    /// Fraction of pixels allowed to differ, in [0, 1], before the images do not match.
    /// Covers the few edge pixels that rasterize differently between GPUs.
    double maxDifferingRatio = 0.0;
};

struct ImageComparison {
    /// True when the share of differing pixels is within maxDifferingRatio.
    bool matches = false;
    /// Largest difference found in any channel of any pixel, in 8-bit steps.
    uint32_t maxChannelDifference = 0;
    uint64_t differingPixels = 0;
    uint64_t totalPixels = 0;
    /// Mean absolute difference over all channels of all pixels, in 8-bit steps.
    double meanDifference = 0.0;

    [[nodiscard]] double differingRatio() const {
        return totalPixels > 0 ? static_cast<double>(differingPixels) / static_cast<double>(totalPixels) : 0.0;
    }
};

/// Compares two Rgba8 images of equal size. Fails with InvalidArgument when the formats
/// or sizes differ, which is a different kind of failure from a content mismatch.
[[nodiscard]] Result<ImageComparison> compareImages(const Image& actual, const Image& expected,
                                                    const ImageCompareOptions& options = {});

/// Builds an image showing where two images differ: matching pixels are a dimmed copy of
/// `expected`, differing pixels are red, brighter for larger differences.
[[nodiscard]] Result<Image> visualizeDifference(const Image& actual, const Image& expected, uint32_t channelTolerance);

} // namespace cordite
