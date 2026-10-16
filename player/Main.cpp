/// @file
/// Entry point of a game made with Cordite: cordite-player, or the name an export gives it.
///
/// It plays the game in its own directory - which is what an export writes beside it - or
/// in the one it is given, until its window is closed; or, for automation and tests,
/// without a window for a number of frames, saving what the last one looked like:
///
///     cordite-player
///     cordite-player --game path/to/game --headless --frames 120 --fixed-time-step 0.0166667 --screenshot out.png
///
/// With a window it plays with the settings of the person who plays it (GameSettings.h):
/// settings.json in a directory named after the game in their configuration directory -
/// written with the defaults the first time, for them to change - or the file --settings
/// names. What its options say of the window, vsync and the quality of the pictures wins
/// over that file for one start, and is not written into it. Without a window it reads no
/// settings unless --settings names a file: a run that nobody watches must not change what
/// a person chose.
///
/// Exit status: 0 the game ran, 1 it could not run, 2 the GPU validation layers reported
/// issues. What it has to say goes to standard error.

#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Game.h"
#include "cordite/engine/GameSettings.h"
#include "cordite/engine/Project.h"
#include "cordite/renderer/SceneRenderer.h"

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitValidationIssues = 2;

struct Options {
    std::string game;
    std::string settings;
    bool headless = false;
    bool validation = false;
    bool noVsync = false;
    bool fullscreen = false;
    bool windowed = false;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string quality;
    /// What of the above was given, to win over the settings of the person who plays.
    cordite::GameSettingsOverrides overrides;
    /// Zero plays until the window is closed.
    uint64_t frames = 0;
    double fixedTimeStep = 0.0;
    std::string screenshot;
    std::string adapter;
    std::string logLevel = "info";
};

/// Says why the game ends to the person who plays it, who has no console to read the log
/// in. Nobody is asked when it runs unattended (--headless, suppressErrorDialogs()).
void tellPlayer(const std::string& message) {
    // The program is named after its game.
    cordite::showErrorDialog(cordite::pathToUtf8(cordite::executablePath().stem()), message);
}

/// Where the settings of the person who plays are kept: the file --settings names, or the
/// game's own in their configuration directory. None without a window, unless --settings
/// names one.
std::filesystem::path settingsFileOf(const Options& options, const std::filesystem::path& gameDirectory) {
    if (!options.settings.empty()) {
        return cordite::pathFromUtf8(options.settings);
    }
    if (options.headless) {
        return {};
    }
    // The name of the game says where. What is wrong with a directory that is no game,
    // Game::create() says.
    const cordite::Result<cordite::Project> project = cordite::loadProject(gameDirectory);
    if (!project) {
        return {};
    }
    std::filesystem::path file = cordite::gameSettingsFile(project->settings.name);
    if (file.empty()) {
        CORDITE_LOG_WARN("Game", "this system names no directory for settings: the game plays with the defaults");
    }
    return file;
}

int run(const Options& options) {
    cordite::LogConfig logConfig;
    logConfig.level = cordite::logLevelFromName(options.logLevel).value_or(cordite::LogLevel::Info);
    if (const cordite::Status status = cordite::Log::initialize(logConfig); !status) {
        CORDITE_LOG_WARN("Game", "{}", status.error().message);
    }

    cordite::GameDesc desc;
    // A game is where its program is, unless it is told otherwise.
    desc.directory = options.game.empty() ? cordite::executableDirectory() : cordite::pathFromUtf8(options.game);
    desc.engine.headless = options.headless;
    desc.engine.validation = options.validation;
    desc.engine.fixedTimeStep = options.fixedTimeStep;
    desc.engine.preferredAdapter = options.adapter;
    // Without a window nobody listens: what would be heard is mixed into memory.
    desc.engine.audio.output = options.headless ? cordite::AudioOutput::Offline : cordite::AudioOutput::Device;
    desc.settingsFile = settingsFileOf(options, desc.directory);
    desc.overrides = options.overrides;

    cordite::Result<std::unique_ptr<cordite::Game>> game = cordite::Game::create(std::move(desc));
    if (!game) {
        CORDITE_LOG_CRITICAL("Game", "cannot start: {}", game.error().describe());
        tellPlayer(fmt::format("The game cannot start.\n\n{}", game.error().describe()));
        return kExitFailure;
    }
    cordite::Engine& engine = (*game)->engine();

    const uint64_t firstFrame = engine.frameIndex();
    while (options.frames == 0 || engine.frameIndex() - firstFrame < options.frames) {
        const cordite::Result<bool> keepRunning = (*game)->runFrame();
        if (!keepRunning) {
            CORDITE_LOG_CRITICAL("Game", "frame {} failed: {}", engine.frameIndex(), keepRunning.error().describe());
            tellPlayer(fmt::format("The game has stopped.\n\n{}", keepRunning.error().describe()));
            return kExitFailure;
        }
        if (!*keepRunning) {
            break;
        }
    }

    if (!options.screenshot.empty()) {
        const cordite::Result<cordite::Image> frame = engine.captureFrame();
        if (!frame) {
            CORDITE_LOG_ERROR("Game", "cannot capture the frame: {}", frame.error().describe());
            return kExitFailure;
        }
        if (const cordite::Status saved = frame->savePng(cordite::pathFromUtf8(options.screenshot)); !saved) {
            CORDITE_LOG_ERROR("Game", "cannot save the screenshot: {}", saved.error().describe());
            return kExitFailure;
        }
    }

    const size_t issues = engine.graphics().validationIssueCount();
    if (issues > 0) {
        CORDITE_LOG_ERROR("Game", "the GPU validation layers reported {} issue(s)", issues);
        return kExitValidationIssues;
    }
    return kExitSuccess;
}

/// Parses the command line and plays the game. Kept apart from main(), so that main() can
/// make sure that nothing escapes it.
int parseAndRun(int argc, char** argv) {
    Options options;

    CLI::App app{"A game made with Cordite"};
    // On Windows the C runtime hands main() arguments in the ANSI code page; this re-reads
    // them as UTF-8, so that a path with other letters survives.
    argv = app.ensure_utf8(argv);
    app.set_version_flag("--version", cordite::kEngineVersion);
    app.add_option("--game", options.game,
                   "The directory of the game: where its project.cordite is. By default the directory of this "
                   "program");
    app.add_flag("--headless", options.headless, "Play without a window, drawing offscreen");
    app.add_flag("--validation", options.validation,
                 "Enable the GPU validation layers; exits with status 2 if they report anything");
    app.add_option("--settings", options.settings,
                   "The file of the player's settings. By default the game's own in the configuration directory of "
                   "the person who plays, and none with --headless");
    app.add_flag("--no-vsync", options.noVsync, "Do not wait for the display refresh");
    CLI::Option* fullscreen = app.add_flag("--fullscreen", options.fullscreen, "Fill the screen");
    app.add_flag("--windowed", options.windowed, "Play in a window")->excludes(fullscreen);
    app.add_option("--width", options.width, "Window or offscreen width in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--height", options.height, "Window or offscreen height in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--quality", options.quality, "What a picture may cost")
        ->check(CLI::IsMember({"low", "medium", "high"}));
    app.add_option("--frames", options.frames, "Exit after this many frames (0 = play until the window is closed)");
    app.add_option("--fixed-time-step", options.fixedTimeStep,
                   "Advance time by this many seconds per frame instead of using the clock")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--screenshot", options.screenshot, "Save the last frame to this PNG file on exit");
    app.add_option("--adapter", options.adapter, "Prefer the GPU whose name contains this text");
    app.add_option("--log-level", options.logLevel, "Minimum severity to log")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical", "off"}));

    CLI11_PARSE(app, argc, argv);

    // What was said wins over the settings; what was not said leaves them as they are.
    if (options.fullscreen || options.windowed) {
        options.overrides.fullscreen = options.fullscreen;
    }
    if (app.count("--width") > 0) {
        options.overrides.width = options.width;
    }
    if (app.count("--height") > 0) {
        options.overrides.height = options.height;
    }
    if (options.noVsync) {
        options.overrides.vsync = false;
    }
    if (!options.quality.empty()) {
        options.overrides.quality = cordite::renderQualityFromName(options.quality);
    }

    if (options.headless) {
        // Nobody is there to dismiss a dialog; a fatal error must end the process.
        cordite::suppressErrorDialogs();
        if (options.frames == 0) {
            CORDITE_LOG_ERROR("Game", "--headless needs --frames, because there is no window to close");
            return kExitFailure;
        }
    }
    return run(options);
}

} // namespace

int main(int argc, char** argv) {
    // Before anything is written: a game has no console of its own.
    cordite::attachToParentConsole();
    // The engine reports failures as Results; an exception reaching this point comes from a
    // library and is a bug. Report it and exit with a status instead of terminating.
    try {
        return parseAndRun(argc, argv);
    } catch (const std::exception& exception) {
        CORDITE_LOG_CRITICAL("Game", "unhandled exception: {}", exception.what());
    } catch (...) {
        CORDITE_LOG_CRITICAL("Game", "unhandled exception of unknown type");
    }
    return kExitFailure;
}
