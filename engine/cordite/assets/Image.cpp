#include "cordite/assets/Image.h"

#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"

// stb_image is compiled here, restricted to the formats the engine accepts and to a
// maximum size, so a malicious file cannot make it allocate arbitrary amounts of memory.
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// The writer keeps its functions that take a file name (they are never called: files
// are written through FileSystem.h, which knows about UTF-8 paths). Leaving them out
// with STBI_WRITE_NO_STDIO would leave the Radiance HDR writer out with them.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace cordite {

static_assert(kMaxImageDimension == STBI_MAX_DIMENSIONS, "keep the stb limit in sync with kMaxImageDimension");

namespace {

struct StbiDeleter {
    void operator()(void* pixels) const { stbi_image_free(pixels); }
};

Status validateDimensions(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return makeError(ErrorCode::InvalidArgument, "image dimensions must be positive, got {}x{}", width, height);
    }
    if (width > kMaxImageDimension || height > kMaxImageDimension) {
        return makeError(ErrorCode::OutOfRange, "image dimensions {}x{} exceed the limit of {}", width, height,
                         kMaxImageDimension);
    }
    return {};
}

size_t byteSize(uint32_t width, uint32_t height, ImageFormat format) {
    return static_cast<size_t>(width) * height * bytesPerPixel(format);
}

void appendToVector(void* context, void* data, int size) {
    auto* output = static_cast<std::vector<std::byte>*>(context);
    const auto* bytes = static_cast<const std::byte*>(data);
    output->insert(output->end(), bytes, bytes + size);
}

uint8_t toByte(float value) {
    return static_cast<uint8_t>(std::lround(saturate(value) * 255.0f));
}

} // namespace

std::string_view nameOf(ImageFormat format) {
    switch (format) {
        case ImageFormat::R8:
            return "R8";
        case ImageFormat::Rg8:
            return "Rg8";
        case ImageFormat::Rgba8:
            return "Rgba8";
        case ImageFormat::Rgba32F:
            return "Rgba32F";
    }
    return "Unknown";
}

uint32_t channelCount(ImageFormat format) {
    switch (format) {
        case ImageFormat::R8:
            return 1;
        case ImageFormat::Rg8:
            return 2;
        case ImageFormat::Rgba8:
        case ImageFormat::Rgba32F:
            return 4;
    }
    return 0;
}

uint32_t bytesPerPixel(ImageFormat format) {
    return format == ImageFormat::Rgba32F ? 16 : channelCount(format);
}

Result<Image> Image::create(uint32_t width, uint32_t height, ImageFormat format) {
    CORDITE_TRY(validateDimensions(width, height));
    return Image(width, height, format, std::vector<std::byte>(byteSize(width, height, format)));
}

Result<Image> Image::fromPixels(uint32_t width, uint32_t height, ImageFormat format, std::vector<std::byte> pixels) {
    CORDITE_TRY(validateDimensions(width, height));

    const size_t expected = byteSize(width, height, format);
    if (pixels.size() != expected) {
        return makeError(ErrorCode::InvalidArgument, "a {}x{} {} image needs {} bytes of pixel data, got {}", width,
                         height, nameOf(format), expected, pixels.size());
    }
    return Image(width, height, format, std::move(pixels));
}

Result<Image> Image::load(const std::filesystem::path& path) {
    CORDITE_TRY_ASSIGN(const std::vector<std::byte> encoded, readBinaryFile(path));
    return decode(encoded, pathToUtf8(path));
}

Result<Image> Image::decode(std::span<const std::byte> encoded, std::string_view sourceName) {
    if (encoded.empty()) {
        return makeError(ErrorCode::ParseError, "{}: the image data is empty", sourceName);
    }
    if (encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return makeError(ErrorCode::OutOfRange, "{}: the image data is larger than 2 GiB", sourceName);
    }

    const auto* data = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int size = static_cast<int>(encoded.size());
    constexpr int kChannels = 4;

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    const bool isHdr = stbi_is_hdr_from_memory(data, size) != 0;
    const ImageFormat format = isHdr ? ImageFormat::Rgba32F : ImageFormat::Rgba8;

    std::unique_ptr<void, StbiDeleter> decoded;
    if (isHdr) {
        decoded.reset(stbi_loadf_from_memory(data, size, &width, &height, &sourceChannels, kChannels));
    } else {
        decoded.reset(stbi_load_from_memory(data, size, &width, &height, &sourceChannels, kChannels));
    }
    if (!decoded) {
        return makeError(ErrorCode::ParseError, "{}: cannot decode image: {}", sourceName, stbi_failure_reason());
    }

    CORDITE_TRY(validateDimensions(static_cast<uint32_t>(width), static_cast<uint32_t>(height)));

    std::vector<std::byte> pixels(byteSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height), format));
    std::memcpy(pixels.data(), decoded.get(), pixels.size());
    return Image(static_cast<uint32_t>(width), static_cast<uint32_t>(height), format, std::move(pixels));
}

Result<std::vector<std::byte>> Image::encodePng() const {
    if (isEmpty()) {
        return makeError(ErrorCode::InvalidState, "cannot encode an empty image");
    }
    if (m_format == ImageFormat::Rgba32F) {
        return makeError(ErrorCode::Unsupported, "PNG cannot store {} images", nameOf(m_format));
    }

    std::vector<std::byte> encoded;
    const int result =
        stbi_write_png_to_func(&appendToVector, &encoded, static_cast<int>(m_width), static_cast<int>(m_height),
                               static_cast<int>(channelCount(m_format)), m_pixels.data(), static_cast<int>(rowBytes()));
    if (result == 0 || encoded.empty()) {
        return makeError(ErrorCode::IoError, "PNG encoding failed");
    }
    return encoded;
}

Status Image::savePng(const std::filesystem::path& path) const {
    CORDITE_TRY_ASSIGN(const std::vector<std::byte> encoded, encodePng());
    return writeBinaryFile(path, encoded);
}

Result<std::vector<std::byte>> Image::encodeHdr() const {
    if (isEmpty()) {
        return makeError(ErrorCode::InvalidState, "cannot encode an empty image");
    }
    if (m_format != ImageFormat::Rgba32F) {
        return makeError(ErrorCode::Unsupported, "Radiance HDR stores Rgba32F images, not {}", nameOf(m_format));
    }

    // The format has no way to write "not a number" or "without end", and nothing
    // below zero: the first two are refused, the last is written as nothing.
    constexpr size_t kChannels = 4;
    std::vector<float> values(static_cast<size_t>(m_width) * m_height * kChannels);
    std::memcpy(values.data(), m_pixels.data(), m_pixels.size());
    for (size_t index = 0; index < values.size(); ++index) {
        if (!std::isfinite(values[index])) {
            const size_t pixelIndex = index / kChannels;
            return makeError(ErrorCode::InvalidArgument,
                             "the pixel at ({}, {}) holds a value that is not finite, which Radiance HDR cannot store",
                             pixelIndex % m_width, pixelIndex / m_width);
        }
        values[index] = std::max(values[index], 0.0f);
    }

    std::vector<std::byte> encoded;
    const int result = stbi_write_hdr_to_func(&appendToVector, &encoded, static_cast<int>(m_width),
                                              static_cast<int>(m_height), static_cast<int>(kChannels), values.data());
    if (result == 0 || encoded.empty()) {
        return makeError(ErrorCode::IoError, "Radiance HDR encoding failed");
    }
    return encoded;
}

Status Image::saveHdr(const std::filesystem::path& path) const {
    CORDITE_TRY_ASSIGN(const std::vector<std::byte> encoded, encodeHdr());
    return writeBinaryFile(path, encoded);
}

Vec4 Image::pixel(uint32_t x, uint32_t y) const {
    CORDITE_ASSERT(x < m_width && y < m_height, "pixel ({}, {}) is outside a {}x{} image", x, y, m_width, m_height);
    if (x >= m_width || y >= m_height) {
        return Vec4(0.0f);
    }

    const std::byte* source = m_pixels.data() + (static_cast<size_t>(y) * m_width + x) * bytesPerPixel(m_format);
    Vec4 result(0.0f, 0.0f, 0.0f, 1.0f);
    if (m_format == ImageFormat::Rgba32F) {
        std::memcpy(&result, source, sizeof(float) * 4);
        return result;
    }

    const uint32_t channels = channelCount(m_format);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        result[static_cast<int>(channel)] = static_cast<float>(std::to_integer<uint8_t>(source[channel])) / 255.0f;
    }
    return result;
}

void Image::setPixel(uint32_t x, uint32_t y, const Vec4& value) {
    CORDITE_ASSERT(x < m_width && y < m_height, "pixel ({}, {}) is outside a {}x{} image", x, y, m_width, m_height);
    if (x >= m_width || y >= m_height) {
        return;
    }

    std::byte* destination = m_pixels.data() + (static_cast<size_t>(y) * m_width + x) * bytesPerPixel(m_format);
    if (m_format == ImageFormat::Rgba32F) {
        std::memcpy(destination, &value, sizeof(float) * 4);
        return;
    }

    const uint32_t channels = channelCount(m_format);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        destination[channel] = static_cast<std::byte>(toByte(value[static_cast<int>(channel)]));
    }
}

} // namespace cordite
