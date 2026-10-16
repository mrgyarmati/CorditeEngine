#pragma once

/// @file
/// The editor panel that shows the assets of the project, and puts them to use.
///
/// A tree of directories, files and what is inside files (AssetBrowser), with a filter
/// by type and by text above it, the asset that is chosen below it, and beside it a
/// picture of that asset when it has one (AssetPreview). A model or a
/// prefab is placed in the scene by a double click or from its menu, a scene is opened
/// from its menu, and any asset is dragged onto a property of the inspector that takes
/// one of its kind, or - a model or a prefab - onto an entity of the hierarchy. A sound
/// has its length beside the rows instead of a picture, and is heard with Play.
///
/// @code
/// AssetBrowserPanel assets;
/// std::vector<std::string> submitted;
/// assets.draw(engine, context, ShadowQuality::Medium, submitted);   // once per frame
/// assets.browser().setFilter({.type = AssetType::Prefab});
/// @endcode
///
/// What the panel does it does by commands - `asset.instantiate`, `scene.open`,
/// `asset.refresh` - which it hands to the caller as lines to run, as the console does:
/// they change the scene and the assets, which is for between two frames, and what a
/// click did is then in the log like what was typed.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/editor/AssetPreview.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/Engine.h"

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
    void drawToolbar(const AssetDatabase& database, std::vector<std::string>& submittedLines);
    void drawRows(const EditorContext& context, std::vector<std::string>& submittedLines);
    void drawSelection(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The buttons that list what the chosen asset needs and what needs it, and their lists.
    void drawUses(const EditorContext& context, const AssetInfo& asset, std::vector<std::string>& submittedLines);
    /// The entities of the open scene in the list of what needs the chosen asset; a click
    /// selects one, or all.
    void drawSceneUsers(std::vector<std::string>& submittedLines) const;
    /// The dialog in which a file is given another name or place, once a menu asked for it.
    void drawMoveDialog(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The picture of `asset`, as large as there is room for.
    void drawPicture(Engine& engine, EditorContext& context, ShadowQuality shadows, const AssetInfo& asset);
    /// What a sound has instead of a picture: how long it is, and Play and Stop
    /// (`asset.listen`).
    void drawSound(Engine& engine, EditorContext& context, const AssetInfo& asset,
                   std::vector<std::string>& submittedLines);

    AssetBrowser m_browser;
    AssetPreview m_preview;
    /// The rows that are drawn, and what they were worked out from: they are asked for
    /// again when the assets were read again or the browser shows something else.
    std::vector<AssetBrowserRow> m_rows;
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
    /// The entities of the open scene that use the chosen asset, with their paths: found
    /// when its list opens, not in every frame it is open.
    std::vector<std::pair<Uuid, std::string>> m_sceneUsers;
    /// The sound that was last read for the area beside the rows, at which revision of
    /// the assets, and what was found: whether it can be heard, and what is said of it.
    std::optional<Uuid> m_soundRead;
    uint64_t m_soundRevision = 0;
    bool m_soundHeard = false;
    std::string m_soundText;
    /// The asset the rows were last drawn with as the chosen one. When something else
    /// than a click in here chooses another - a command - the rows scroll to it.
    Uuid m_shownSelection;
};

} // namespace cordite
