#include "cordite/math/Color.h"

#include "cordite/core/StringUtils.h"

#include <fmt/format.h>

#include <algorithm>

namespace cordite {
namespace {

uint32_t toByte(float value) {
    return static_cast<uint32_t>(std::lround(saturate(value) * 255.0f));
}

std::optional<float> parseHexByte(std::string_view text) {
    const int high = hexDigitValue(text[0]);
    const int low = hexDigitValue(text[1]);
    if (high < 0 || low < 0) {
        return std::nullopt;
    }
    return static_cast<float>(high * 16 + low) / 255.0f;
}

} // namespace

float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

Vec3 srgbToLinear(const Vec3& color) {
    return {srgbToLinear(color.r), srgbToLinear(color.g), srgbToLinear(color.b)};
}

float linearToSrgb(float value) {
    const float clamped = saturate(value);
    return clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
}

Vec3 linearToSrgb(const Vec3& color) {
    return {linearToSrgb(color.r), linearToSrgb(color.g), linearToSrgb(color.b)};
}

float luminance(const Vec3& linearColor) {
    return glm::dot(linearColor, Vec3(0.2126f, 0.7152f, 0.0722f));
}

Vec3 hsvToRgb(const Vec3& hsv) {
    const float hue = hsv.x - std::floor(hsv.x);
    const float saturation = saturate(hsv.y);
    const float value = hsv.z;

    const float scaled = hue * 6.0f;
    const int sector = static_cast<int>(scaled) % 6;
    const float fraction = scaled - std::floor(scaled);

    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - saturation * fraction);
    const float t = value * (1.0f - saturation * (1.0f - fraction));

    switch (sector) {
        case 0:
            return {value, t, p};
        case 1:
            return {q, value, p};
        case 2:
            return {p, value, t};
        case 3:
            return {p, q, value};
        case 4:
            return {t, p, value};
        default:
            return {value, p, q};
    }
}

Vec3 rgbToHsv(const Vec3& rgb) {
    const float maximum = std::max({rgb.r, rgb.g, rgb.b});
    const float minimum = std::min({rgb.r, rgb.g, rgb.b});
    const float chroma = maximum - minimum;

    float hue = 0.0f;
    if (chroma > kEpsilon) {
        if (maximum == rgb.r) {
            hue = std::fmod((rgb.g - rgb.b) / chroma, 6.0f);
        } else if (maximum == rgb.g) {
            hue = (rgb.b - rgb.r) / chroma + 2.0f;
        } else {
            hue = (rgb.r - rgb.g) / chroma + 4.0f;
        }
        hue /= 6.0f;
        if (hue < 0.0f) {
            hue += 1.0f;
        }
    }

    const float saturation = maximum > kEpsilon ? chroma / maximum : 0.0f;
    return {hue, saturation, maximum};
}

Vec3 colorTemperatureToRgb(float kelvin) {
    // Tanner Helland's curve fit of the Planckian locus. It yields display (sRGB)
    // values, which are converted to linear before returning.
    const float temperature = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;

    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;

    if (temperature <= 66.0f) {
        green = saturate((99.4708025861f * std::log(temperature) - 161.1195681661f) / 255.0f);
    } else {
        red = saturate(329.698727446f * std::pow(temperature - 60.0f, -0.1332047592f) / 255.0f);
        green = saturate(288.1221695283f * std::pow(temperature - 60.0f, -0.0755148492f) / 255.0f);
    }

    if (temperature <= 19.0f) {
        blue = 0.0f;
    } else if (temperature < 66.0f) {
        blue = saturate((138.5177312231f * std::log(temperature - 10.0f) - 305.0447927307f) / 255.0f);
    }

    const Vec3 linear = srgbToLinear(Vec3(red, green, blue));
    const float brightest = std::max({linear.r, linear.g, linear.b});
    return brightest > kEpsilon ? linear / brightest : Vec3(1.0f);
}

uint32_t packRgba8(const Vec4& color) {
    return toByte(color.r) | (toByte(color.g) << 8) | (toByte(color.b) << 16) | (toByte(color.a) << 24);
}

Vec4 unpackRgba8(uint32_t packed) {
    return Vec4(static_cast<float>(packed & 0xffu), static_cast<float>((packed >> 8) & 0xffu),
                static_cast<float>((packed >> 16) & 0xffu), static_cast<float>((packed >> 24) & 0xffu)) /
           255.0f;
}

std::optional<Vec4> parseHexColor(std::string_view text) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return std::nullopt;
    }

    const std::optional<float> red = parseHexByte(text.substr(1, 2));
    const std::optional<float> green = parseHexByte(text.substr(3, 2));
    const std::optional<float> blue = parseHexByte(text.substr(5, 2));
    const std::optional<float> alpha = text.size() == 9 ? parseHexByte(text.substr(7, 2)) : std::optional<float>(1.0f);
    if (!red || !green || !blue || !alpha) {
        return std::nullopt;
    }
    return Vec4(srgbToLinear(Vec3(*red, *green, *blue)), *alpha);
}

std::string toHexColor(const Vec4& linearColor) {
    const Vec3 srgb = linearToSrgb(Vec3(linearColor));
    return fmt::format("#{:02x}{:02x}{:02x}{:02x}", toByte(srgb.r), toByte(srgb.g), toByte(srgb.b),
                       toByte(linearColor.a));
}

} // namespace cordite
