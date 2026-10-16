#pragma once

/// @file
/// The editor panel that shows the assets of the project, and puts them to use.
///
/// A tree of directories, files and what is inside files (AssetBrowser), with a filter
/// by type and by text above it and the asset that is chosen below it. A model or a
/// prefab is placed in the scene by a double click or from its menu, a scene is opened
/// from its menu, and any asset is dragged onto a property of the inspector that takes
/// one of its kind, or - a model or a prefab - onto an entity of the hierarchy.
///
/// @code
/// AssetBrowserPanel assets;
/// std::vector<std::string> submitted;
/// assets.draw(context, submitted);      // once per frame; `submitted` receives command lines
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
#include "cordite/editor/EditorContext.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cordite {

class AssetBrowserPanel {
public:
    /// What the window is known by: its place in a layout is remembered by it, whatever its
    /// tab shows (windowName() in UiWidgets.h).
    static constexpr const char* kWindowId = "###Assets";

    /// Draws the panel as its own window. Commands that the user asked for are appended
    /// to `submittedLines`; running them is up to the caller.
    void draw(EditorContext& context, std::vector<std::string>& submittedLines);

    /// What the panel lists and what is chosen in it: the same object a command asks.
    [[nodiscard]] AssetBrowser& browser() { return m_browser; }

    [[nodiscard]] const AssetBrowser& browser() const { return m_browser; }

private:
    void drawToolbar(const AssetDatabase& database, std::vector<std::string>& submittedLines);
    void drawRows(const EditorContext& context, std::vector<std::string>& submittedLines);
    void drawSelection(const EditorContext& context, std::vector<std::string>& submittedLines);
    /// The buttons that list what the chosen asset needs and what needs it, and their lists.
    void drawUses(const EditorContext& context, const AssetInfo& asset, std::vector<std::string>& submittedLines);
    /// The entities of the open scene in the list of what needs the chosen asset; a click
    /// selects one, or all.
    void drawSceneUsers(std::vector<std::string>& submittedLines) const;

    AssetBrowser m_browser;
    /// The rows that are drawn, and what they were worked out from: they are asked for
    /// again when the assets were read again or the browser shows something else.
    std::vector<AssetBrowserRow> m_rows;
    uint64_t m_rowsOfDatabase = 0;
    uint64_t m_rowsOfBrowser = 0;
    bool m_hasRows = false;
    /// What is in the text field, as it was typed: the filter keeps it without the
    /// blanks at its ends, and a blank that was just typed must not vanish.
    std::string m_search;
    /// The entities of the open scene that use the chosen asset, with their paths: found
    /// when its list opens, not in every frame it is open.
    std::vector<std::pair<Uuid, std::string>> m_sceneUsers;
    /// The asset the rows were last drawn with as the chosen one. When something else
    /// than a click in here chooses another - a command - the rows scroll to it.
    Uuid m_shownSelection;
};

} // namespace cordite
