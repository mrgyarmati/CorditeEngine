#pragma once

/// @file
/// Mipmaps: the chain of ever smaller copies of an image that keeps a texture from
/// shimmering when it is drawn small.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::vector<Image> smaller, generateMipChain(image, ColorSpace::Srgb));
/// upload(image, 0);
/// for (size_t level = 0; level < smaller.size(); ++level) {
///     upload(smaller[level], level + 1);
/// }
/// @endcode
///
/// Averaging pixels is only correct on linear values. Say which kind of data the image
/// holds: colors are converted from sRGB before they are averaged and back afterwards,
/// and weighted by their alpha so that transparent pixels do not bleed in; data (normals,
/// roughness) is averaged as it is, channel by channel.

#include "cordite/assets/Image.h"
#include "cordite/core/Result.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cordite {

/// How the numbers in an image relate to light.
enum class ColorSpace : uint8_t {
    /// Proportional to light, or not light at all: normals, roughness, masks.
    Linear,
    /// Display-encoded color, as painted and photographed images are stored.
    Srgb,
};

std::string_view nameOf(ColorSpace colorSpace);

/// The number of images in a full chain for a base of this size, the base included.
/// Each level halves both sides, rounding down but never below 1, until both are 1.
[[nodiscard]] uint32_t mipLevelCount(uint32_t width, uint32_t height);

/// The size of mip level `level` of an image, level 0 being the image itself.
[[nodiscard]] UVec2 mipLevelSize(uint32_t width, uint32_t height, uint32_t level);

/// Returns the levels below `image`, from half its size down to 1x1, in its format.
/// A 1x1 image yields none. Fails for an empty image.
[[nodiscard]] Result<std::vector<Image>> generateMipChain(const Image& image, ColorSpace colorSpace);

} // namespace cordite
