#include "cordite/engine/GameSettings.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Reflection.h"
#include "cordite/engine/Engine.h"
#include "cordite/renderer/SceneRenderer.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Settings unlike the defaults in every member.
GameSettings chosenSettings() {
    GameSettings settings;
    settings.fullscreen = true;
    settings.width = 1920;
    settings.height = 1080;
    settings.vsync = false;
    settings.quality = RenderQuality::Low;
    settings.masterVolume = 0.8f;
    settings.effectsVolume = 0.7f;
    settings.musicVolume = 0.1f;
    settings.ambienceVolume = 0.25f;
    settings.voiceVolume = 0.5f;
    settings.interfaceVolume = 0.0f;
    return settings;
}

/// Why the settings in `text` are refused.
Error refusalOf(const TempDirectory& directory, std::string_view text) {
    const std::filesystem::path file = directory.path() / "settings.json";
    REQUIRE(writeTextFile(file, text).has_value());
    const Result<GameSettings> settings = loadGameSettings(file);
    REQUIRE_FALSE(settings.has_value());
    return settings.error();
}

} // namespace

TEST_SUITE("GameSettings") {
    TEST_CASE("settings are written whole and read back as they were, to the byte") {
        const TempDirectory directory;
        // The directories it is in are made.
        const std::filesystem::path file = directory.path() / "Depot" / "settings.json";
        const GameSettings settings = chosenSettings();
        REQUIRE(saveGameSettings(file, settings).has_value());
        CHECK(unwrap(loadGameSettings(file)) == settings);

        // Every member is in the file, under the name a person reads.
        const std::string text = unwrap(readTextFile(file));
        for (const std::string_view member :
             {R"("format": "cordite.settings")", R"("version": 1)", R"("fullscreen": true)", R"("width": 1920)",
              R"("height": 1080)", R"("vsync": false)", R"("quality": "low")", R"("masterVolume": 0.8)",
              R"("effectsVolume": 0.7)", R"("musicVolume": 0.1)", R"("ambienceVolume": 0.25)", R"("voiceVolume": 0.5)",
              R"("interfaceVolume": 0.0)"}) {
            CHECK_MESSAGE(text.find(member) != std::string::npos, std::string(member));
        }
        // Read and written again, it is the same bytes.
        REQUIRE(saveGameSettings(file, unwrap(loadGameSettings(file))).has_value());
        CHECK(unwrap(readTextFile(file)) == text);
    }

    TEST_CASE("what a file leaves out keeps its default, and what is not there is said") {
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / "settings.json";
        REQUIRE(writeTextFile(file, R"({
            "format": "cordite.settings",
            "version": 1,
            // Written by a person, who wanted it quieter.
            "musicVolume": 0.25
        })")
                    .has_value());
        GameSettings expected;
        expected.musicVolume = 0.25f;
        CHECK(unwrap(loadGameSettings(file)) == expected);

        const Result<GameSettings> none = loadGameSettings(directory.path() / "none.json");
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().code == ErrorCode::NotFound);
    }

    TEST_CASE("a file that is no settings, or holds what settings cannot be, is refused, naming the file") {
        const TempDirectory directory;
        const std::string file = pathToUtf8(directory.path() / "settings.json");
        const auto refused = [&](std::string_view members, ErrorCode code, std::string_view named) {
            CAPTURE(std::string(members));
            const Error error =
                refusalOf(directory, fmt::format(R"({{"format": "cordite.settings", "version": 1, {}}})", members));
            CHECK(error.code == code);
            CHECK(error.message.find(file) != std::string::npos);
            CHECK(error.message.find(named) != std::string::npos);
        };
        refused(R"("colour": 1)", ErrorCode::InvalidArgument, "colour");
        refused(R"("fullscreen": "yes")", ErrorCode::InvalidArgument, "fullscreen");
        refused(R"("vsync": null)", ErrorCode::InvalidArgument, "vsync");
        refused(R"("quality": "ultra")", ErrorCode::InvalidArgument, "quality");
        refused(R"("width": 0)", ErrorCode::OutOfRange, "width");
        refused(R"("height": 16385)", ErrorCode::OutOfRange, "height");
        refused(R"("width": -5)", ErrorCode::OutOfRange, "width");
        refused(R"("musicVolume": 1.5)", ErrorCode::OutOfRange, "musicVolume");
        refused(R"("masterVolume": -0.1)", ErrorCode::OutOfRange, "masterVolume");

        CHECK(refusalOf(directory, R"({"format": "cordite.settings", "vers)").code == ErrorCode::ParseError);
        const Error other = refusalOf(directory, R"({"format": "cordite.project", "version": 1})");
        CHECK(other.code == ErrorCode::ParseError);
        CHECK(other.message.find(file) != std::string::npos);
        const Error newer = refusalOf(directory, R"({"format": "cordite.settings", "version": 2})");
        CHECK(newer.code == ErrorCode::VersionMismatch);
        CHECK(newer.message.find(file) != std::string::npos);
    }

    TEST_CASE("settings that no file may hold are not written") {
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / "settings.json";
        for (const auto& change : std::initializer_list<void (*)(GameSettings&)>{
                 [](GameSettings& settings) { settings.masterVolume = 1.5f; },
                 [](GameSettings& settings) { settings.voiceVolume = std::numeric_limits<float>::quiet_NaN(); },
                 [](GameSettings& settings) { settings.width = 0; },
                 [](GameSettings& settings) { settings.height = 20000; },
             }) {
            GameSettings settings;
            change(settings);
            const Status saved = saveGameSettings(file, settings);
            REQUIRE_FALSE(saved.has_value());
            CHECK(saved.error().code == ErrorCode::InvalidArgument);
            CHECK_FALSE(fileExists(file));
        }
    }

    TEST_CASE("what the command line says wins over the settings, and what it does not say leaves them") {
        const GameSettings settings = chosenSettings();
        CHECK(withOverrides(settings, {}) == settings);

        GameSettings expected = settings;
        expected.fullscreen = false;
        expected.width = 640;
        expected.height = 360;
        expected.vsync = true;
        expected.quality = RenderQuality::High;
        CHECK(withOverrides(
                  settings,
                  {.fullscreen = false, .width = 640, .height = 360, .vsync = true, .quality = RenderQuality::High}) ==
              expected);
        // One at a time.
        const GameSettings taller = withOverrides(settings, {.height = 1200});
        CHECK(taller.height == 1200);
        CHECK(taller.width == settings.width);
        CHECK(taller.fullscreen == settings.fullscreen);
    }

    TEST_CASE("settings decide the window of an engine before it is made") {
        EngineDesc desc;
        GameSettings settings = chosenSettings();
        applyGameSettings(settings, desc);
        CHECK(desc.window.fullscreen);
        CHECK(desc.window.width == 1920);
        CHECK(desc.window.height == 1080);
        // Without a window, the picture is that large.
        CHECK(desc.renderSize == UVec2(1920, 1080));
        CHECK_FALSE(desc.vsync);
        // The defaults are an engine's own.
        const EngineDesc made;
        EngineDesc defaults;
        applyGameSettings(GameSettings{}, defaults);
        CHECK(defaults.window.fullscreen == made.window.fullscreen);
        CHECK(defaults.window.width == made.window.width);
        CHECK(defaults.window.height == made.window.height);
        CHECK(defaults.renderSize == made.renderSize);
        CHECK(defaults.vsync == made.vsync);
        CHECK(renderQualityOf(SceneRenderSettings{}) == GameSettings{}.quality);
    }

    TEST_CASE("a game keeps its settings in a directory named after it, where its player keeps settings") {
        const std::filesystem::path base = userConfigurationDirectory();
        REQUIRE_FALSE(base.empty());
        CHECK(gameSettingsFile("Depot: the Game!") == base / "Depot the Game" / "settings.json");
        CHECK(gameSettingsFile("con") == base / "Game" / "settings.json");
        CHECK(gameSettingsFile("Café") == base / pathFromUtf8("Café") / "settings.json");
    }

    TEST_CASE("the settings are described for tools") {
        const TypeInfo& type = gameSettingsTypeInfo();
        CHECK(type.properties().size() == 11);
        for (const PropertyInfo& property : type.properties()) {
            CAPTURE(property.name());
            CHECK_FALSE(property.attributes().description.empty());
            if (property.type() == PropertyType::Float || property.type() == PropertyType::Int) {
                CHECK(property.attributes().minimum.has_value());
                CHECK(property.attributes().maximum.has_value());
            }
        }
    }
}
