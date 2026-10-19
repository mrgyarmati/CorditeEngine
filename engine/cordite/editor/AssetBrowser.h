#pragma once

/// @file
/// What the asset browser of the editor shows, as the browsers of Unity and Unreal Engine
/// show it: a tree of the folders of the project beside what the folder chosen in it has
/// in it - its directories and files, and what is inside a file once it is opened - as a
/// list or as a grid of tiles, narrowed down by kind and by a text.
///
/// @code
/// AssetBrowser browser;
/// for (const AssetBrowserRow& folder : browser.folders(assets.database())) {
///     // folder.depth, folder.label, folder.open, folder.selected (the one shown) ...
/// }
/// browser.showFolder("models");
/// browser.setFilter({.type = AssetType::Model, .search = "crate"});
/// for (const AssetBrowserRow& row : browser.contents(assets.database())) {
///     // row.kind, row.label, row.asset, row.type ...
/// }
/// browser.select(crateId);
/// @endcode
///
/// This is the browser without its window: which rows there are, in which order, which
/// of them are open, which folder is shown and which asset is chosen. AssetBrowserPanel
/// draws it, and the command `assets.browse` asks it the same things, so that what a
/// person sees is what a script is told. rows() is all of it as one tree, for whoever
/// wants everything at once.
///
/// The rows are worked out anew from the database every time they are asked for, and
/// hold no pointers into it: they are right after the assets were read again.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct SoundData;

/// How what a folder has in it is shown.
enum class AssetBrowserView : uint8_t {
    /// A row for each, with its type.
    List,
    /// A tile for each, side by side: a large icon, and the name under it.
    Grid,
};

inline constexpr std::array<AssetBrowserView, 2> kAssetBrowserViews = {AssetBrowserView::List, AssetBrowserView::Grid};

/// "list" or "grid".
[[nodiscard]] std::string_view nameOf(AssetBrowserView view);

/// The view of that name, whatever its case. None for a name no view has.
[[nodiscard]] std::optional<AssetBrowserView> assetBrowserViewFromName(std::string_view name);

/// The key of the folder at the top of the tree (AssetBrowserRow::key): the asset
/// directory itself, which holds everything else.
inline constexpr std::string_view kTopFolder{};

/// The key of the folder of what the engine has built in, which is in no directory and is
/// shown as one in the top: no path of a project has a '#' in it, so no directory of a
/// project - not even one called "builtin" - has this key.
inline constexpr std::string_view kBuiltinFolder = "#builtin";

/// What the top of the tree is called: the asset directory, whatever its name on the disk.
inline constexpr std::string_view kTopFolderLabel = "Assets";

/// A step of the way from the top of the tree to a folder (folderSteps()).
struct AssetFolderStep {
    /// What the folder is called: kTopFolderLabel for the top, else the name of its
    /// directory.
    std::string label;
    /// Which folder it is, to showFolder(): AssetBrowserRow::key of its row.
    std::string key;
    /// Where it is: AssetBrowserRow::path of its row - empty for the top.
    std::string path;

    [[nodiscard]] bool operator==(const AssetFolderStep&) const = default;
};

/// The way from the top of the tree to the folder of `key`, one step for each folder it
/// is in and the last for the folder itself: what the panel shows above the contents, as
/// a path that is clicked to go back up. The top alone for kTopFolder.
[[nodiscard]] std::vector<AssetFolderStep> folderSteps(std::string_view key);

/// The key of the folder at `path` of `database`, whether it is shown in the tree or not
/// and whatever the filter lists: kTopFolder for an empty path, the directory's own path
/// for a directory that holds an asset, and kBuiltinFolder for "builtin" - unless the
/// project has a directory of that name, which a path finds first, as it does for the
/// rows of AssetBrowser::find(). None for anything else: a file, or a directory that holds
/// no asset, which the browser does not know of.
[[nodiscard]] std::optional<std::string> assetFolderAt(const AssetDatabase& database, std::string_view path);

/// What of the assets is shown.
struct AssetBrowserFilter {
    /// Only assets of this type. None for assets of every type.
    std::optional<AssetType> type;
    /// Only assets with this text in their name or in their path, whatever its case.
    /// Empty for all of them.
    std::string search;

    [[nodiscard]] bool operator==(const AssetBrowserFilter&) const = default;

    /// Whether anything is left out.
    [[nodiscard]] bool isActive() const { return type.has_value() || !search.empty(); }
};

/// A line of the browser: of the tree of folders, of what a folder has in it, or of
/// rows().
struct AssetBrowserRow {
    enum class Kind : uint8_t {
        /// A directory of the project, the top of the tree, or "builtin" for what the
        /// engine has built in.
        Directory,
        /// An asset: a file, or something inside a file.
        Asset,
    };

    Kind kind = Kind::Asset;
    /// How far in the row is. In rows(): 0 at the top of the asset directory, one more
    /// for what is in a directory, and for what is inside a file. In folders(): 0 for the
    /// top, one more for each directory. In contents(): 0, and 1 for what is inside a file.
    uint32_t depth = 0;
    /// What the row is called: the name of a directory, the name of a file as it is
    /// written, the name an asset has inside its file - or kTopFolderLabel for the top.
    std::string label;
    /// What says which row this is to setOpen(), isOpen() and - for a directory -
    /// showFolder(): the path of a directory or of a file, kTopFolder for the top and
    /// kBuiltinFolder for what the engine has built in. Empty for an asset that has
    /// nothing inside it.
    std::string key;
    /// The asset of the row. Nil for a directory.
    Uuid asset;
    /// The type of the asset. Of no meaning for a directory.
    AssetType type = AssetType::Texture;
    /// AssetInfo::reference() of the asset; the path of a directory, empty for the top.
    std::string path;
    /// Whether there are rows below this one, and whether they are shown. In the tree
    /// what is below a folder is the directories in it; in contents() a directory has
    /// nothing below it - it is shown, not opened.
    bool hasChildren = false;
    bool open = false;
    /// Whether the filter asks for this asset. A file that it does not ask for is shown
    /// all the same when it asks for something inside it; a directory is never asked for.
    bool matches = false;
    /// Whether this is the asset that is chosen - or, in the tree, the folder shown.
    bool selected = false;
};

class AssetBrowser {
public:
    /// What is shown of the assets. The text of a filter is kept without the blanks at
    /// its ends: one of nothing but blanks leaves nothing out.
    [[nodiscard]] const AssetBrowserFilter& filter() const { return m_filter; }

    void setFilter(AssetBrowserFilter filter);

    /// The asset that is chosen. Nil for none. It stays chosen while it is not shown,
    /// and while it does not exist: the assets may be read again and have it back.
    [[nodiscard]] const Uuid& selection() const { return m_selection; }

    void select(const Uuid& asset);

    /// Whether the rows below a directory or a file are shown in rows() and contents().
    /// Directories - and the top of the tree - start open, files closed: what is inside a
    /// model is many rows that are seldom wanted. While the filter leaves something out
    /// everything is open, so that what was asked for is seen, and what was opened and
    /// closed by hand is kept for afterwards. A directory is opened and closed in the tree
    /// and in rows() alike, but the filter does not open the tree: its rows say whether
    /// they are open as they were left (AssetBrowserRow::open).
    [[nodiscard]] bool isOpen(const AssetBrowserRow& row) const;
    void setOpen(const AssetBrowserRow& row, bool open);

    /// The folder whose contents are shown, by its key (AssetBrowserRow::key): kTopFolder
    /// at first, the path of a directory, or kBuiltinFolder. It stays the folder while it
    /// does not exist - the assets may be read again and have it back - and what is
    /// shown meanwhile is the nearest folder above it that does (shownFolder()).
    [[nodiscard]] const std::string& folder() const { return m_folder; }

    /// Shows the contents of the folder of `key` - the key of a directory's row, or of a
    /// step of folderSteps() - and opens the folders above it, so that its row is seen in
    /// the tree. A key that is no folder shows the nearest folder above it that is.
    void showFolder(std::string_view key);

    /// The key of the folder whose contents are shown: folder() where it exists, else the
    /// nearest folder above it that does, at last the top.
    [[nodiscard]] std::string shownFolder(const AssetDatabase& database) const;

    /// The tree beside the contents, from top to bottom: the top of the asset directory,
    /// called kTopFolderLabel, and below it its directories in the order of their names,
    /// each with the directories in it below it, and what the engine has built in last;
    /// nothing below a row that is closed. The filter does not narrow it. A directory is
    /// in the tree when an asset is in it or below it; the folder shown is selected.
    [[nodiscard]] std::vector<AssetBrowserRow> folders(const AssetDatabase& database) const;

    /// What the folder shown has in it, from first to last: its directories in the order
    /// of their names - what the engine has built in last, in the top - and then its
    /// files in the order of their names, each followed by what is inside it when it is
    /// open, one further in. While the filter leaves something out: every asset that it
    /// asks for in the folder and in the directories below it, in the order of rows(),
    /// none further in than another and no directory among them.
    [[nodiscard]] std::vector<AssetBrowserRow> contents(const AssetDatabase& database) const;

    /// How the contents are shown: a grid at first.
    [[nodiscard]] AssetBrowserView view() const { return m_view; }

    void setView(AssetBrowserView view) { m_view = view; }

    /// All the assets as one tree, from top to bottom: directories and files in the order
    /// of their names, what is inside a file below it in the order it has there, and
    /// nothing below a row that is closed. What the engine has built in comes last, in a
    /// directory called "builtin". A directory is listed when something that is listed is
    /// in it.
    [[nodiscard]] std::vector<AssetBrowserRow> rows(const AssetDatabase& database) const;

    /// Shows where an asset is: the folder it is in, with the folders above it opened in
    /// the tree and the file it is inside opened, so that its row is among contents() and
    /// rows(). Returns whether it is: the filter may not list it, and then it is given its
    /// place all the same, to be seen once the filter lists it. An asset that does not
    /// exist has no place, and nothing changes.
    bool reveal(const AssetDatabase& database, const Uuid& asset);

    /// The row of rows() - of the directory, the file or the asset inside a file - that
    /// has this path (AssetBrowserRow::path), whether it is shown or below a row that is
    /// closed. None when the filter lists nothing there.
    [[nodiscard]] std::optional<AssetBrowserRow> find(const AssetDatabase& database, std::string_view path) const;

    /// Counts up whenever rows(), folders() or contents() may give something else for the
    /// same database: with another filter, another selection, another folder shown, a row
    /// opened or closed. For whoever keeps the rows instead of asking for them in every
    /// frame.
    [[nodiscard]] uint64_t revision() const { return m_revision; }

private:
    /// Every row the filter lists, as rows() gives them, with those below a row that is
    /// closed.
    [[nodiscard]] std::vector<AssetBrowserRow> allRows(const AssetDatabase& database) const;

    /// Opens the folders of the tree that the folder of `key` is in - not that folder
    /// itself. Returns whether any of them was closed.
    bool openFoldersAbove(std::string_view key);

    AssetBrowserFilter m_filter;
    Uuid m_selection;
    std::string m_folder{kTopFolder};
    AssetBrowserView m_view = AssetBrowserView::Grid;
    uint64_t m_revision = 0;
    /// The keys of the rows that are not as they start: directories that were closed,
    /// files that were opened.
    std::set<std::string, std::less<>> m_toggled;
};

/// What the Assets panel asks for when an asset is to be placed in the scene, as a line
/// for the console: `asset.instantiate`, with the entity that is selected as the parent
/// - new things go under what is selected, as a new entity does - and none when
/// nothing is selected.
[[nodiscard]] std::string placeAssetCommand(const EditorContext& context, const Uuid& asset);

/// What the Assets panel asks for when a scene of the project is to be opened, as a line
/// for the console: `scene.open`, with where the scene is among the assets.
[[nodiscard]] std::string openSceneCommand(const AssetInfo& scene);

/// The entities of the open scene that refer to `asset`, each once, in the order of the
/// hierarchy: what the Assets panel lists of the scene under "Used by".
[[nodiscard]] std::vector<Uuid> entitiesUsing(const EditorContext& context, const Uuid& asset);

/// What the Assets panel asks for when entities are to be selected, as a line for the
/// console: `selection.set`, with their ids.
[[nodiscard]] std::string selectEntitiesCommand(std::span<const Uuid> entities);

/// What the Assets panel asks for when the file of an asset is renamed or moved, as a
/// line for the console: `asset.move`.
[[nodiscard]] std::string moveAssetCommand(const Uuid& asset, std::string_view to);

/// What the Assets panel asks for when a sound is to be heard, as a line for the console:
/// `asset.listen`, with the sound - or without one, which stops what is heard.
[[nodiscard]] std::string listenCommand(std::optional<Uuid> sound);

/// What the Assets panel says of a sound where other assets have a picture: how long it
/// lasts, its channels and its rate, as in "1.50 s, stereo, 44100 Hz".
[[nodiscard]] std::string describeSound(const SoundData& sound);

/// Where the file of an asset goes when it is dropped onto a row of the browser: into the
/// directory of the row, under its own name - into the top, for the top of the tree.
/// Empty when the row is no directory of the project - an asset, or what the engine has
/// built in - or holds the file already, and for an asset that has no file of its own to
/// move: one that is built in or inside a file.
[[nodiscard]] std::string dropTarget(const AssetInfo& asset, const AssetBrowserRow& row);

/// Whether a row is a directory of the project's own - the top of the tree among them: not
/// a file, nor what the engine has built in, which the browser shows as a directory too.
[[nodiscard]] bool isProjectDirectory(const AssetBrowserRow& row);

/// Whether a row is a directory that can be renamed, moved and dragged: one of the
/// project's own that is not the top of the tree, which is the asset directory itself.
[[nodiscard]] bool isMovableDirectory(const AssetBrowserRow& row);

/// What the Assets panel asks for when a directory is renamed or moved, as a line for the
/// console: `asset.moveDirectory`.
[[nodiscard]] std::string moveDirectoryCommand(std::string_view from, std::string_view to);

/// Where the directory `directory` goes when it is dropped onto a row of the browser: into
/// the directory of the row, under its own name - into the top, for the top of the tree.
/// Empty when the row is no directory of the project, is the directory itself or inside
/// it, or holds it already.
[[nodiscard]] std::string directoryDropTarget(std::string_view directory, const AssetBrowserRow& row);

/// Where files dropped onto the editor from outside are brought in: the directory whose
/// contents the browser shows (AssetBrowser::shownFolder()), as in the browsers of Unity
/// and Unreal Engine - or the top, while it shows what the engine has built in.
[[nodiscard]] std::string importDirectory(const AssetBrowser& browser, const AssetDatabase& database);

/// What the editor asks for when files are dropped onto it, as a line for the console:
/// `asset.import`, with their paths and the directory they go into, empty for the top.
[[nodiscard]] std::string importAssetsCommand(std::span<const std::filesystem::path> files, std::string_view to);

} // namespace cordite
