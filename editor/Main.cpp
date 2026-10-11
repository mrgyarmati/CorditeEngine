/// @file
/// Entry point of the Cordite editor.
///
/// Besides the interactive editor, the executable has a batch mode for automation and
/// tests: `cordite-editor --headless --frames 10 --screenshot out.png` renders ten frames
/// without a window, saves the last one and exits with a status that reflects success.

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Engine.h"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

/// Process exit codes. Automation distinguishes "it crashed" from "it ran but the GPU
/// validation layers complained".
constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitValidationIssues = 2;

struct Options {
    bool headless = false;
    bool validation = false;
    bool noVsync = false;
    uint32_t width = 1280;
    uint32_t height = 720;
    /// Zero runs until the window is closed.
    uint64_t frames = 0;
    double fixedTimeStep = 0.0;
    std::string screenshot;
    std::string adapter;
    std::string logLevel = "info";
};

cordite::LogLevel parseLogLevel(const std::string& name) {
    for (const cordite::LogLevel level :
         {cordite::LogLevel::Trace, cordite::LogLevel::Debug, cordite::LogLevel::Info, cordite::LogLevel::Warn,
          cordite::LogLevel::Error, cordite::LogLevel::Critical, cordite::LogLevel::Off}) {
        if (cordite::nameOf(level) == name) {
            return level;
        }
    }
    return cordite::LogLevel::Info;
}

int run(const Options& options) {
    using namespace cordite;

    LogConfig logConfig;
    logConfig.level = parseLogLevel(options.logLevel);
    if (const Status status = Log::initialize(logConfig); !status) {
        CORDITE_LOG_WARN("Editor", "{}", status.error().message);
    }
    CORDITE_LOG_INFO("Editor", "{} editor {}", kEngineName, kEngineVersion);

    EngineDesc desc;
    desc.applicationName = "Cordite Editor";
    desc.headless = options.headless;
    desc.validation = options.validation;
    desc.vsync = !options.noVsync;
    desc.preferredAdapter = options.adapter;
    desc.fixedTimeStep = options.fixedTimeStep;
    desc.renderSize = UVec2(options.width, options.height);
    desc.window.title = "Cordite Editor";
    desc.window.width = options.width;
    desc.window.height = options.height;

    Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
    if (!engine) {
        CORDITE_LOG_CRITICAL("Editor", "cannot start: {}", engine.error().describe());
        return kExitFailure;
    }

    while (options.frames == 0 || (*engine)->frameIndex() < options.frames) {
        const Result<bool> keepRunning = (*engine)->runFrame();
        if (!keepRunning) {
            CORDITE_LOG_CRITICAL("Editor", "frame {} failed: {}", (*engine)->frameIndex(),
                                 keepRunning.error().describe());
            return kExitFailure;
        }
        if (!*keepRunning) {
            break;
        }
    }

    if (!options.screenshot.empty()) {
        const Result<Image> frame = (*engine)->captureFrame();
        if (!frame) {
            CORDITE_LOG_ERROR("Editor", "cannot capture the frame: {}", frame.error().describe());
            return kExitFailure;
        }
        if (const Status saved = frame->savePng(pathFromUtf8(options.screenshot)); !saved) {
            CORDITE_LOG_ERROR("Editor", "cannot save the screenshot: {}", saved.error().describe());
            return kExitFailure;
        }
        CORDITE_LOG_INFO("Editor", "saved {}", options.screenshot);
    }

    const size_t issues = (*engine)->graphics().validationIssueCount();
    if (issues > 0) {
        CORDITE_LOG_ERROR("Editor", "the GPU validation layers reported {} issue(s)", issues);
        return kExitValidationIssues;
    }
    return kExitSuccess;
}

/// Parses the command line and runs the editor. Kept separate from main() so that main()
/// can guarantee nothing escapes it.
int parseAndRun(int argc, char** argv) {
    Options options;

    CLI::App app{"Cordite editor"};
    // On Windows the C runtime hands main() arguments in the ANSI code page; this
    // re-reads them as UTF-8 so that non-ASCII paths survive.
    argv = app.ensure_utf8(argv);
    app.set_version_flag("--version", cordite::kEngineVersion);

    app.add_flag("--headless", options.headless, "Run without a window and render offscreen");
    app.add_flag("--validation", options.validation,
                 "Enable GPU validation layers; exits with status 2 if they report anything");
    app.add_flag("--no-vsync", options.noVsync, "Do not wait for the display refresh");
    app.add_option("--width", options.width, "Window or offscreen width in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--height", options.height, "Window or offscreen height in pixels")->check(CLI::Range(1u, 16384u));
    app.add_option("--frames", options.frames, "Exit after rendering this many frames (0 = run until closed)");
    app.add_option("--fixed-time-step", options.fixedTimeStep,
                   "Advance time by this many seconds per frame instead of using the clock")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--screenshot", options.screenshot, "Save the last rendered frame to this PNG file on exit");
    app.add_option("--adapter", options.adapter, "Prefer the GPU whose name contains this text");
    app.add_option("--log-level", options.logLevel, "Minimum severity to log")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical", "off"}));

    CLI11_PARSE(app, argc, argv);

    if (options.headless && options.frames == 0) {
        CORDITE_LOG_ERROR("Editor", "--headless needs --frames, because there is no window to close");
        return kExitFailure;
    }
    return run(options);
}

} // namespace

int main(int argc, char** argv) {
    // The engine reports failures as Results; an exception reaching this point comes from
    // a library and is a bug. Report it and exit with a status instead of terminating.
    try {
        return parseAndRun(argc, argv);
    } catch (const std::exception& exception) {
        CORDITE_LOG_CRITICAL("Editor", "unhandled exception: {}", exception.what());
    } catch (...) {
        CORDITE_LOG_CRITICAL("Editor", "unhandled exception of unknown type");
    }
    return kExitFailure;
}
