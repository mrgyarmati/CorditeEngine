#pragma once

/// @file
/// What the asset browser of the editor shows: the assets of a project as a tree of
/// directories, files and what is inside files, narrowed down by kind and by a text.
///
/// @code
/// AssetBrowser browser;
/// browser.setFilter({.type = AssetType::Model, .search = "crate"});
/// for (const AssetBrowserRow& row : browser.rows(assets.database())) {
///     // row.depth, row.label, row.asset, row.type ...
/// }
/// browser.select(crateId);
/// @endcode
///
/// This is the browser without its window: which rows there are, in which order, which
/// of them are open and which asset is chosen. AssetBrowserPanel draws it, and the
/// command `assets.browse` asks it the same things, so that what a person sees is what a
/// script is told.
///
/// The rows are worked out anew from the database every time they are asked for, and
/// hold no pointers into it: they are right after the assets were read again.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"

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

/// A line of the browser.
struct AssetBrowserRow {
    enum class Kind : uint8_t {
        /// A directory of the project, or "builtin" for what the engine has built in.
        Directory,
        /// An asset: a file, or something inside a file.
        Asset,
    };

    Kind kind = Kind::Asset;
    /// How far in the row is: 0 at the top of the asset directory, one more for what is
    /// in a directory, and for what is inside a file.
    uint32_t depth = 0;
    /// What the row is called: the name of a directory, the name of a file as it is
    /// written, or the name an asset has inside its file.
    std::string label;
    /// What says which row this is to open(), close() and isOpen(): the path of the
    /// directory or of the file. Empty for a row that has nothing below it.
    std::string key;
    /// The asset of the row. Nil for a directory.
    Uuid asset;
    /// The type of the asset. Of no meaning for a directory.
    AssetType type = AssetType::Texture;
    /// AssetInfo::reference() of the asset; the path of a directory.
    std::string path;
    /// Whether there are rows below this one, and whether they are shown.
    bool hasChildren = false;
    bool open = false;
    /// Whether the filter asks for this asset. A file that it does not ask for is shown
    /// all the same when it asks for something inside it; a directory is never asked for.
    bool matches = false;
    /// Whether this is the asset that is chosen.
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

    /// Whether the rows below a directory or a file are shown. Directories start open,
    /// files closed: what is inside a model is many rows that are seldom wanted. While
    /// the filter leaves something out everything is open, so that what was asked for
    /// is seen, and what was opened and closed by hand is kept for afterwards.
    [[nodiscard]] bool isOpen(const AssetBrowserRow& row) const;
    void setOpen(const AssetBrowserRow& row, bool open);

    /// The rows to show, from top to bottom: directories and files in the order of their
    /// names, what is inside a file below it in the order it has there, and nothing below
    /// a row that is closed. What the engine has built in comes last, in a directory
    /// called "builtin". A directory is listed when something that is listed is in it.
    [[nodiscard]] std::vector<AssetBrowserRow> rows(const AssetDatabase& database) const;

    /// Opens what an asset is in - the directories above it, and the file it is inside -
    /// so that its row is among rows(). Returns whether it is: an asset the filter does
    /// not list, or that does not exist, has no row to show, and nothing is opened.
    bool reveal(const AssetDatabase& database, const Uuid& asset);

    /// The row of the directory, the file or the asset inside a file that has this path
    /// (AssetBrowserRow::path), whether it is shown or below a row that is closed. None
    /// when the filter lists nothing there.
    [[nodiscard]] std::optional<AssetBrowserRow> find(const AssetDatabase& database, std::string_view path) const;

    /// Counts up whenever rows() may give something else for the same database: with
    /// another filter, another selection, a row opened or closed. For whoever keeps the
    /// rows instead of asking for them in every frame.
    [[nodiscard]] uint64_t revision() const { return m_revision; }

private:
    /// Every row the filter lists, as rows() gives them, with those below a row that is
    /// closed.
    [[nodiscard]] std::vector<AssetBrowserRow> allRows(const AssetDatabase& database) const;

    AssetBrowserFilter m_filter;
    Uuid m_selection;
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

/// Where the file of an asset goes when it is dropped onto a row of the browser: into the
/// directory of the row, under its own name. Empty when the row is no directory of the
/// project - an asset, or what the engine has built in - or holds the file already, and
/// for an asset that has no file of its own to move: one that is built in or inside a file.
[[nodiscard]] std::string dropTarget(const AssetInfo& asset, const AssetBrowserRow& row);

/// Where files dropped onto the editor from outside are brought in: the directory of the
/// asset that is chosen in the browser - of its file, for one inside a file - or the top,
/// when nothing is chosen or what is chosen is built in.
[[nodiscard]] std::string importDirectory(const AssetBrowser& browser, const AssetDatabase& database);

/// What the editor asks for when files are dropped onto it, as a line for the console:
/// `asset.import`, with their paths and the directory they go into, empty for the top.
[[nodiscard]] std::string importAssetsCommand(std::span<const std::filesystem::path> files, std::string_view to);

} // namespace cordite
