/// @file
/// Entry point of the Cordite editor.
///
/// Besides the interactive editor, the executable has a batch mode for automation and
/// tests. It can open a scene, run a script of editor commands, render a number of frames
/// without a window, save what the last one looked like, and report the outcome through
/// its exit status:
///
///     cordite-editor --headless --scene depot.scene.json --exec edits.json --frames 3 --screenshot out.png
///
/// The outcome of a script is written to standard output as JSON (or to the file given
/// with --exec-output); everything else the editor has to say goes to standard error.

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/editor/EditorApp.h"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

/// Process exit codes. Automation distinguishes "it could not run" from "it ran, but the
/// GPU validation layers complained" and from "it ran, but a script step failed".
constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitValidationIssues = 2;
constexpr int kExitScriptFailed = 3;

/// Where an interactive editor remembers its window layout, relative to the working
/// directory. The .cordite directory holds per-user state and is not version-controlled.
constexpr const char* kLayoutFile = ".cordite/editor-layout.ini";

struct Options {
    bool headless = false;
    bool validation = false;
    bool noVsync = false;
    uint32_t width = 1600;
    uint32_t height = 900;
    /// Zero runs until the window is closed.
    uint64_t frames = 0;
    double fixedTimeStep = 0.0;
    std::string scene;
    std::string script;
    std::string scriptOutput;
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

/// Runs the script file and writes its outcome. Returns false when the script could not
/// be run or one of its steps failed.
bool runScriptFile(cordite::EditorApp& editor, const Options& options) {
    using namespace cordite;

    const Result<Json> script = readJsonFile(pathFromUtf8(options.script));
    if (!script) {
        CORDITE_LOG_ERROR("Editor", "cannot read the script: {}", script.error().describe());
        return false;
    }

    const Json outcome = editor.runScript(*script);
    if (options.scriptOutput.empty()) {
        std::cout << dumpJson(outcome) << '\n' << std::flush;
    } else if (const Status written = writeJsonFile(pathFromUtf8(options.scriptOutput), outcome); !written) {
        CORDITE_LOG_ERROR("Editor", "cannot write the script outcome: {}", written.error().describe());
        return false;
    }

    const bool succeeded = outcome.value("ok", false);
    if (!succeeded) {
        CORDITE_LOG_ERROR("Editor", "the script {} did not run to the end; see its outcome", options.script);
    }
    return succeeded;
}

int run(const Options& options) {
    using namespace cordite;

    LogConfig logConfig;
    logConfig.level = parseLogLevel(options.logLevel);
    if (const Status status = Log::initialize(logConfig); !status) {
        CORDITE_LOG_WARN("Editor", "{}", status.error().message);
    }
    CORDITE_LOG_INFO("Editor", "{} editor {}", kEngineName, kEngineVersion);

    EditorAppDesc desc;
    desc.engine.headless = options.headless;
    desc.engine.validation = options.validation;
    desc.engine.vsync = !options.noVsync;
    desc.engine.preferredAdapter = options.adapter;
    desc.engine.fixedTimeStep = options.fixedTimeStep;
    desc.engine.renderSize = UVec2(options.width, options.height);
    desc.engine.window.width = options.width;
    desc.engine.window.height = options.height;
    if (!options.headless) {
        // A batch run starts from the default layout every time, so that its output
        // does not depend on what a person did to the windows earlier.
        desc.engine.uiSettingsFile = pathFromUtf8(kLayoutFile);
    }
    desc.scene = pathFromUtf8(options.scene);

    Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(desc);
    if (!editor) {
        CORDITE_LOG_CRITICAL("Editor", "cannot start: {}", editor.error().describe());
        return kExitFailure;
    }
    Engine& engine = (*editor)->engine();

    int exitCode = kExitSuccess;
    if (!options.script.empty() && !runScriptFile(**editor, options)) {
        exitCode = kExitScriptFailed;
    }

    // Interactive: until the window is closed. Batch: exactly as many frames as asked for,
    // on top of any the script ran.
    const uint64_t firstFrame = engine.frameIndex();
    const bool untilClosed = options.frames == 0 && !options.headless;
    while (untilClosed || engine.frameIndex() - firstFrame < options.frames) {
        const Result<bool> keepRunning = (*editor)->runFrame();
        if (!keepRunning) {
            CORDITE_LOG_CRITICAL("Editor", "frame {} failed: {}", engine.frameIndex(), keepRunning.error().describe());
            return kExitFailure;
        }
        if (!*keepRunning) {
            break;
        }
    }

    if (!options.screenshot.empty()) {
        const Result<Image> frame = engine.captureFrame();
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

    const size_t issues = engine.graphics().validationIssueCount();
    if (issues > 0) {
        CORDITE_LOG_ERROR("Editor", "the GPU validation layers reported {} issue(s)", issues);
        return kExitValidationIssues;
    }
    return exitCode;
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

    app.add_option("--scene", options.scene, "Scene file to open at start");
    app.add_option("--exec", options.script,
                   "Run the editor commands in this JSON script before rendering; exits with status 3 if a step "
                   "fails");
    app.add_option("--exec-output", options.scriptOutput,
                   "Write the outcome of --exec to this file instead of standard output")
        ->needs("--exec");
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

    if (options.headless) {
        // Nobody is there to dismiss a dialog; a fatal error must end the process.
        cordite::suppressErrorDialogs();
    }
    if (options.headless && options.frames == 0 && options.script.empty()) {
        CORDITE_LOG_ERROR("Editor", "--headless needs --frames or --exec, because there is no window to close");
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
