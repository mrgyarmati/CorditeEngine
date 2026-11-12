#pragma once

/// @file
/// The editor panel that shows the assets of the project, and puts them to use.
///
/// Laid out as the browsers of Unity and Unreal Engine are (AssetBrowser): the tree of
/// the folders of the project at the left; beside it what the folder chosen there has in
/// it, as a list or as a grid of tiles, with the way to the folder above it - each step a
/// button back up - and a filter by type and by text; at the right a picture of the
/// chosen asset when it has one (AssetPreview); and under all of it the asset that is
/// chosen. A directory is gone into by a double click, a file with something inside it
/// opened by its arrow; a model or a prefab is placed in the scene by a double click or
/// from its menu, a scene is opened from its menu, and any asset is dragged onto a
/// property of the inspector that takes one of its kind; onto a folder to move its file
/// there; what can be placed - a model, a prefab or a mesh - onto an entity of the
/// hierarchy, below its rows, or onto a viewport, where it lands under the pointer; and a
/// material onto the mesh under the pointer in a viewport (AssetDrop.h). A sound has its
/// length at the right instead of a picture, and is heard with Play.
///
/// @code
/// AssetBrowserPanel assets;
/// std::vector<std::string> submitted;
/// assets.draw(engine, context, ShadowQuality::Medium, submitted);   // once per frame
/// assets.browser().setFilter({.type = AssetType::Prefab});
/// assets.browser().setView(AssetBrowserView::List);
/// @endcode
///
/// What the panel does it does by commands - `asset.instantiate`, `scene.open`,
/// `asset.refresh` - which it hands to the caller as lines to run, as the console does:
/// they change the scene and the assets, which is for between two frames, and what a
/// click did is then in the log like what was typed. What it shows - the folder, the
/// filter, the view, what is chosen - it changes in its browser, which is what
/// `assets.browse` asks.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/editor/AssetPreview.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/Engine.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite {

class AssetBrowserPanel {
public:
    /// What the window is known by: its place in a layout is remembered by it, whatever its
    /// tab shows (windowName() in UiWidgets.h).
    static constexpr const char* kWindowId = "###Assets";

    /// Draws the panel as its own window, with the frame of `engine` - in which the
    /// picture of the chosen asset is drawn, with shadows of the quality the other views
    /// have (AssetPreview::show()). Commands that the user asked for are appended to
    /// `submittedLines`; running them is up to the caller.
    void draw(Engine& engine, EditorContext& context, ShadowQuality shadows, std::vector<std::string>& submittedLines);

    /// What the panel lists and what is chosen in it: the same object a command asks.
    [[nodiscard]] AssetBrowser& browser() { return m_browser; }

    [[nodiscard]] const AssetBrowser& browser() const { return m_browser; }

    /// The picture of the chosen asset beside the rows.
    [[nodiscard]] const AssetPreview& preview() const { return m_preview; }

private:
    /// What a click on a row of the tree or of the contents asked for, to be done once
    /// all of them have been drawn: the rows change with it.
    struct Click {
        /// A folder, or a file, to open or close.
        std::optional<AssetBrowserRow> toggled;
        /// The key of a folder to show.
        std::optional<std::string> shown;
        /// An asset to choose.
        std::optional<Uuid> chosen;
    };

    /// The way to the folder shown, the filter, the switch between the list and the grid,
    /// Refresh, and what is wrong with the asset directory.
    void drawToolbar(const AssetDatabase& database, std::vector<std::string>& submittedLines, Click& click);
    /// The way to the folder shown, as buttons that show the folders on it, in `width`
    /// units of the interface - the first steps left out, when it is longer.
    void drawSteps(float width, Click& click);
    void drawFolders(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click);
    void drawContents(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click);
    void drawList(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click);
    void drawGrid(const EditorContext& context, std::vector<std::string>& submittedLines, Click& click);
    /// One tile of the grid, `height` units high, with its top left corner at (`left`,
    /// `top`) on the screen: what a click, a double click, a drag and its menu do, its face,
    /// and the arrow of a file with something inside it.
    void drawTile(const EditorContext& context, const AssetBrowserRow& row, float left, float top, float height,
                  std::vector<std::string>& submittedLines, Click& click);
    /// What a row of the tree or of the contents does that is the same whatever it looks
    /// like, for the item drawn last: its menu, what a drag of it carries, what a drop on
    /// it moves, and what a double click does.
    void handleRow(const EditorContext& context, const AssetBrowserRow& row, bool doubleClicked,
                   std::vector<std::string>& submittedLines, Click& click);
    void drawSelection(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The buttons that list what the chosen asset needs and what needs it, and their lists.
    void drawUses(const EditorContext& context, const AssetInfo& asset, std::vector<std::string>& submittedLines);
    /// The entities of the open scene in the list of what needs the chosen asset; a click
    /// selects one, or all.
    void drawSceneUsers(std::vector<std::string>& submittedLines) const;
    /// The dialog in which a file is given another name or place, once a menu asked for it.
    void drawMoveDialog(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The question whether to delete the file of an asset, with what uses it, and the
    /// command it is answered with.
    void drawDeleteDialog(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The menu of the contents' empty space: a new folder or material in the folder shown.
    void drawContentsMenu(const EditorContext& context);
    /// The dialog that names a new folder or material, and the command that makes it.
    void drawCreateDialog(std::vector<std::string>& submittedLines);
    /// The picture of `asset`, as large as there is room for.
    void drawPicture(Engine& engine, EditorContext& context, ShadowQuality shadows, const AssetInfo& asset);
    /// What a sound has instead of a picture: how long it is, and Play and Stop
    /// (`asset.listen`).
    void drawSound(Engine& engine, EditorContext& context, const AssetInfo& asset,
                   std::vector<std::string>& submittedLines);
    /// Does what the clicks of a frame asked for, to the browser.
    void apply(const Click& click);
    /// The place among the contents of the asset to scroll to in this frame, if any: one
    /// that something else than a click in here has chosen - a command - in the frame
    /// after it was chosen, and in the one after that, since a pane drawn for the first
    /// time does not know its size yet.
    [[nodiscard]] std::optional<size_t> rowToReveal();

    AssetBrowser m_browser;
    AssetPreview m_preview;
    /// The rows that are drawn - of the tree, and of the folder shown - and what they were
    /// worked out from: they are asked for again when the assets were read again or the
    /// browser shows something else.
    std::vector<AssetBrowserRow> m_folders;
    std::vector<AssetBrowserRow> m_contents;
    /// The key of the folder shown, and the way to it.
    std::string m_shownFolder;
    std::vector<AssetFolderStep> m_steps;
    uint64_t m_rowsOfDatabase = 0;
    uint64_t m_rowsOfBrowser = 0;
    bool m_hasRows = false;
    /// What is in the text field, as it was typed: the filter keeps it without the
    /// blanks at its ends, and a blank that was just typed must not vanish.
    std::string m_search;
    /// The asset whose file the dialog moves - or the directory, when it moves one - and
    /// where it is to go, as it is typed. The dialog opens in the frame after the menu
    /// asked: a popup is opened where it is drawn.
    Uuid m_moving;
    std::string m_movingDirectory;
    std::string m_moveTarget;
    bool m_openMoveDialog = false;
    /// The asset whose file a person asked to delete, until the question is answered.
    Uuid m_deleting;
    bool m_openDeleteDialog = false;
    /// What the dialog of something new makes, and where; None while it is closed.
    enum class Creating : uint8_t {
        None,
        Folder,
        Material
    };
    Creating m_creating = Creating::None;
    std::string m_createTarget;
    bool m_openCreateDialog = false;
    /// The entities of the open scene that use the chosen asset, with their paths: found
    /// when its list opens, not in every frame it is open.
    std::vector<std::pair<Uuid, std::string>> m_sceneUsers;
    /// The sound that was last read for the area beside the rows, at which revision of
    /// the assets, and what was found: whether it can be heard, and what is said of it.
    std::optional<Uuid> m_soundRead;
    uint64_t m_soundRevision = 0;
    bool m_soundHeard = false;
    std::string m_soundText;
    /// The asset the contents were last drawn with as the chosen one, and in how many more
    /// frames they scroll to it (rowToReveal()).
    Uuid m_shownSelection;
    uint32_t m_revealFrames = 0;
};

} // namespace cordite
