#pragma once

/// @file
/// CPU-side images: decoding, encoding and pixel access.
///
/// @code
/// CORDITE_TRY_ASSIGN(Image image, Image::load(projectDir / "textures/rust.png"));
/// const Vec4 corner = image.pixel(0, 0);
/// CORDITE_TRY(image.savePng(outputDir / "copy.png"));
/// @endcode
///
/// Image files are untrusted input. Decoding rejects images larger than
/// kMaxImageDimension on either side and never reads outside the encoded data.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace cordite {

/// Largest width or height Image will decode or create.
inline constexpr uint32_t kMaxImageDimension = 16384;

enum class ImageFormat : uint8_t {
    /// One 8-bit channel.
    R8,
    /// Two 8-bit channels.
    Rg8,
    /// Four 8-bit channels. Color data is sRGB-encoded unless the image says otherwise.
    Rgba8,
    /// Four 32-bit float channels holding linear values, for HDR images.
    Rgba32F,
};

std::string_view nameOf(ImageFormat format);
uint32_t channelCount(ImageFormat format);
uint32_t bytesPerPixel(ImageFormat format);

/// A rectangular grid of pixels stored row by row from the top, without padding.
class Image {
public:
    /// An empty 0x0 image.
    Image() = default;

    /// Creates a zero-filled image. Fails when a dimension is zero or exceeds
    /// kMaxImageDimension.
    [[nodiscard]] static Result<Image> create(uint32_t width, uint32_t height, ImageFormat format);

    /// Wraps existing pixel data, which must be exactly width * height * bytesPerPixel bytes.
    [[nodiscard]] static Result<Image> fromPixels(uint32_t width, uint32_t height, ImageFormat format,
                                                  std::vector<std::byte> pixels);

    /// Reads and decodes an image file. See decode().
    [[nodiscard]] static Result<Image> load(const std::filesystem::path& path);

    /// Decodes PNG, JPEG, TGA or BMP data into Rgba8, or Radiance HDR data into Rgba32F.
    /// `sourceName` is used in error messages.
    [[nodiscard]] static Result<Image> decode(std::span<const std::byte> encoded, std::string_view sourceName);

    /// Encodes as PNG. Only the 8-bit formats can be saved this way.
    [[nodiscard]] Result<std::vector<std::byte>> encodePng() const;
    [[nodiscard]] Status savePng(const std::filesystem::path& path) const;

    [[nodiscard]] uint32_t width() const { return m_width; }

    [[nodiscard]] uint32_t height() const { return m_height; }

    [[nodiscard]] ImageFormat format() const { return m_format; }

    [[nodiscard]] bool isEmpty() const { return m_pixels.empty(); }

    /// Bytes in one row of pixels.
    [[nodiscard]] size_t rowBytes() const { return static_cast<size_t>(m_width) * bytesPerPixel(m_format); }

    [[nodiscard]] std::span<const std::byte> pixels() const { return m_pixels; }

    [[nodiscard]] std::span<std::byte> pixels() { return m_pixels; }

    /// Returns a pixel as four floats. 8-bit channels map to [0, 1]; channels the format
    /// lacks read as 0, except alpha, which reads as 1. Coordinates must be in range.
    [[nodiscard]] Vec4 pixel(uint32_t x, uint32_t y) const;

    /// Writes a pixel from four floats, the inverse of pixel(). Values are clamped to
    /// [0, 1] for the 8-bit formats.
    void setPixel(uint32_t x, uint32_t y, const Vec4& value);

private:
    Image(uint32_t width, uint32_t height, ImageFormat format, std::vector<std::byte> pixels)
        : m_width(width), m_height(height), m_format(format), m_pixels(std::move(pixels)) {}

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    ImageFormat m_format = ImageFormat::Rgba8;
    std::vector<std::byte> m_pixels;
};

} // namespace cordite
