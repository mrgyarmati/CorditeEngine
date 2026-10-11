#pragma once

/// @file
/// Color utilities.
///
/// Lighting math is done on linear RGB. sRGB is only an encoding for storage and
/// display, so convert at the boundaries: when reading authored colors and 8-bit
/// textures, and when writing to an 8-bit target.
///
/// @code
/// const Vec3 lampColor = colorTemperatureToRgb(3200.0f);           // warm tungsten
/// const std::optional<Vec4> rust = parseHexColor("#8a3b1c");       // linear RGBA
/// const float brightness = luminance(Vec3(*rust));
/// @endcode

#include "cordite/math/Math.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {

/// Converts one sRGB-encoded channel in [0, 1] to linear.
[[nodiscard]] float srgbToLinear(float value);
[[nodiscard]] Vec3 srgbToLinear(const Vec3& color);

/// Converts one linear channel to sRGB encoding. Input is clamped to [0, 1].
[[nodiscard]] float linearToSrgb(float value);
[[nodiscard]] Vec3 linearToSrgb(const Vec3& color);

/// Relative luminance of a linear RGB color (Rec. 709 primaries).
[[nodiscard]] float luminance(const Vec3& linearColor);

/// Converts hue (in [0, 1), wrapping), saturation and value to RGB.
[[nodiscard]] Vec3 hsvToRgb(const Vec3& hsv);

/// Converts RGB to hue in [0, 1), saturation and value.
[[nodiscard]] Vec3 rgbToHsv(const Vec3& rgb);

/// Returns the linear RGB tint of a black-body light source, normalized so the
/// brightest channel is 1. `kelvin` is clamped to [1000, 40000]; about 6500 is neutral.
[[nodiscard]] Vec3 colorTemperatureToRgb(float kelvin);

/// Packs a color in [0, 1] into 0xAABBGGRR, the byte order R, G, B, A in memory.
[[nodiscard]] uint32_t packRgba8(const Vec4& color);
[[nodiscard]] Vec4 unpackRgba8(uint32_t packed);

/// Parses "#RRGGBB" or "#RRGGBBAA" (sRGB, as shown by color pickers) into linear RGBA.
/// Alpha is not gamma-encoded and defaults to 1.
[[nodiscard]] std::optional<Vec4> parseHexColor(std::string_view text);

/// Formats a linear RGBA color as "#RRGGBBAA" in sRGB.
[[nodiscard]] std::string toHexColor(const Vec4& linearColor);

} // namespace cordite
