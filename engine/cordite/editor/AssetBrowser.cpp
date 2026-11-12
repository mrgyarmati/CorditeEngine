#include "cordite/editor/AssetBrowser.h"

#include "cordite/assets/Sound.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/StringUtils.h"
#include "cordite/engine/SceneAssets.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// A file of the project with what is inside it - or, for "builtin", what the engine has
/// built in, which is in no file.
struct File {
    /// AssetInfo::path of everything in here.
    std::string_view path;
    /// The asset that is the file itself. None for what is built in.
    const AssetInfo* asset = nullptr;
    /// What is inside the file, in the order the database has it.
    std::vector<const AssetInfo*> inside;
};

/// The assets of a database by file, in the order of their paths, what is built in last.
std::vector<File> filesOf(const AssetDatabase& database) {
    std::vector<File> files;
    std::unordered_map<std::string_view, size_t> byPath;
    for (const AssetInfo& asset : database.assets()) {
        const auto [found, added] = byPath.try_emplace(asset.path, files.size());
        if (added) {
            files.push_back({.path = asset.path});
        }
        File& file = files[found->second];
        if (asset.subPath.empty()) {
            file.asset = &asset;
        } else {
            file.inside.push_back(&asset);
        }
    }
    std::ranges::sort(files, [](const File& a, const File& b) {
        const bool aBuiltin = a.path == kBuiltinAssetPath;
        const bool bBuiltin = b.path == kBuiltinAssetPath;
        return aBuiltin != bBuiltin ? bBuiltin : a.path < b.path;
    });
    return files;
}

/// The directories a file is in, from the top down: none for a file at the top.
std::vector<std::string_view> directoriesOf(std::string_view path) {
    std::vector<std::string_view> directories;
    size_t begin = 0;
    for (size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', begin)) {
        directories.push_back(path.substr(begin, slash - begin));
        begin = slash + 1;
    }
    return directories;
}

std::string_view fileNameOf(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// The directory a path is in: what comes before its last '/', the top for a path that
/// has none.
std::string_view parentOf(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? kTopFolder : path.substr(0, slash);
}

/// `name` in the directory `directory`, which is the top when it is empty.
std::string pathIn(std::string_view directory, std::string_view name) {
    return directory.empty() ? std::string(name) : fmt::format("{}/{}", directory, name);
}

/// Every directory of a project that an asset is in, or below, by the directory it is in -
/// those at the top under kTopFolder - with the names in each in their order.
using DirectoryTree = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;

DirectoryTree directoryTreeOf(const AssetDatabase& database) {
    DirectoryTree tree;
    for (const AssetInfo& asset : database.assets()) {
        if (asset.isBuiltin()) {
            continue;
        }
        const std::string_view path = asset.path;
        size_t begin = 0;
        for (size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', begin)) {
            tree[std::string(begin == 0 ? kTopFolder : path.substr(0, begin - 1))].emplace(
                path.substr(begin, slash - begin));
            begin = slash + 1;
        }
    }
    // A directory that holds no asset is a folder all the same: one that was just made.
    for (const std::string& directory : database.directories()) {
        size_t begin = 0;
        while (begin < directory.size()) {
            const size_t slash = std::min(directory.find('/', begin), directory.size());
            tree[std::string(begin == 0 ? kTopFolder : std::string_view(directory).substr(0, begin - 1))].emplace(
                directory.substr(begin, slash - begin));
            begin = slash + 1;
        }
    }
    return tree;
}

bool hasBuiltins(const AssetDatabase& database) {
    return std::ranges::any_of(database.assets(), &AssetInfo::isBuiltin);
}

/// Whether there is a folder of `key` among the directories of `tree`, and the folder of
/// what the engine has built in when there is something built in.
bool folderExists(const DirectoryTree& tree, bool builtins, std::string_view key) {
    if (key == kTopFolder) {
        return true;
    }
    if (key == kBuiltinFolder) {
        return builtins;
    }
    // "/textures" is in nothing: its parent is no directory, though it is written as none.
    if (key.starts_with('/')) {
        return false;
    }
    const auto in = tree.find(parentOf(key));
    return in != tree.end() && in->second.contains(fileNameOf(key));
}

/// The folder of `key` if there is one, else the nearest above it that there is: the top
/// at last, which always is. What the engine has built in is in the top.
std::string nearestFolder(const DirectoryTree& tree, bool builtins, std::string_view key) {
    std::string_view folder = key;
    while (!folderExists(tree, builtins, folder)) {
        folder = parentOf(folder);
    }
    return std::string(folder);
}

/// Whether a file is in the folder of `key` or in a directory below it.
bool isWithin(const File& file, std::string_view key) {
    const bool builtin = file.path == kBuiltinAssetPath;
    if (key == kTopFolder) {
        return true;
    }
    if (key == kBuiltinFolder) {
        return builtin;
    }
    return !builtin && file.path.size() > key.size() && file.path.starts_with(key) && file.path[key.size()] == '/';
}

/// Whether a filter asks for an asset.
class FilterTest {
public:
    explicit FilterTest(const AssetBrowserFilter& filter)
        : m_type(filter.type), m_needle(toLowerAscii(filter.search)) {}

    [[nodiscard]] bool operator()(const AssetInfo& asset) const {
        if (m_type && asset.type != *m_type) {
            return false;
        }
        return m_needle.empty() || toLowerAscii(asset.name).find(m_needle) != std::string::npos ||
               toLowerAscii(asset.reference()).find(m_needle) != std::string::npos;
    }

private:
    std::optional<AssetType> m_type;
    std::string m_needle;
};

/// The row of an asset, with nothing below it.
AssetBrowserRow assetRow(const AssetInfo& asset, uint32_t depth, std::string_view label, bool matches,
                         const Uuid& selection) {
    AssetBrowserRow row;
    row.kind = AssetBrowserRow::Kind::Asset;
    row.depth = depth;
    row.label = label;
    row.asset = asset.id;
    row.type = asset.type;
    row.path = asset.reference();
    row.matches = matches;
    row.selected = asset.id == selection;
    return row;
}

/// The row of a directory - or of what the engine has built in - with nothing below it.
AssetBrowserRow directoryRow(uint32_t depth, std::string_view label, std::string_view key, std::string_view path) {
    AssetBrowserRow row;
    row.kind = AssetBrowserRow::Kind::Directory;
    row.depth = depth;
    row.label = label;
    row.key = key;
    row.path = path;
    return row;
}

/// What a search finds among `files` in the folder of `key` and below it: every asset it
/// asks for, side by side, in the order of the files.
std::vector<AssetBrowserRow> foundIn(std::span<const File> files, std::string_view key, const FilterTest& asked,
                                     const Uuid& selection) {
    std::vector<AssetBrowserRow> found;
    for (const File& file : files) {
        if (!isWithin(file, key)) {
            continue;
        }
        if (file.asset != nullptr && asked(*file.asset)) {
            found.push_back(assetRow(*file.asset, 0, fileNameOf(file.path), true, selection));
        }
        for (const AssetInfo* asset : file.inside) {
            if (asked(*asset)) {
                found.push_back(assetRow(*asset, 0, asset->name, true, selection));
            }
        }
    }
    return found;
}

} // namespace

std::string_view nameOf(AssetBrowserView view) {
    switch (view) {
        case AssetBrowserView::List:
            return "list";
        case AssetBrowserView::Grid:
            return "grid";
    }
    return "unknown";
}

std::optional<AssetBrowserView> assetBrowserViewFromName(std::string_view name) {
    for (const AssetBrowserView view : kAssetBrowserViews) {
        if (equalsIgnoreCase(name, nameOf(view))) {
            return view;
        }
    }
    return std::nullopt;
}

std::vector<AssetFolderStep> folderSteps(std::string_view key) {
    std::vector<AssetFolderStep> steps;
    steps.push_back({.label = std::string(kTopFolderLabel), .key = std::string(kTopFolder), .path = ""});
    if (key == kTopFolder) {
        return steps;
    }
    if (key == kBuiltinFolder) {
        steps.push_back({.label = std::string(kBuiltinAssetPath),
                         .key = std::string(kBuiltinFolder),
                         .path = std::string(kBuiltinAssetPath)});
        return steps;
    }
    // A step for every directory on the way, the folder itself the last.
    size_t begin = 0;
    for (;;) {
        const size_t slash = key.find('/', begin);
        const std::string_view through = slash == std::string_view::npos ? key : key.substr(0, slash);
        steps.push_back(
            {.label = std::string(through.substr(begin)), .key = std::string(through), .path = std::string(through)});
        if (slash == std::string_view::npos) {
            return steps;
        }
        begin = slash + 1;
    }
}

std::optional<std::string> assetFolderAt(const AssetDatabase& database, std::string_view path) {
    if (path.empty()) {
        return std::string(kTopFolder);
    }
    // A directory of the project first, though it is called "builtin".
    if (folderExists(directoryTreeOf(database), false, path)) {
        return std::string(path);
    }
    if (path == kBuiltinAssetPath && hasBuiltins(database)) {
        return std::string(kBuiltinFolder);
    }
    return std::nullopt;
}

void AssetBrowser::setFilter(AssetBrowserFilter filter) {
    // Blanks around the text are not looked for, and a text of nothing else is none.
    filter.search = std::string(trim(filter.search));
    if (filter != m_filter) {
        m_filter = std::move(filter);
        ++m_revision;
    }
}

void AssetBrowser::select(const Uuid& asset) {
    if (asset != m_selection) {
        m_selection = asset;
        ++m_revision;
    }
}

bool AssetBrowser::isOpen(const AssetBrowserRow& row) const {
    if (!row.hasChildren) {
        return false;
    }
    if (m_filter.isActive()) {
        return true;
    }
    const bool startsOpen = row.kind == AssetBrowserRow::Kind::Directory;
    return startsOpen != m_toggled.contains(row.key);
}

void AssetBrowser::setOpen(const AssetBrowserRow& row, bool open) {
    if (!row.hasChildren) {
        return;
    }
    const bool startsOpen = row.kind == AssetBrowserRow::Kind::Directory;
    const bool changed = open == startsOpen ? m_toggled.erase(row.key) > 0 : m_toggled.insert(row.key).second;
    if (changed) {
        ++m_revision;
    }
}

bool AssetBrowser::openFoldersAbove(std::string_view key) {
    if (key == kTopFolder) {
        return false;
    }
    // Folders start open: a folder that was closed is among those toggled.
    bool changed = m_toggled.erase(std::string(kTopFolder)) > 0;
    if (key == kBuiltinFolder) {
        return changed;
    }
    for (size_t slash = key.find('/'); slash != std::string_view::npos; slash = key.find('/', slash + 1)) {
        changed = m_toggled.erase(std::string(key.substr(0, slash))) > 0 || changed;
    }
    return changed;
}

void AssetBrowser::showFolder(std::string_view key) {
    bool changed = openFoldersAbove(key);
    if (m_folder != key) {
        m_folder = std::string(key);
        changed = true;
    }
    if (changed) {
        ++m_revision;
    }
}

std::string AssetBrowser::shownFolder(const AssetDatabase& database) const {
    return nearestFolder(directoryTreeOf(database), hasBuiltins(database), m_folder);
}

std::vector<AssetBrowserRow> AssetBrowser::folders(const AssetDatabase& database) const {
    const DirectoryTree tree = directoryTreeOf(database);
    const bool builtins = hasBuiltins(database);
    const std::string shown = nearestFolder(tree, builtins, m_folder);

    std::vector<AssetBrowserRow> folders;
    // Adds the row of a folder, and returns whether it is open. Folders start open, and the
    // filter has no say in it: the tree is not narrowed by it.
    const auto add = [&](AssetBrowserRow row, bool hasChildren) {
        row.hasChildren = hasChildren;
        row.open = hasChildren && !m_toggled.contains(row.key);
        row.selected = row.key == shown;
        const bool open = row.open;
        folders.push_back(std::move(row));
        return open;
    };
    if (!add(directoryRow(0, kTopFolderLabel, kTopFolder, ""), tree.contains(kTopFolder) || builtins)) {
        return folders;
    }

    // The directories below the top, depth first: each is followed by those in it, while
    // it is open. Not by calling a function for each, which a deep tree would take the
    // stack of the program for.
    std::vector<std::pair<std::string, uint32_t>> pending;
    const auto push = [&](std::string_view directory, uint32_t depth) {
        const auto in = tree.find(directory);
        if (in == tree.end()) {
            return;
        }
        // The last first: what is taken from the back comes out in its order.
        for (const std::string& name : std::views::reverse(in->second)) {
            pending.emplace_back(pathIn(directory, name), depth);
        }
    };
    push(kTopFolder, 1);
    while (!pending.empty()) {
        auto [path, depth] = std::move(pending.back());
        pending.pop_back();
        if (add(directoryRow(depth, fileNameOf(path), path, path), tree.contains(path))) {
            push(path, depth + 1);
        }
    }
    if (builtins) {
        add(directoryRow(1, kBuiltinAssetPath, kBuiltinFolder, kBuiltinAssetPath), false);
    }
    return folders;
}

std::vector<AssetBrowserRow> AssetBrowser::contents(const AssetDatabase& database) const {
    const DirectoryTree tree = directoryTreeOf(database);
    const bool builtins = hasBuiltins(database);
    const std::string shown = nearestFolder(tree, builtins, m_folder);
    if (m_filter.isActive()) {
        return foundIn(filesOf(database), shown, FilterTest(m_filter), m_selection);
    }

    // The directories, which are shown and not opened: nothing is below them here.
    std::vector<AssetBrowserRow> contents;
    if (const auto in = tree.find(shown); in != tree.end()) {
        for (const std::string& name : in->second) {
            const std::string path = pathIn(shown, name);
            contents.push_back(directoryRow(0, name, path, path));
        }
    }
    if (shown == kTopFolder && builtins) {
        contents.push_back(directoryRow(0, kBuiltinAssetPath, kBuiltinFolder, kBuiltinAssetPath));
    }

    for (const File& file : filesOf(database)) {
        if (file.path == kBuiltinAssetPath) {
            // What the engine has built in is in no file: it is what its folder has in it.
            if (shown == kBuiltinFolder) {
                for (const AssetInfo* asset : file.inside) {
                    contents.push_back(assetRow(*asset, 0, asset->name, true, m_selection));
                }
            }
            continue;
        }
        if (shown == kBuiltinFolder || parentOf(file.path) != shown) {
            continue;
        }
        uint32_t depth = 0;
        if (file.asset != nullptr) {
            AssetBrowserRow row = assetRow(*file.asset, 0, fileNameOf(file.path), true, m_selection);
            row.hasChildren = !file.inside.empty();
            row.key = row.hasChildren ? std::string(file.path) : std::string();
            row.open = isOpen(row);
            const bool open = row.open;
            contents.push_back(std::move(row));
            if (!open) {
                continue;
            }
            depth = 1;
        }
        for (const AssetInfo* asset : file.inside) {
            contents.push_back(assetRow(*asset, depth, asset->name, true, m_selection));
        }
    }
    return contents;
}

std::vector<AssetBrowserRow> AssetBrowser::allRows(const AssetDatabase& database) const {
    const FilterTest asked(m_filter);
    std::vector<AssetBrowserRow> all;
    std::vector<std::string_view> entered;
    for (const File& file : filesOf(database)) {
        std::vector<const AssetInfo*> inside;
        std::ranges::copy_if(file.inside, std::back_inserter(inside),
                             [&](const AssetInfo* asset) { return asked(*asset); });
        const bool fileAsked = file.asset != nullptr && asked(*file.asset);
        if (!fileAsked && inside.empty()) {
            continue;
        }

        // The directories this file is in and the one before it was not.
        const bool builtin = file.path == kBuiltinAssetPath;
        const std::vector<std::string_view> directories =
            builtin ? std::vector<std::string_view>{kBuiltinAssetPath} : directoriesOf(file.path);
        size_t shared = 0;
        while (shared < entered.size() && shared < directories.size() && entered[shared] == directories[shared]) {
            ++shared;
        }
        entered.resize(shared);
        for (size_t level = shared; level < directories.size(); ++level) {
            entered.push_back(directories[level]);
            // A directory is its path from the top, which is where the name of the last
            // of its directories ends in the path of the file.
            const std::string path =
                builtin ? std::string(kBuiltinAssetPath)
                        : std::string(
                              file.path.substr(0, static_cast<size_t>(directories[level].data() +
                                                                      directories[level].size() - file.path.data())));
            AssetBrowserRow row = directoryRow(static_cast<uint32_t>(level), directories[level],
                                               builtin ? kBuiltinFolder : std::string_view(path), path);
            row.hasChildren = true;
            all.push_back(std::move(row));
        }

        auto depth = static_cast<uint32_t>(directories.size());
        if (file.asset != nullptr) {
            AssetBrowserRow row = assetRow(*file.asset, depth, fileNameOf(file.path), fileAsked, m_selection);
            row.hasChildren = !inside.empty();
            row.key = row.hasChildren ? std::string(file.path) : std::string();
            all.push_back(std::move(row));
            ++depth;
        }
        for (const AssetInfo* asset : inside) {
            all.push_back(assetRow(*asset, depth, asset->name, true, m_selection));
        }
    }

    for (AssetBrowserRow& row : all) {
        row.open = isOpen(row);
    }
    return all;
}

std::vector<AssetBrowserRow> AssetBrowser::rows(const AssetDatabase& database) const {
    // Of everything that is listed, what is not below a row that is closed.
    std::vector<AssetBrowserRow> all = allRows(database);
    std::vector<AssetBrowserRow> shown;
    shown.reserve(all.size());
    std::optional<uint32_t> closedAt;
    for (AssetBrowserRow& row : all) {
        if (closedAt && row.depth > *closedAt) {
            continue;
        }
        closedAt.reset();
        if (row.hasChildren && !row.open) {
            closedAt = row.depth;
        }
        shown.push_back(std::move(row));
    }
    return shown;
}

bool AssetBrowser::reveal(const AssetDatabase& database, const Uuid& asset) {
    const AssetInfo* found = database.find(asset);
    if (found == nullptr) {
        return false;
    }
    // The folder it is in, open in the tree and in rows() - which lists what is in a
    // directory only while the directory is open - and the file it is inside, opened.
    const std::string folder = found->isBuiltin() ? std::string(kBuiltinFolder) : std::string(parentOf(found->path));
    bool changed = openFoldersAbove(folder);
    changed = m_toggled.erase(folder) > 0 || changed;
    if (!found->isBuiltin() && !found->subPath.empty()) {
        changed = m_toggled.insert(found->path).second || changed;
    }
    if (m_folder != folder) {
        m_folder = folder;
        changed = true;
    }
    if (changed) {
        ++m_revision;
    }
    return !m_filter.isActive() || FilterTest(m_filter)(*found);
}

std::optional<AssetBrowserRow> AssetBrowser::find(const AssetDatabase& database, std::string_view path) const {
    std::vector<AssetBrowserRow> all = allRows(database);
    const auto found = std::ranges::find(all, path, &AssetBrowserRow::path);
    return found != all.end() ? std::optional(std::move(*found)) : std::nullopt;
}

std::string placeAssetCommand(const EditorContext& context, const Uuid& asset) {
    Json parameters = Json::object();
    parameters["asset"] = asset.toString();
    if (const Entity parent = context.primarySelection(); !parent.isNull()) {
        parameters["parent"] = context.world().id(parent).toString();
    }
    return fmt::format("asset.instantiate {}", parameters.dump());
}

std::vector<Uuid> entitiesUsing(const EditorContext& context, const Uuid& asset) {
    const World& world = context.world();
    std::vector<Uuid> users;
    // The uses of one entity come one after another: the walk goes entity by entity.
    for (const AssetReferenceUse& use : collectAssetReferences(world, context.components())) {
        if (const Uuid id = world.id(use.entity); use.asset == asset && (users.empty() || users.back() != id)) {
            users.push_back(id);
        }
    }
    return users;
}

std::string selectEntitiesCommand(std::span<const Uuid> entities) {
    Json ids = Json::array();
    for (const Uuid& entity : entities) {
        ids.push_back(entity.toString());
    }
    Json parameters = Json::object();
    parameters["entities"] = std::move(ids);
    return fmt::format("selection.set {}", parameters.dump());
}

std::string moveAssetCommand(const Uuid& asset, std::string_view to) {
    Json parameters = Json::object();
    parameters["asset"] = asset.toString();
    parameters["to"] = to;
    return fmt::format("asset.move {}", parameters.dump());
}

std::string deleteAssetCommand(const Uuid& asset, bool force) {
    Json parameters = Json::object();
    parameters["asset"] = asset.toString();
    if (force) {
        parameters["force"] = true;
    }
    return fmt::format("asset.delete {}", parameters.dump());
}

std::string createDirectoryCommand(std::string_view path) {
    Json parameters = Json::object();
    parameters["path"] = std::string(path);
    return fmt::format("asset.createDirectory {}", parameters.dump());
}

std::string createMaterialCommand(std::string_view path) {
    Json parameters = Json::object();
    parameters["path"] = std::string(path);
    return fmt::format("asset.createMaterial {}", parameters.dump());
}

std::string listenCommand(std::optional<Uuid> sound) {
    if (!sound) {
        return "asset.listen";
    }
    Json parameters = Json::object();
    parameters["asset"] = sound->toString();
    return fmt::format("asset.listen {}", parameters.dump());
}

std::string describeSound(const SoundData& sound) {
    return fmt::format("{:.2f} s, {}, {} Hz", sound.seconds(), sound.channels == 1 ? "mono" : "stereo",
                       sound.sampleRate);
}

std::string dropTarget(const AssetInfo& asset, const AssetBrowserRow& row) {
    if (!isProjectDirectory(row) || asset.isBuiltin() || !asset.subPath.empty()) {
        return {};
    }
    if (parentOf(asset.path) == row.path) {
        return {};
    }
    return pathIn(row.path, fileNameOf(asset.path));
}

bool isProjectDirectory(const AssetBrowserRow& row) {
    return row.kind == AssetBrowserRow::Kind::Directory && row.key != kBuiltinFolder;
}

bool isMovableDirectory(const AssetBrowserRow& row) {
    return isProjectDirectory(row) && !row.path.empty();
}

std::string moveDirectoryCommand(std::string_view from, std::string_view to) {
    Json parameters = Json::object();
    parameters["from"] = std::string(from);
    parameters["to"] = std::string(to);
    return fmt::format("asset.moveDirectory {}", parameters.dump());
}

std::string directoryDropTarget(std::string_view directory, const AssetBrowserRow& row) {
    if (!isProjectDirectory(row) || directory.empty()) {
        return {};
    }
    // Onto itself, or onto what is inside it.
    const std::string_view onto = row.path;
    if (onto == directory ||
        (onto.size() > directory.size() && onto.starts_with(directory) && onto[directory.size()] == '/')) {
        return {};
    }
    if (parentOf(directory) == onto) {
        return {};
    }
    return pathIn(onto, fileNameOf(directory));
}

std::string importDirectory(const AssetBrowser& browser, const AssetDatabase& database) {
    const std::string folder = browser.shownFolder(database);
    return folder == kBuiltinFolder ? std::string() : folder;
}

std::string importAssetsCommand(std::span<const std::filesystem::path> files, std::string_view to) {
    Json paths = Json::array();
    for (const std::filesystem::path& file : files) {
        paths.push_back(pathToUtf8(file));
    }
    Json parameters = Json::object();
    parameters["files"] = std::move(paths);
    if (!to.empty()) {
        parameters["to"] = to;
    }
    return fmt::format("asset.import {}", parameters.dump());
}

std::string openSceneCommand(const AssetInfo& scene) {
    Json parameters = Json::object();
    parameters["path"] = scene.reference();
    return fmt::format("scene.open {}", parameters.dump());
}

} // namespace cordite
