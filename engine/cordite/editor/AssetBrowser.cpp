#include "cordite/editor/AssetBrowser.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/StringUtils.h"
#include "cordite/engine/SceneAssets.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// What says which row the directory of the built-in assets is. No directory of a
/// project has this path: '#' is what sets a path apart from what is inside the file.
constexpr std::string_view kBuiltinKey = "#builtin";

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

} // namespace

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

std::vector<AssetBrowserRow> AssetBrowser::allRows(const AssetDatabase& database) const {
    const std::string needle = toLowerAscii(m_filter.search);
    const auto asked = [&](const AssetInfo& asset) {
        if (m_filter.type && asset.type != *m_filter.type) {
            return false;
        }
        return needle.empty() || toLowerAscii(asset.name).find(needle) != std::string::npos ||
               toLowerAscii(asset.reference()).find(needle) != std::string::npos;
    };
    const auto assetRow = [&](const AssetInfo& asset, uint32_t depth, std::string_view label) {
        AssetBrowserRow row;
        row.kind = AssetBrowserRow::Kind::Asset;
        row.depth = depth;
        row.label = label;
        row.asset = asset.id;
        row.type = asset.type;
        row.path = asset.reference();
        row.matches = asked(asset);
        row.selected = asset.id == m_selection;
        return row;
    };

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
            AssetBrowserRow row;
            row.kind = AssetBrowserRow::Kind::Directory;
            row.depth = static_cast<uint32_t>(level);
            row.label = directories[level];
            // A directory is its path from the top, which is where the name of the last
            // of its directories ends in the path of the file.
            row.path = builtin ? std::string(kBuiltinAssetPath)
                               : std::string(file.path.substr(0, static_cast<size_t>(directories[level].data() +
                                                                                     directories[level].size() -
                                                                                     file.path.data())));
            row.key = builtin ? std::string(kBuiltinKey) : row.path;
            row.hasChildren = true;
            all.push_back(std::move(row));
        }

        auto depth = static_cast<uint32_t>(directories.size());
        if (file.asset != nullptr) {
            AssetBrowserRow row = assetRow(*file.asset, depth, fileNameOf(file.path));
            row.hasChildren = !inside.empty();
            row.key = row.hasChildren ? std::string(file.path) : std::string();
            all.push_back(std::move(row));
            ++depth;
        }
        for (const AssetInfo* asset : inside) {
            all.push_back(assetRow(*asset, depth, asset->name));
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
    // What a row is in is the rows before it that are less far in, the nearest of each
    // depth: kept while walking down to the row that is looked for.
    std::vector<AssetBrowserRow> above;
    for (AssetBrowserRow& row : allRows(database)) {
        above.resize(std::min<size_t>(above.size(), row.depth));
        if (row.kind == AssetBrowserRow::Kind::Asset && row.asset == asset) {
            for (const AssetBrowserRow& container : above) {
                setOpen(container, true);
            }
            return true;
        }
        above.push_back(std::move(row));
    }
    return false;
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

std::string dropTarget(const AssetInfo& asset, const AssetBrowserRow& row) {
    if (row.kind != AssetBrowserRow::Kind::Directory || row.key == kBuiltinKey || asset.isBuiltin() ||
        !asset.subPath.empty()) {
        return {};
    }
    const size_t slash = asset.path.rfind('/');
    if (slash != std::string::npos && std::string_view(asset.path).substr(0, slash) == row.path) {
        return {};
    }
    const std::string_view name =
        slash == std::string::npos ? std::string_view(asset.path) : std::string_view(asset.path).substr(slash + 1);
    return fmt::format("{}/{}", row.path, name);
}

std::string importDirectory(const AssetBrowser& browser, const AssetDatabase& database) {
    const AssetInfo* chosen = database.find(browser.selection());
    if (chosen == nullptr || chosen->isBuiltin()) {
        return {};
    }
    const size_t slash = chosen->path.rfind('/');
    return slash == std::string::npos ? std::string() : chosen->path.substr(0, slash);
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
