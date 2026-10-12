#include "cordite/renderer/PostProcessSettings.h"

#include "cordite/core/StringUtils.h"
#include "cordite/math/Color.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// The grey a tone mapper leaves where it is, and contrast turns about: what a sheet
/// that reflects 18% of the light shows.
constexpr float kMidGrey = 0.18f;

/// Light is multiplied by these before it goes through the curves, so that mid-grey
/// comes out as mid-grey. Match kFilmicScale and kAcesScale in
/// shaders/common/ToneMapping.slang.
constexpr float kFilmicScale = 2.9275f;
constexpr float kAcesScale = 0.7232f;
/// What the filmic curve takes for white, after the scale: light four times as bright
/// as a white sheet.
constexpr float kFilmicWhite = 11.2f;

/// `value` between `low` and `high`, or `fallback` when it is not a number.
float within(float value, float low, float high, float fallback) {
    return std::isnan(value) ? fallback : std::clamp(value, low, high);
}

Vec3 within(const Vec3& value, float low, float high, const Vec3& fallback) {
    return {within(value.x, low, high, fallback.x), within(value.y, low, high, fallback.y),
            within(value.z, low, high, fallback.z)};
}

/// J. Hable's curve, before it is scaled to end at white.
float hable(float x) {
    constexpr float kShoulder = 0.15f;
    constexpr float kLinear = 0.50f;
    constexpr float kLinearAngle = 0.10f;
    constexpr float kToe = 0.20f;
    constexpr float kToeNumerator = 0.02f;
    constexpr float kToeDenominator = 0.30f;
    return (x * (kShoulder * x + kLinearAngle * kLinear) + kToe * kToeNumerator) /
               (x * (kShoulder * x + kLinear) + kToe * kToeDenominator) -
           kToeNumerator / kToeDenominator;
}

float filmic(float x) {
    return hable(x * kFilmicScale) / hable(kFilmicWhite);
}

/// K. Narkowicz's fit of the ACES reference rendering and output transforms.
float aces(float x) {
    const float scaled = x * kAcesScale;
    return (scaled * (2.51f * scaled + 0.03f)) / (scaled * (2.43f * scaled + 0.59f) + 0.14f);
}

float mix(float from, float to, float amount) {
    return from + (to - from) * amount;
}

} // namespace

std::string_view nameOf(ToneMapper toneMapper) {
    switch (toneMapper) {
        case ToneMapper::Filmic:
            return "filmic";
        case ToneMapper::Aces:
            return "aces";
        case ToneMapper::Linear:
            return "linear";
    }
    return "unknown";
}

std::string_view nameOf(ExposureMode mode) {
    switch (mode) {
        case ExposureMode::Manual:
            return "manual";
        case ExposureMode::Automatic:
            return "automatic";
    }
    return "unknown";
}

std::string_view nameOf(PostProcessQuality quality) {
    switch (quality) {
        case PostProcessQuality::Off:
            return "off";
        case PostProcessQuality::Low:
            return "low";
        case PostProcessQuality::Medium:
            return "medium";
        case PostProcessQuality::High:
            return "high";
    }
    return "unknown";
}

std::string_view displayNameOf(PostProcessQuality quality) {
    switch (quality) {
        case PostProcessQuality::Off:
            return "Off";
        case PostProcessQuality::Low:
            return "Low";
        case PostProcessQuality::Medium:
            return "Medium";
        case PostProcessQuality::High:
            return "High";
    }
    return "Unknown";
}

std::optional<PostProcessQuality> postProcessQualityFromName(std::string_view name) {
    for (const PostProcessQuality quality : kPostProcessQualities) {
        if (equalsIgnoreCase(name, nameOf(quality))) {
            return quality;
        }
    }
    return std::nullopt;
}

PostProcessSettings sanitized(const PostProcessSettings& settings) {
    const PostProcessSettings defaults;
    PostProcessSettings result = settings;
    result.exposure = within(settings.exposure, -16.0f, 16.0f, defaults.exposure);
    result.autoExposureMinimum = within(settings.autoExposureMinimum, -16.0f, 16.0f, defaults.autoExposureMinimum);
    // The most is never less than the fewest, whichever of the two had to be replaced.
    result.autoExposureMaximum = std::max(
        within(settings.autoExposureMaximum, -16.0f, 16.0f, defaults.autoExposureMaximum), result.autoExposureMinimum);
    result.adaptationSpeedUp = within(settings.adaptationSpeedUp, 0.0f, 100.0f, defaults.adaptationSpeedUp);
    result.adaptationSpeedDown = within(settings.adaptationSpeedDown, 0.0f, 100.0f, defaults.adaptationSpeedDown);
    result.bloomIntensity = within(settings.bloomIntensity, 0.0f, 4.0f, defaults.bloomIntensity);
    result.bloomThreshold = within(settings.bloomThreshold, 0.0f, 64.0f, defaults.bloomThreshold);
    result.bloomScatter = within(settings.bloomScatter, 0.0f, 1.0f, defaults.bloomScatter);
    result.temperature = within(settings.temperature, -1.0f, 1.0f, defaults.temperature);
    result.tint = within(settings.tint, -1.0f, 1.0f, defaults.tint);
    result.contrast = within(settings.contrast, 0.5f, 2.0f, defaults.contrast);
    result.saturation = within(settings.saturation, 0.0f, 2.0f, defaults.saturation);
    result.lift = within(settings.lift, -1.0f, 1.0f, defaults.lift);
    result.gamma = within(settings.gamma, 0.2f, 5.0f, defaults.gamma);
    result.gain = within(settings.gain, 0.0f, 4.0f, defaults.gain);
    result.colorLutIntensity = within(settings.colorLutIntensity, 0.0f, 1.0f, defaults.colorLutIntensity);
    result.vignette = within(settings.vignette, 0.0f, 1.0f, defaults.vignette);
    result.chromaticAberration = within(settings.chromaticAberration, 0.0f, 1.0f, defaults.chromaticAberration);
    result.filmGrain = within(settings.filmGrain, 0.0f, 1.0f, defaults.filmGrain);
    result.ambientOcclusionIntensity =
        within(settings.ambientOcclusionIntensity, 0.0f, 4.0f, defaults.ambientOcclusionIntensity);
    result.ambientOcclusionRadius =
        within(settings.ambientOcclusionRadius, 0.01f, 10.0f, defaults.ambientOcclusionRadius);
    result.ambientOcclusionBias = within(settings.ambientOcclusionBias, 0.0f, 30.0f, defaults.ambientOcclusionBias);
    return result;
}

PostProcessSettings blend(const PostProcessSettings& from, const PostProcessSettings& to, float amount) {
    const float t = within(amount, 0.0f, 1.0f, 0.0f);
    const bool takeNew = t >= 0.5f;

    PostProcessSettings result;
    result.exposureMode = takeNew ? to.exposureMode : from.exposureMode;
    result.exposure = mix(from.exposure, to.exposure, t);
    result.autoExposureMinimum = mix(from.autoExposureMinimum, to.autoExposureMinimum, t);
    result.autoExposureMaximum = mix(from.autoExposureMaximum, to.autoExposureMaximum, t);
    result.adaptationSpeedUp = mix(from.adaptationSpeedUp, to.adaptationSpeedUp, t);
    result.adaptationSpeedDown = mix(from.adaptationSpeedDown, to.adaptationSpeedDown, t);
    result.toneMapper = takeNew ? to.toneMapper : from.toneMapper;
    result.bloomIntensity = mix(from.bloomIntensity, to.bloomIntensity, t);
    result.bloomThreshold = mix(from.bloomThreshold, to.bloomThreshold, t);
    result.bloomScatter = mix(from.bloomScatter, to.bloomScatter, t);
    result.temperature = mix(from.temperature, to.temperature, t);
    result.tint = mix(from.tint, to.tint, t);
    result.contrast = mix(from.contrast, to.contrast, t);
    result.saturation = mix(from.saturation, to.saturation, t);
    result.lift = glm::mix(from.lift, to.lift, t);
    result.gamma = glm::mix(from.gamma, to.gamma, t);
    result.gain = glm::mix(from.gain, to.gain, t);
    result.vignette = mix(from.vignette, to.vignette, t);
    result.chromaticAberration = mix(from.chromaticAberration, to.chromaticAberration, t);
    result.filmGrain = mix(from.filmGrain, to.filmGrain, t);
    result.ambientOcclusionIntensity = mix(from.ambientOcclusionIntensity, to.ambientOcclusionIntensity, t);
    result.ambientOcclusionRadius = mix(from.ambientOcclusionRadius, to.ambientOcclusionRadius, t);
    result.ambientOcclusionBias = mix(from.ambientOcclusionBias, to.ambientOcclusionBias, t);

    // Two tables cannot be blended into one without making a third. The same table on
    // both sides blends its intensity; otherwise the one fades out towards the middle
    // and the other in from there - with no table on a side, there is no fade on it.
    const float fromIntensity = from.colorLut.isNil() ? 0.0f : from.colorLutIntensity;
    const float toIntensity = to.colorLut.isNil() ? 0.0f : to.colorLutIntensity;
    if (from.colorLut == to.colorLut) {
        result.colorLut = to.colorLut;
        result.colorLutIntensity = mix(from.colorLutIntensity, to.colorLutIntensity, t);
    } else if (from.colorLut.isNil()) {
        result.colorLut = to.colorLut;
        result.colorLutIntensity = toIntensity * t;
    } else if (to.colorLut.isNil()) {
        result.colorLut = from.colorLut;
        result.colorLutIntensity = fromIntensity * (1.0f - t);
    } else {
        result.colorLut = takeNew ? to.colorLut : from.colorLut;
        result.colorLutIntensity = takeNew ? toIntensity * (2.0f * t - 1.0f) : fromIntensity * (1.0f - 2.0f * t);
    }
    return result;
}

float exposureScale(float stops) {
    return std::exp2(stops);
}

Vec3 whiteBalanceScale(float temperature, float tint) {
    // Warmer is more red and less blue by the same factor, a stop apart at the most;
    // towards magenta is less green. Then all three so that white is as bright as before.
    const float warmth = within(temperature, -1.0f, 1.0f, 0.0f);
    const float magenta = within(tint, -1.0f, 1.0f, 0.0f);
    const Vec3 scale(std::exp2(0.5f * warmth), std::exp2(-0.5f * magenta), std::exp2(-0.5f * warmth));
    return scale / luminance(scale);
}

Vec3 applyToneMapper(ToneMapper toneMapper, const Vec3& light) {
    const Vec3 positive = glm::max(light, Vec3(0.0f));
    Vec3 shown = positive;
    if (toneMapper == ToneMapper::Filmic) {
        shown = Vec3(filmic(positive.r), filmic(positive.g), filmic(positive.b));
    } else if (toneMapper == ToneMapper::Aces) {
        shown = Vec3(aces(positive.r), aces(positive.g), aces(positive.b));
    }
    return glm::clamp(shown, Vec3(0.0f), Vec3(1.0f));
}

Vec3 applyGrading(const PostProcessSettings& settings, const Vec3& color) {
    Vec3 graded = glm::max(color, Vec3(0.0f));
    graded = glm::max(glm::mix(Vec3(luminance(graded)), graded, settings.saturation), Vec3(0.0f));
    graded = kMidGrey * glm::pow(graded / kMidGrey, Vec3(settings.contrast));
    graded = glm::pow(glm::max(settings.gain * (graded + settings.lift * (Vec3(1.0f) - graded)), Vec3(0.0f)),
                      Vec3(1.0f) / settings.gamma);
    return glm::clamp(graded, Vec3(0.0f), Vec3(1.0f));
}

float vignetteScale(float vignette, const Vec2& fromCentre) {
    // Nothing in the middle, all of it in the corners, where the square of the distance
    // is two.
    return 1.0f - within(vignette, 0.0f, 1.0f, 0.0f) * std::min(glm::dot(fromCentre, fromCentre) * 0.5f, 1.0f);
}

PostProcessQualitySettings postProcessSettingsFor(PostProcessQuality quality) {
    switch (quality) {
        case PostProcessQuality::Off:
        case PostProcessQuality::Low:
            return {.bloomFirstLevel = 1, .bloomLevels = 4, .lensEffects = false};
        case PostProcessQuality::Medium:
            return {.bloomFirstLevel = 0, .bloomLevels = 5, .lensEffects = true};
        case PostProcessQuality::High:
            return {.bloomFirstLevel = 0, .bloomLevels = 7, .lensEffects = true};
    }
    return {};
}

} // namespace cordite
