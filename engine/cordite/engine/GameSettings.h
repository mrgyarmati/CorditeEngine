#pragma once

/// @file
/// What the person who plays a game chooses about it: whether it fills the screen, the size
/// of its window, whether a frame waits for the display, how good its pictures are and how
/// loud each kind of its sounds. They are the player's, not the project's: kept in a file in
/// the player's own configuration directory (gameSettingsFile()), which a game reads when it
/// starts and writes with the defaults when there is none yet - so that there is a file to
/// change, which the next start takes (GameDesc::settingsFile). What the command line of a
/// game says wins over the file for that start, and is not written into it
/// (GameSettingsOverrides).
///
/// A file that a game writes has every setting in it; one that a person writes may leave
/// some out, which keep their defaults. It is read as strictly as a scene: a member that is
/// not known, or a value of the wrong kind or out of its range, is an error that names the
/// file and the member.
///
/// @code{.json}
/// {
///     "format": "cordite.settings",
///     "version": 1,
///     "fullscreen": true,
///     "quality": "medium",
///     "musicVolume": 0.5
/// }
/// @endcode
///
/// @code
/// const std::filesystem::path file = gameSettingsFile(project.settings.name);
/// CORDITE_TRY_ASSIGN(GameSettings settings, loadGameSettings(file));
/// settings = withOverrides(settings, {.quality = RenderQuality::Low});
/// CORDITE_TRY(saveGameSettings(file, settings));
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/renderer/SceneRenderer.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace cordite {

class Engine;
struct EngineDesc;
class TypeInfo;

struct GameSettings {
    /// A borderless window over the whole of the primary monitor, or a window of its own.
    bool fullscreen = false;
    /// The size of the window when it does not fill the screen, in pixels, from 1 to 16384.
    uint32_t width = 1280;
    uint32_t height = 720;
    /// Whether a frame waits for the display to show the one before.
    bool vsync = true;
    /// What a picture may cost: shadows, ambient occlusion, how far anti-aliasing looks. The
    /// default is what an engine draws with when it is not told (SceneRenderSettings).
    RenderQuality quality = RenderQuality::Medium;
    /// How loud everything is, and each bus (AudioBus): 0 nothing, 1 as made, up to 1.
    float masterVolume = 1.0f;
    float effectsVolume = 1.0f;
    float musicVolume = 1.0f;
    float ambienceVolume = 1.0f;
    float voiceVolume = 1.0f;
    float interfaceVolume = 1.0f;

    bool operator==(const GameSettings&) const = default;
};

/// What wins over the settings of a player for one start of a game, and is not written into
/// them: what was said on the command line. What is left empty is as the settings say.
struct GameSettingsOverrides {
    std::optional<bool> fullscreen;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
    std::optional<bool> vsync;
    std::optional<RenderQuality> quality;

    bool operator==(const GameSettingsOverrides&) const = default;
};

/// `settings` with what `overrides` says instead.
[[nodiscard]] GameSettings withOverrides(GameSettings settings, const GameSettingsOverrides& overrides);

/// The description of GameSettings: the names of its members in a file, their ranges, what
/// each is.
[[nodiscard]] const TypeInfo& gameSettingsTypeInfo();

/// Where the person who plays the game called `gameName` keeps its settings: settings.json in
/// a directory named after the game (gameFileName()) in their configuration directory
/// (userConfigurationDirectory()) - %APPDATA% on Windows. Empty when the system does not say
/// where that is.
[[nodiscard]] std::filesystem::path gameSettingsFile(std::string_view gameName);

/// Reads settings. Fails with NotFound for a file that is not there; with ParseError for one
/// that is no JSON or no settings file, and VersionMismatch for one of a newer version; with
/// InvalidArgument for a member that is not known or a value of the wrong kind, and
/// OutOfRange for a value out of its range, naming the member - as a scene does; and as
/// readJsonFile() does for a file that cannot be read. Every message names the file.
[[nodiscard]] Result<GameSettings> loadGameSettings(const std::filesystem::path& file);

/// Writes settings - every member, so that a person finds them all - and the directories the
/// file is in. Fails with InvalidArgument for settings out of their ranges, which no file may
/// hold, and with IoError.
[[nodiscard]] Status saveGameSettings(const std::filesystem::path& file, const GameSettings& settings);

/// What of `settings` is decided before an engine is made: whether its window fills the
/// screen, its size - and that of the picture, for an engine without a window - and vsync.
void applyGameSettings(const GameSettings& settings, EngineDesc& desc);

/// What of `settings` an engine takes while it runs: the quality of its pictures
/// (Engine::renderSettings()) and the volumes of its sound. Settings out of their ranges -
/// which no file holds - leave a volume as it was, and say so in the log.
void applyGameSettings(const GameSettings& settings, Engine& engine);

} // namespace cordite
