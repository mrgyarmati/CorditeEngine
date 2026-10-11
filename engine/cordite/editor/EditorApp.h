#pragma once

/// @file
/// The editor application: an Engine, the scene document being edited, the commands that
/// edit it, and the panels that show it.
///
/// The editor executable is a thin `main` around this class, and so are the tests that
/// drive the editor without a window.
///
/// @code
/// EditorAppDesc desc;
/// desc.engine.headless = true;
/// desc.scene = "samples/showcase/showcase.scene.json";
/// CORDITE_TRY_ASSIGN(auto editor, EditorApp::create(desc));
///
/// // Interactive: run until the window is closed.
/// while (true) {
///     CORDITE_TRY_ASSIGN(const bool keepRunning, editor->runFrame());
///     if (!keepRunning) {
///         break;
///     }
/// }
///
/// // Automated: run a script of commands and look at the outcome.
/// const Json outcome = editor->runScript(script);
/// @endcode
///
/// A script is a JSON array of steps, each naming a command and its parameters:
///
/// @code{.json}
/// [
///     {"command": "entity.create", "params": {"name": "Eye", "components": {"Camera": {}}}},
///     {"command": "viewport.lookAt", "params": {"position": [4, 3, 6], "target": [0, 0, 0]}},
///     {"command": "editor.screenshot", "params": {"path": "out/eye.png"}}
/// ]
/// @endcode
///
/// Besides the scene-editing commands (see EditorCommands.h) the application adds the
/// ones that need a running editor: `editor.screenshot`, `editor.runFrames`,
/// `editor.exit`, `viewport.lookAt`, `viewport.frame` and `viewport.camera`.

#include "cordite/assets/Image.h"
#include "cordite/core/Base.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/editor/ConsolePanel.h"
#include "cordite/editor/EditorCamera.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/HierarchyPanel.h"
#include "cordite/editor/InspectorPanel.h"
#include "cordite/editor/ViewportPanel.h"
#include "cordite/engine/Engine.h"

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct EditorAppDesc {
    /// How to start the engine. The application name and the window title default to
    /// the editor's when left at the engine's defaults.
    EngineDesc engine;

    /// A scene file to open at start. Empty starts with a new, empty scene.
    std::filesystem::path scene;
};

class EditorApp {
public:
    /// Starts the engine and opens the scene. Fails when either cannot be done.
    [[nodiscard]] static Result<std::unique_ptr<EditorApp>> create(const EditorAppDesc& desc);
    ~EditorApp();
    CORDITE_NON_MOVABLE(EditorApp);

    /// Runs one frame: commands entered in the console, the panels, rendering. Returns
    /// false once the editor should close.
    [[nodiscard]] Result<bool> runFrame();

    /// Runs up to `count` frames, stopping early if the editor should close.
    [[nodiscard]] Status runFrames(uint32_t count);

    /// Runs a script (see the file comment) and returns its outcome:
    /// `{"ok": true, "results": [{"command": ..., "ok": true, "result": ...}, ...]}`. A
    /// failed step has `"ok": false` and an `"error"` with `code` and `message`; the
    /// steps after it are not run. Never call this during a frame.
    [[nodiscard]] Json runScript(const Json& script);

    /// Runs one line as typed into the console: a command name, optionally followed by
    /// its parameters as a JSON object. The outcome is logged.
    void runCommandLine(std::string_view line);

    /// Renders the scene as the viewport shows it, without any interface, at the given
    /// size and returns the image. Never call this during a frame.
    [[nodiscard]] Result<Image> captureViewport(UVec2 size);

    [[nodiscard]] Engine& engine() { return *m_engine; }

    [[nodiscard]] EditorContext& context() { return *m_context; }

    [[nodiscard]] CommandRegistry& commands() { return m_commands; }

    [[nodiscard]] EditorCamera& camera() { return m_viewport->camera(); }

    [[nodiscard]] ViewportPanel& viewport() { return *m_viewport; }

private:
    EditorApp() = default;

    [[nodiscard]] Status registerApplicationCommands();
    void drawInterface();
    void drawMainMenu();
    void drawFileDialog();
    void handleShortcuts();
    void updateWindowTitle();

    /// What the path dialog is being asked for.
    enum class FileDialog : uint8_t {
        None,
        Open,
        SaveAs,
    };

    // The engine comes first: everything else refers to it and goes before it.
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<EditorContext> m_context;
    CommandRegistry m_commands;

    std::unique_ptr<ViewportPanel> m_viewport;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ConsolePanel m_console;

    /// Lines entered in the console during a frame; they run before the next one, when
    /// commands are free to render frames themselves.
    std::vector<std::string> m_submittedLines;
    FileDialog m_fileDialog = FileDialog::None;
    bool m_openFileDialog = false;
    std::string m_fileDialogPath;
    bool m_showInterfaceDemo = false;
    /// What the window title was last set from, to change it only when needed.
    std::string m_shownTitle;
};

} // namespace cordite
