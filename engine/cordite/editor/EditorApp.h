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
/// `editor.statistics`, `editor.showPanel`, `editor.focusPanel`, `editor.theme`,
/// `editor.exit` and `probes.capture`; those of the project - `project.new`, `project.open`
/// and `project.export`; those of assets that need the engine or the open scene -
/// `asset.listen`, `asset.preview`, `asset.move` and `asset.moveDirectory`; those of
/// the viewports - `viewport.open`, `viewport.close`, `viewport.list`, `viewport.lookAt`,
/// `viewport.frame`, `viewport.show`, `viewport.camera`, `viewport.debugView`,
/// `viewport.quality` and `viewport.gizmo` - and those that play the scene (PlayMode.h):
/// `play.start`, `play.stop`, `play.pause`, `play.resume`, `play.step` and `play.status`.
///
/// An editor has up to kMaxViewports viewports. The first is always there; the others are
/// opened and closed. What one of them is told without being named is told to the one
/// that was used last (activeViewport()); what their pictures cost and what the gizmo does
/// is theirs together (viewportSettings()).

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
#include "cordite/editor/FileDialogs.h"
#include "cordite/editor/HierarchyPanel.h"
#include "cordite/editor/InspectorPanel.h"
#include "cordite/editor/PlayMode.h"
#include "cordite/editor/StatisticsPanel.h"
#include "cordite/editor/ViewportPanel.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/Project.h"

#include <imgui.h>

#include <array>
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
    /// the editor's when left at the engine's defaults; the shader compiler to Slang's;
    /// the cache directory, where compiled shaders are kept between sessions, to the
    /// project's (Project::cacheDirectory), or `.cordite` in the working directory without
    /// a project.
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

    /// The dialogs the File menu asks a person for paths with, which must outlive the
    /// editor - for a test, which answers for the person. Null: the dialogs of the system
    /// for an editor with a window, and none without one, whose menu then asks for no
    /// path - the commands that take paths are the way of a batch run.
    FileDialogs* fileDialogs = nullptr;
};

class EditorApp {
public:
    /// The paths the File menu asks a person for, each in a dialog.
    enum class PathDialog : uint8_t {
        None,
        /// A scene to open.
        OpenScene,
        /// Where the scene is saved, and under what name.
        SaveSceneAs,
        /// Where a game of the project goes.
        ExportGame,
        /// Where a new project is made, and what it is called: the name of its directory.
        NewProject,
        /// A project to open instead.
        OpenProject,
    };

    /// What leaves the scene that is open, and would lose the changes it has not saved.
    enum class Leaving : uint8_t {
        None,
        /// File > New scene: an empty scene in its place.
        NewScene,
        /// File > Open scene, Ctrl+O: a scene a dialog names.
        OpenScene,
        /// File > New project: a project made where a dialog says, which the editor
        /// closes to begin again on.
        NewProject,
        /// File > Open project: a project a dialog names, which the editor closes to begin
        /// again on.
        OpenProject,
        /// File > Open recent: the project chosen there (openRecentProject()), which the
        /// editor closes to begin again on.
        OpenRecentProject,
        /// File > Exit.
        Exit,
    };

    /// What a person answers when asked whether to save the changes of the scene first.
    enum class UnsavedChangesAnswer : uint8_t {
        Save,
        DontSave,
        Cancel,
    };

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

    /// Renders the scene as a viewport shows it - the first, unless another is named by its
    /// number - without any interface, at the given size and returns the image. Fails for
    /// a viewport that is not open. Never call this during a frame.
    [[nodiscard]] Result<Image> captureViewport(UVec2 size, uint32_t viewport = 1);

    [[nodiscard]] Engine& engine() { return *m_engine; }

    [[nodiscard]] EditorContext& context() { return *m_context; }

    [[nodiscard]] CommandRegistry& commands() { return m_commands; }

    /// The camera of the first viewport.
    [[nodiscard]] EditorCamera& camera() { return viewport().camera(); }

    /// The first viewport, which is always there.
    [[nodiscard]] ViewportPanel& viewport() { return *m_viewports.front(); }

    /// A viewport by its number, from 1 to kMaxViewports; null when that one is not open.
    [[nodiscard]] ViewportPanel* findViewport(uint32_t number);

    /// The number of the viewport that the menus, F and the commands that name none act on:
    /// the one whose window had the focus last, or that was opened last.
    [[nodiscard]] uint32_t activeViewport() const { return m_activeViewport; }

    /// What every viewport shares: what its picture may cost, and what the gizmo does.
    [[nodiscard]] const ViewportSettings& viewportSettings() const { return m_viewportSettings; }

    [[nodiscard]] const AssetBrowserPanel& assetBrowser() const { return m_assets; }

    /// Asks for a path in a dialog, as the File menu does, between this frame and the
    /// next - where a dialog that holds the program until it is answered does not stop a
    /// frame half drawn - and does with the answer what the menu does: a scene is opened
    /// or saved, a project made, opened or exported by its command. A cancelled dialog
    /// changes nothing; a dialog that cannot be shown, and an editor without dialogs, is
    /// said in the console.
    void askForPath(PathDialog dialog) { m_pathDialog = dialog; }

    /// Whether the editor has dialogs to ask for paths with (EditorAppDesc::fileDialogs).
    [[nodiscard]] bool hasFileDialogs() const { return m_fileDialogs != nullptr; }

    /// Does what `leaving` says, as the File menu does - asking for a path where it needs
    /// one - and first, when the scene has changes that are not saved, asks whether to save
    /// them: a question of the editor's own, drawn in its window from the next frame on,
    /// which unsavedChangesQuestion() says is open and answerUnsavedChanges() answers, as
    /// its buttons do. Without such changes it goes ahead at once. A question that was not
    /// answered is replaced by the new one.
    void leave(Leaving leaving);

    /// Opens a project of File > Open recent, as the menu does: after asking whether to save
    /// the changes of the scene, if it has any, by `project.open` - which refuses what is no
    /// project, and says so in the console. The editor then closes (projectToOpen()).
    void openRecentProject(const std::filesystem::path& directory);

    /// What a person chose about the editor, and the projects they opened last.
    [[nodiscard]] const EditorPreferences& preferences() const { return m_preferences; }

    /// What the open question about unsaved changes is about; Leaving::None while none is.
    [[nodiscard]] Leaving unsavedChangesQuestion() const { return m_leaving; }

    /// Answers the open question about unsaved changes. Save writes the scene to its file -
    /// a scene without one where a dialog says - and goes on once it is written; a save
    /// that fails, or a dialog that is cancelled, leaves the scene where it was. DontSave
    /// goes on and lets the changes go; Cancel changes nothing. Nothing without a question.
    void answerUnsavedChanges(UnsavedChangesAnswer answer);

    /// Playing the scene where it is edited: start it, hold it, stop it. Start and stop
    /// between frames, never during one - which is where commands run.
    [[nodiscard]] PlayMode& play() { return *m_play; }

    /// The project being worked on, if the editor was started with one.
    [[nodiscard]] const std::optional<Project>& project() const { return m_project; }

    /// The project the editor was asked to open instead of its own - by File > New project,
    /// File > Open project or their commands - which closes it: runFrame() returns false
    /// from then on, and whoever runs it starts another editor on that project. Empty while
    /// nothing was asked.
    [[nodiscard]] const std::optional<std::filesystem::path>& projectToOpen() const { return m_projectToOpen; }

private:
    EditorApp() = default;

    [[nodiscard]] Status registerApplicationCommands();
    [[nodiscard]] Status registerViewportCommands();
    [[nodiscard]] Status registerPlayCommands();
    [[nodiscard]] Status registerProjectCommands();
    /// Opens the viewport of the lowest number that is not open, looking from where the
    /// active one does, through `camera`, and makes it the active one. Returns its number;
    /// fails when kMaxViewports are open.
    [[nodiscard]] Result<uint32_t> openViewport(ViewportCamera camera);
    /// Closes a viewport other than the first, after it has let go of what it held. The first
    /// is active after the active one is closed.
    [[nodiscard]] Status closeViewport(uint32_t number);
    /// The viewport a command is about: the one its "viewport" parameter names by its number,
    /// or the active one. Fails, naming the open ones, for one that is not open.
    [[nodiscard]] Result<ViewportPanel*> viewportOf(const Json& parameters);
    /// The viewport used last, which is always open.
    [[nodiscard]] ViewportPanel& activePanel();
    /// The numbers of the open viewports, for a message: "1, 2 and 4".
    [[nodiscard]] std::string openViewportNumbers() const;
    /// What viewport.list returns: the active viewport, and what each that is open shows.
    [[nodiscard]] Json describeViewports() const;
    /// Gives the viewports that were opened since the last frame a place in the layout of
    /// the dock space before they are drawn: beside the others, unless a place is kept for
    /// them.
    void placeOpenedViewports(ImGuiID dockSpaceId);
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
    /// Remembers in the preferences how the Assets panel shows the contents of a folder,
    /// when the panel or `assets.browse` has switched it - and writes them, if there is a
    /// file for them.
    void rememberAssetView();
    /// Writes the preferences, when there is a file for them; says `whatIsLost` - what the
    /// next session will not know - when they cannot be written.
    void savePreferences(std::string_view whatIsLost);
    /// What editor.theme returns: the theme in use, and the names of all.
    [[nodiscard]] Json describeTheme() const;
    /// What play.status returns: whether the scene is being played, and how it goes.
    [[nodiscard]] Json describePlay() const;
    void drawInterface();
    void drawMainMenu();
    void drawFileMenu();
    /// File > Open recent: the projects opened last, those that are gone greyed out, and an
    /// item that takes those out of the list.
    void drawRecentProjectsMenu();
    void drawEntityMenu();
    void drawAssetMenu();
    /// A menu of the assets of one type, called `label`; `nothing` is said in it when
    /// there are none. Returns the one that was chosen in this frame, or nil.
    [[nodiscard]] Uuid drawAssetChoice(const char* label, AssetType type, const char* nothing) const;
    void drawPlayMenu();
    void drawViewMenu();
    void handleShortcuts();
    /// W, E, R and X: what the gizmo does, and along whose axes.
    void handleGizmoShortcuts();
    /// F: frames what is selected in the active viewport.
    void handleFrameShortcut();
    /// Copying, cutting, pasting, duplicating and deleting entities.
    void handleEntityShortcuts();
    void updateWindowTitle();

    /// Shows the dialog askForPath() asked for, if any, and does what its answer says.
    void showPathDialog();
    /// What the dialog of `dialog` asks for.
    [[nodiscard]] FileDialogRequest pathRequest(PathDialog dialog) const;
    /// Does what `leaving` says, the changes of the scene saved or given up - the second
    /// when `discardChanges`, which the project commands that a dialog's answer runs are
    /// told: they refuse to lose changes otherwise.
    void goAhead(Leaving leaving, bool discardChanges);
    /// Draws the question about unsaved changes while one is open, and answers it as its
    /// buttons say - and its keys: Enter saves, Escape cancels.
    void drawUnsavedChangesQuestion();

    std::optional<Project> m_project;
    /// What the person at the editor chose about it, and where it is kept (empty: nowhere).
    EditorPreferences m_preferences;
    std::filesystem::path m_preferencesFile;
    // The engine comes first: everything else refers to it and goes before it.
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<EditorContext> m_context;
    std::unique_ptr<PlayMode> m_play;
    /// Through whose eyes the first viewport showed the scene before it was played.
    ViewportCamera m_viewBeforePlay = ViewportCamera::Editor;
    CommandRegistry m_commands;

    // Before the viewports, which refer to it.
    ViewportSettings m_viewportSettings;
    /// The viewports that are open, by their numbers less one; the first is always there.
    std::array<std::unique_ptr<ViewportPanel>, kMaxViewports> m_viewports;
    uint32_t m_activeViewport = 1;
    /// The viewports opened since the last frame, in the order they were opened: they are
    /// given a place before they are first drawn.
    std::vector<uint32_t> m_viewportsToPlace;
    HierarchyPanel m_hierarchy;
    InspectorPanel m_inspector;
    ConsolePanel m_console;
    AssetBrowserPanel m_assets;
    StatisticsPanel m_statistics;

    /// Lines entered in the console during a frame; they run before the next one, when
    /// commands are free to render frames themselves.
    std::vector<std::string> m_submittedLines;
    /// The dialogs of the system, when the editor has made them, and the dialogs it asks
    /// for paths with - those, or a test's - if any.
    std::unique_ptr<FileDialogs> m_systemDialogs;
    FileDialogs* m_fileDialogs = nullptr;
    /// The dialog to show before the next frame.
    PathDialog m_pathDialog = PathDialog::None;
    /// Whether the project command that the answer of that dialog runs gives up the
    /// changes of the scene (`discardChanges`): a person said not to save them.
    bool m_discardChanges = false;
    /// What the open question about unsaved changes is about; None while none is open.
    Leaving m_leaving = Leaving::None;
    /// What is done once the scene has been saved where the Save as dialog says, when the
    /// question was answered with Save for a scene without a file; None for nothing.
    Leaving m_leavingAfterSave = Leaving::None;
    /// The project File > Open recent chose, until it is opened or the question is answered.
    std::filesystem::path m_recentProjectToOpen;
    std::optional<std::filesystem::path> m_projectToOpen;
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
