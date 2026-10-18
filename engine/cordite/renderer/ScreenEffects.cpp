#include "cordite/renderer/ScreenEffects.h"

#include "cordite/core/StringUtils.h"

namespace cordite {

std::string_view nameOf(ScreenEffectsQuality quality) {
    switch (quality) {
        case ScreenEffectsQuality::Off:
            return "off";
        case ScreenEffectsQuality::Low:
            return "low";
        case ScreenEffectsQuality::Medium:
            return "medium";
        case ScreenEffectsQuality::High:
            return "high";
    }
    return "unknown";
}

std::string_view displayNameOf(ScreenEffectsQuality quality) {
    switch (quality) {
        case ScreenEffectsQuality::Off:
            return "Off";
        case ScreenEffectsQuality::Low:
            return "Low";
        case ScreenEffectsQuality::Medium:
            return "Medium";
        case ScreenEffectsQuality::High:
            return "High";
    }
    return "Unknown";
}

std::optional<ScreenEffectsQuality> screenEffectsQualityFromName(std::string_view name) {
    for (const ScreenEffectsQuality quality : kScreenEffectsQualities) {
        if (equalsIgnoreCase(name, nameOf(quality))) {
            return quality;
        }
    }
    return std::nullopt;
}

ScreenEffectsQualitySettings screenEffectsSettingsFor(ScreenEffectsQuality quality) {
    switch (quality) {
        case ScreenEffectsQuality::Off:
        case ScreenEffectsQuality::Low:
            return {.ambientOcclusionDivisor = 2,
                    .ambientOcclusionSlices = 2,
                    .ambientOcclusionSteps = 2,
                    .fogVolumeTile = 16,
                    .fogVolumeMaxCells = 120,
                    .fogVolumeSlices = 48};
        case ScreenEffectsQuality::Medium:
            return {.ambientOcclusionDivisor = 2,
                    .ambientOcclusionSlices = 2,
                    .ambientOcclusionSteps = 4,
                    .fogVolumeTile = 12,
                    .fogVolumeMaxCells = 160,
                    .fogVolumeSlices = 64};
        case ScreenEffectsQuality::High:
            return {.ambientOcclusionDivisor = 1,
                    .ambientOcclusionSlices = 3,
                    .ambientOcclusionSteps = 5,
                    .fogVolumeTile = 8,
                    .fogVolumeMaxCells = 240,
                    .fogVolumeSlices = 80};
    }
    return {};
}

} // namespace cordite
