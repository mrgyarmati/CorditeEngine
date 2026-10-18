#pragma once

/// @file
/// How much the effects that work from the screen, or over it, may cost.
///
/// Some of what makes a picture is worked out from the picture itself: from the depth
/// and the normals of what has been drawn, not from the scene. How much of the light
/// from all around reaches the foot of a wall (ambient occlusion, AmbientOcclusion.h)
/// is the first of them. Some is worked out for a grid laid over the picture: what the
/// lights throw towards the viewer in a fog that they light (VolumetricFog.h). What they
/// look like is set by the scene, with the rest of its look (PostProcessSettings.h, the
/// Fog component); how finely they are worked out is a quality of the view, like that of
/// its shadows.
///
/// @code
/// SceneRenderSettings settings;
/// settings.screenEffectsQuality = ScreenEffectsQuality::High;
///
/// const ScreenEffectsQualitySettings numbers = screenEffectsSettingsFor(ScreenEffectsQuality::Low);
/// // numbers.ambientOcclusionDivisor == 2: worked out for every other pixel each way
/// @endcode

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

/// How much the screen-space effects of a view may cost. From low to high the look
/// stays; what gives is how finely it is worked out.
enum class ScreenEffectsQuality : uint8_t {
    /// None of them: the light as the lights and the surroundings give it, and a fog lit
    /// by its own color alone.
    Off,
    /// For a quarter of the pixels, with few looks around each.
    Low,
    Medium,
    /// For every pixel.
    High,
};

inline constexpr std::array<ScreenEffectsQuality, 4> kScreenEffectsQualities = {
    ScreenEffectsQuality::Off, ScreenEffectsQuality::Low, ScreenEffectsQuality::Medium, ScreenEffectsQuality::High};

/// The name used in files and by tools: "off", "low", "medium", "high".
std::string_view nameOf(ScreenEffectsQuality quality);

/// The name shown to people: "Off", "Low", "Medium", "High".
std::string_view displayNameOf(ScreenEffectsQuality quality);

/// The quality with the given nameOf(), or nothing. Upper and lower case are not told
/// apart.
[[nodiscard]] std::optional<ScreenEffectsQuality> screenEffectsQualityFromName(std::string_view name);

/// What a quality comes to. With ScreenEffectsQuality::Off no effect is drawn, and the
/// numbers are those of the lowest quality.
struct ScreenEffectsQualitySettings {
    /// Ambient occlusion is worked out for one pixel in this many, each way: 1 for
    /// every pixel, 2 for a quarter of them. The others get theirs from those around.
    uint32_t ambientOcclusionDivisor = 2;
    /// Ambient occlusion: in how many directions across the screen a pixel looks for
    /// what stands in its light, and how many times it looks along each, either way.
    uint32_t ambientOcclusionSlices = 2;
    uint32_t ambientOcclusionSteps = 4;
    /// The light the lights throw into a fog (VolumetricFog.h) is worked out for boxes about
    /// this many pixels across and down, ...
    uint32_t fogVolumeTile = 16;
    /// ... at most this many across, or down, the picture ...
    uint32_t fogVolumeMaxCells = 120;
    /// ... and this many deep, from the camera to how far the fog is lit.
    uint32_t fogVolumeSlices = 48;

    bool operator==(const ScreenEffectsQualitySettings&) const = default;
};

[[nodiscard]] ScreenEffectsQualitySettings screenEffectsSettingsFor(ScreenEffectsQuality quality);

} // namespace cordite
