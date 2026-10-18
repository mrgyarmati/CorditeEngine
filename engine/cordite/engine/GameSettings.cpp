#include "cordite/engine/GameSettings.h"

#include "cordite/audio/AudioSystem.h"
#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Reflection.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/Project.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace cordite {
namespace {

constexpr DocumentFormat kSettingsFormat{.name = "cordite.settings", .version = 1, .kind = "file of game settings"};
constexpr std::array<std::string_view, 2> kHeaderMembers = {"format", "version"};
constexpr double kMaxWindowSide = 16384.0;

PropertyAttributes volume(const char* description) {
    return {.description = description, .minimum = 0.0, .maximum = 1.0};
}

TypeInfo describeGameSettings() {
    return TypeBuilder<GameSettings>("GameSettings")
        .property("fullscreen", &GameSettings::fullscreen,
                  {.description = "A borderless window over the whole of the primary monitor, or a window of its own"})
        .property("width", &GameSettings::width,
                  {.description = "The width of the window when it does not fill the screen",
                   .minimum = 1.0,
                   .maximum = kMaxWindowSide,
                   .unit = "px"})
        .property("height", &GameSettings::height,
                  {.description = "The height of the window when it does not fill the screen",
                   .minimum = 1.0,
                   .maximum = kMaxWindowSide,
                   .unit = "px"})
        .property("vsync", &GameSettings::vsync,
                  {.description = "Whether a frame waits for the display to show the one before"})
        .enumProperty("quality", &GameSettings::quality,
                      {{RenderQuality::Low, std::string(nameOf(RenderQuality::Low))},
                       {RenderQuality::Medium, std::string(nameOf(RenderQuality::Medium))},
                       {RenderQuality::High, std::string(nameOf(RenderQuality::High))}},
                      {.description = "What a picture may cost: shadows, ambient occlusion, anti-aliasing"})
        .property("masterVolume", &GameSettings::masterVolume, volume("How loud everything is: 0 nothing, 1 as made"))
        .property("effectsVolume", &GameSettings::effectsVolume, volume("How loud sound effects are"))
        .property("musicVolume", &GameSettings::musicVolume, volume("How loud the music is"))
        .property("ambienceVolume", &GameSettings::ambienceVolume, volume("How loud the sound all around is"))
        .property("voiceVolume", &GameSettings::voiceVolume, volume("How loud voices are"))
        .property("interfaceVolume", &GameSettings::interfaceVolume, volume("How loud the sounds of menus are"))
        .build();
}

} // namespace

GameSettings withOverrides(GameSettings settings, const GameSettingsOverrides& overrides) {
    settings.fullscreen = overrides.fullscreen.value_or(settings.fullscreen);
    settings.width = overrides.width.value_or(settings.width);
    settings.height = overrides.height.value_or(settings.height);
    settings.vsync = overrides.vsync.value_or(settings.vsync);
    settings.quality = overrides.quality.value_or(settings.quality);
    return settings;
}

const TypeInfo& gameSettingsTypeInfo() {
    static const TypeInfo kType = describeGameSettings();
    return kType;
}

std::filesystem::path gameSettingsFile(std::string_view gameName) {
    const std::filesystem::path directory = userConfigurationDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory / pathFromUtf8(gameFileName(gameName)) / "settings.json";
}

Result<GameSettings> loadGameSettings(const std::filesystem::path& file) {
    GameSettings settings;
    if (!fileExists(file)) {
        return makeError(ErrorCode::NotFound, "there are no settings in {}", pathToUtf8(file));
    }
    const auto fail = [&file](const Error& error) { return tl::unexpected(error.withContext(pathToUtf8(file))); };
    const Result<Json> document = readJsonFile(file);
    if (!document) {
        return tl::unexpected(document.error());
    }
    if (const Result<uint32_t> version = readDocumentHeader(*document, kSettingsFormat); !version) {
        return fail(version.error());
    }
    Json values = Json::object();
    for (const auto& [key, value] : document->items()) {
        if (std::ranges::find(kHeaderMembers, std::string_view(key)) == kHeaderMembers.end()) {
            values[key] = value;
        }
    }
    if (const Status parsed = gameSettingsTypeInfo().fromJson(&settings, values); !parsed) {
        return fail(parsed.error());
    }
    return settings;
}

Status saveGameSettings(const std::filesystem::path& file, const GameSettings& settings) {
    const Json values = gameSettingsTypeInfo().toJson(&settings);
    // What could not be read back is not written.
    GameSettings readBack;
    if (const Status valid = gameSettingsTypeInfo().fromJson(&readBack, values); !valid) {
        return makeError(ErrorCode::InvalidArgument, "the settings cannot be written to {}: {}", pathToUtf8(file),
                         valid.error().message);
    }
    Json document = makeDocument(kSettingsFormat);
    document.update(values);
    return writeJsonFile(file, document);
}

void applyGameSettings(const GameSettings& settings, EngineDesc& desc) {
    desc.window.fullscreen = settings.fullscreen;
    desc.window.width = settings.width;
    desc.window.height = settings.height;
    // Without a window the picture is drawn at that size.
    desc.renderSize = {settings.width, settings.height};
    desc.vsync = settings.vsync;
}

void applyGameSettings(const GameSettings& settings, Engine& engine) {
    engine.setRenderQuality(settings.quality);
    AudioSystem& audio = engine.audio();
    if (const Status master = audio.setMasterVolume(settings.masterVolume); !master) {
        CORDITE_LOG_WARN("Game", "the volume of everything is left as it was: {}", master.error().message);
    }
    const std::array<std::pair<AudioBus, float>, kAudioBuses.size()> volumes = {{
        {AudioBus::Effects, settings.effectsVolume},
        {AudioBus::Music, settings.musicVolume},
        {AudioBus::Ambience, settings.ambienceVolume},
        {AudioBus::Voice, settings.voiceVolume},
        {AudioBus::Interface, settings.interfaceVolume},
    }};
    for (const auto& [bus, value] : volumes) {
        if (const Status set = audio.setVolume(bus, value); !set) {
            CORDITE_LOG_WARN("Game", "the volume of {} is left as it was: {}", nameOf(bus), set.error().message);
        }
    }
}

} // namespace cordite
