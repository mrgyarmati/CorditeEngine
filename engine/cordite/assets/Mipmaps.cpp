#include "cordite/assets/Mipmaps.h"

#include "cordite/core/ExternalAssert.h"

// stb_image_resize2 is compiled here. Its assertions report through the engine.
#define STBIR_ASSERT(expression) CORDITE_EXTERNAL_ASSERT(expression)
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>

namespace cordite {
namespace {

/// How stb is to treat the channels. Only colors with an alpha channel are weighted by
/// it; in data textures the fourth channel is just another number.
stbir_pixel_layout pixelLayout(ImageFormat format, ColorSpace colorSpace) {
    switch (format) {
        case ImageFormat::R8:
            return STBIR_1CHANNEL;
        case ImageFormat::Rg8:
            return STBIR_2CHANNEL;
        case ImageFormat::Rgba8:
        case ImageFormat::Rgba32F:
            return colorSpace == ColorSpace::Srgb ? STBIR_RGBA : STBIR_4CHANNEL;
    }
    return STBIR_4CHANNEL;
}

/// Resizes `source` into `target`, which has the same format. False when stb ran out
/// of memory.
bool resizeInto(const Image& source, Image& target, ColorSpace colorSpace) {
    const auto sourceWidth = static_cast<int>(source.width());
    const auto sourceHeight = static_cast<int>(source.height());
    const auto sourceStride = static_cast<int>(source.rowBytes());
    const auto targetWidth = static_cast<int>(target.width());
    const auto targetHeight = static_cast<int>(target.height());
    const auto targetStride = static_cast<int>(target.rowBytes());
    const stbir_pixel_layout layout = pixelLayout(source.format(), colorSpace);

    if (source.format() == ImageFormat::Rgba32F) {
        // Floating-point images hold linear values whatever they are used for.
        return stbir_resize_float_linear(reinterpret_cast<const float*>(source.pixels().data()), sourceWidth,
                                         sourceHeight, sourceStride, reinterpret_cast<float*>(target.pixels().data()),
                                         targetWidth, targetHeight, targetStride, layout) != nullptr;
    }

    const auto* input = reinterpret_cast<const unsigned char*>(source.pixels().data());
    auto* output = reinterpret_cast<unsigned char*>(target.pixels().data());
    // A single channel is a mask or a height, never a color.
    if (colorSpace == ColorSpace::Srgb && source.format() == ImageFormat::Rgba8) {
        return stbir_resize_uint8_srgb(input, sourceWidth, sourceHeight, sourceStride, output, targetWidth,
                                       targetHeight, targetStride, layout) != nullptr;
    }
    return stbir_resize_uint8_linear(input, sourceWidth, sourceHeight, sourceStride, output, targetWidth, targetHeight,
                                     targetStride, layout) != nullptr;
}

} // namespace

std::string_view nameOf(ColorSpace colorSpace) {
    switch (colorSpace) {
        case ColorSpace::Linear:
            return "linear";
        case ColorSpace::Srgb:
            return "srgb";
    }
    return "unknown";
}

uint32_t mipLevelCount(uint32_t width, uint32_t height) {
    const uint32_t largest = std::max({width, height, 1u});
    return static_cast<uint32_t>(std::bit_width(largest));
}

UVec2 mipLevelSize(uint32_t width, uint32_t height, uint32_t level) {
    // Shifting by 32 or more is undefined; by then every side is 1 anyway.
    const uint32_t shift = std::min(level, 31u);
    return {std::max(width >> shift, 1u), std::max(height >> shift, 1u)};
}

Result<std::vector<Image>> generateMipChain(const Image& image, ColorSpace colorSpace) {
    if (image.isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "cannot make mipmaps of an empty image");
    }

    const uint32_t levelCount = mipLevelCount(image.width(), image.height());
    std::vector<Image> levels;
    levels.reserve(levelCount - 1);

    // Each level is made from the one above it, which is as good as starting from the
    // base every time and far cheaper.
    const Image* source = &image;
    for (uint32_t level = 1; level < levelCount; ++level) {
        const UVec2 size = mipLevelSize(image.width(), image.height(), level);
        CORDITE_TRY_ASSIGN(Image smaller, Image::create(size.x, size.y, image.format()));
        if (!resizeInto(*source, smaller, colorSpace)) {
            return makeError(ErrorCode::Unknown, "out of memory while making a {}x{} mipmap", size.x, size.y);
        }
        levels.push_back(std::move(smaller));
        source = &levels.back();
    }
    return levels;
}

} // namespace cordite
