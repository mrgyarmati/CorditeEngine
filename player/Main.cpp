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
/// Exit status: 0 the game ran, 1 it could not run, 2 the GPU validation layers reported
/// issues. What it has to say goes to standard error.

#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Game.h"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <exception>
#include <string>
#include <utility>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitValidationIssues = 2;

struct Options {
    std::string game;
    bool headless = false;
    bool validation = false;
    bool noVsync = false;
    bool fullscreen = false;
    uint32_t width = 1280;
    uint32_t height = 720;
    /// Zero plays until the window is closed.
    uint64_t frames = 0;
    double fixedTimeStep = 0.0;
    std::string screenshot;
    std::string adapter;
    std::string logLevel = "info";
};

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
    desc.engine.vsync = !options.noVsync;
    desc.engine.window.width = options.width;
    desc.engine.window.height = options.height;
    desc.engine.window.fullscreen = options.fullscreen;
    desc.engine.renderSize = {options.width, options.height};
    desc.engine.fixedTimeStep = options.fixedTimeStep;
    desc.engine.preferredAdapter = options.adapter;
    // Without a window nobody listens: what would be heard is mixed into memory.
    desc.engine.audio.output = options.headless ? cordite::AudioOutput::Offline : cordite::AudioOutput::Device;

    cordite::Result<std::unique_ptr<cordite::Game>> game = cordite::Game::create(std::move(desc));
    if (!game) {
        CORDITE_LOG_CRITICAL("Game", "cannot start: {}", game.error().describe());
        return kExitFailure;
    }
    cordite::Engine& engine = (*game)->engine();

    const uint64_t firstFrame = engine.frameIndex();
    while (options.frames == 0 || engine.frameIndex() - firstFrame < options.frames) {
        const cordite::Result<bool> keepRunning = (*game)->runFrame();
        if (!keepRunning) {
            CORDITE_LOG_CRITICAL("Game", "frame {} failed: {}", engine.frameIndex(), keepRunning.error().describe());
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
    app.add_flag("--no-vsync", options.noVsync, "Do not wait for the display refresh");
    app.add_flag("--fullscreen", options.fullscreen, "Fill the screen");
    app.add_option("--width", options.width, "Window or offscreen width in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--height", options.height, "Window or offscreen height in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--frames", options.frames, "Exit after this many frames (0 = play until the window is closed)");
    app.add_option("--fixed-time-step", options.fixedTimeStep,
                   "Advance time by this many seconds per frame instead of using the clock")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--screenshot", options.screenshot, "Save the last frame to this PNG file on exit");
    app.add_option("--adapter", options.adapter, "Prefer the GPU whose name contains this text");
    app.add_option("--log-level", options.logLevel, "Minimum severity to log")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical", "off"}));

    CLI11_PARSE(app, argc, argv);

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
