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
/// desc.project = "samples/showcase";          // its assets, and its start scene
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
/// Besides the scene-editing and asset commands (see EditorCommands.h) the application
/// adds the ones that need a running editor: `editor.screenshot`, `editor.runFrames`,
/// `editor.statistics`, `editor.showPanel`, `editor.exit`, `viewport.lookAt`,
/// `viewport.frame`, `viewport.camera` and `viewport.debugView` - and those that play the
/// scene (PlayMode.h): `play.start`, `play.stop`, `play.pause`, `play.resume`, `play.step`
/// and `play.status`.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Image.h"
#include "cordite/core/Base.h"
#include "cordite/core/DirectoryWatcher.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/AssetBrowserPanel.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/editor/ConsolePanel.h"
#include "cordite/editor/EditorCamera.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/EditorPreferences.h"
#include "cordite/editor/HierarchyPanel.h"
#include "cordite/editor/InspectorPanel.h"
#include "cordite/editor/PlayMode.h"
#include "cordite/editor/StatisticsPanel.h"
#include "cordite/editor/ViewportPanel.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/Project.h"

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct EditorAppDesc {
    /// How to start the engine. The application name and the window title default to
    /// the editor's when left at the engine's defaults.
    EngineDesc engine;

    /// The project to work on: a directory with a project.cordite, or that file (see
    /// Project.h).
    /// Its asset directory becomes the engine's. Empty works without a project, with
    /// the built-in assets only.
    std::filesystem::path project;

    /// A scene file to open at start. Empty opens the project's start scene, or, without
    /// one, starts with a new, empty scene.
    std::filesystem::path scene;

    /// Keep the arrangement of the windows from one session to the next, in the
    /// `.cordite` directory of the project - or of the working directory, when there is
    /// no project. Left off, every session starts from the default layout, which is
    /// what automation wants. Ignored when `engine.uiSettingsFile` already names a file.
    bool rememberLayout = false;

    /// The file in which the editor keeps what the person at it chose about it - the
    /// theme - from one session to the next, in every project (editorPreferencesFile()).
    /// Empty keeps nothing, and every session starts with the defaults, which is what
    /// automation wants. A file that cannot be read is said in the log, and not used.
    std::filesystem::path preferencesFile;

    /// Notice when the files of the assets are added, removed or written by other programs
    /// - a script saved in a text editor, a texture painted anew - and read the assets
    /// again, as asset.refresh does: the scripts of a scene that is played go on with
    /// their new text. Left off, the assets are read again only when that is asked for,
    /// which is what automation wants: a batch run sees the files as its commands leave
    /// them. Needs a project.
    bool watchAssets = false;
    /// How long the watcher of the assets waits between two looks at their files.
    std::chrono::milliseconds assetWatchInterval{500};
};

class EditorApp {
public:
    /// Loads the project, starts the engine and opens the scene. Fails when any of
    /// that cannot be done.
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

    [[nodiscard]] const AssetBrowserPanel& assetBrowser() const { return m_assets; }

    /// Playing the scene where it is edited: start it, hold it, stop it. Start and stop
    /// between frames, never during one - which is where commands run.
    [[nodiscard]] PlayMode& play() { return *m_play; }

    /// The project being worked on, if the editor was started with one.
    [[nodiscard]] const std::optional<Project>& project() const { return m_project; }

private:
    EditorApp() = default;

    [[nodiscard]] Status registerApplicationCommands();
    [[nodiscard]] Status registerPlayCommands();
    [[nodiscard]] Status registerProjectCommands();
    /// Asks for files dropped onto the window from outside to be brought into the assets.
    void importDroppedFiles();
    /// Asks for the assets to be read again when files of theirs were changed by another
    /// program, and takes the files for unchanged after the editor wrote or read them.
    void followAssetFiles();
    /// A picture of an asset, drawn between frames: what asset.preview saves.
    [[nodiscard]] Result<Image> previewImage(const Uuid& asset, uint32_t size);
    /// Draws the interface in a theme from the next frame on, and remembers it in the
    /// preferences, if there is a file for them.
    void chooseTheme(UiTheme theme);
    /// What editor.theme returns: the theme in use, and the names of all.
    [[nodiscard]] Json describeTheme() const;
    /// What play.status returns: whether the scene is being played, and how it goes.
    [[nodiscard]] Json describePlay() const;
    void drawInterface();
    void drawMainMenu();
    void drawEntityMenu();
    void drawAssetMenu();
    /// A menu of the assets of one type, called `label`; `nothing` is said in it when
    /// there are none. Returns the one that was chosen in this frame, or nil.
    [[nodiscard]] Uuid drawAssetChoice(const char* label, AssetType type, const char* nothing) const;
    void drawPlayMenu();
    void drawViewMenu();
    void drawFileDialog();
    void handleShortcuts();
    /// W, E, R and X: what the gizmo does, and along whose axes.
    void handleGizmoShortcuts();
    /// Copying, cutting, pasting, duplicating and deleting entities.
    void handleEntityShortcuts();
    void updateWindowTitle();

    /// What the path dialog is being asked for.
    enum class FileDialog : uint8_t {
        None,
        Open,
        SaveAs,
        /// Where a game of the project goes.
        ExportGame,
    };

    std::optional<Project> m_project;
    /// What the person at the editor chose about it, and where it is kept (empty: nowhere).
    EditorPreferences m_preferences;
    std::filesystem::path m_preferencesFile;
    // The engine comes first: everything else refers to it and goes before it.
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<EditorContext> m_context;
    std::unique_ptr<PlayMode> m_play;
    /// Through whose eyes the viewport showed the scene before it was played.
    ViewportCamera m_viewBeforePlay = ViewportCamera::Editor;
    CommandRegistry m_commands;

    std::unique_ptr<ViewportPanel> m_viewport;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ConsolePanel m_console;
    AssetBrowserPanel m_assets;
    StatisticsPanel m_statistics;

    /// Lines entered in the console during a frame; they run before the next one, when
    /// commands are free to render frames themselves.
    std::vector<std::string> m_submittedLines;
    FileDialog m_fileDialog = FileDialog::None;
    bool m_openFileDialog = false;
    std::string m_fileDialogPath;
    bool m_showInterfaceDemo = false;
    bool m_showStatistics = false;
    /// The window that is brought to the front with the next frame: the one
    /// editor.focusPanel asked for, or the console when it has something to say. Empty
    /// for none.
    std::string m_panelToFocus;
    /// What the window title was last set from, to change it only when needed.
    std::string m_shownTitle;
    /// Watches the files of the assets (EditorAppDesc::watchAssets); null when nothing does.
    std::unique_ptr<DirectoryWatcher> m_assetWatcher;
    /// The revision of the assets and the count of saves that the watcher's files are of.
    uint64_t m_watchedRevision = 0;
    uint64_t m_watchedSaves = 0;
};

} // namespace cordite
