#include "cordite/editor/AssetFiles.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Material.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/editor/AssetMove.h"
#include "cordite/engine/SceneAssets.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace cordite {
namespace {

constexpr std::string_view kMaterialEnding = ".material.json";
/// How many names freeAssetPath() tries before it gives the last one up.
constexpr int kMostNumberedNames = 1000;

/// The asset directory, or why there is none to change.
Result<std::filesystem::path> assetRoot(const EditorContext& context) {
    const std::filesystem::path& root = context.assets().database().rootDirectory();
    if (root.empty()) {
        return makeError(ErrorCode::InvalidState, "there is no asset directory: open a project");
    }
    return root;
}

/// Where `path` of the assets is on the disk, after checking that it names a place inside
/// the asset directory - also where a directory on the way is a link.
Result<std::filesystem::path> placeOf(const std::filesystem::path& root, std::string_view path) {
    CORDITE_TRY(checkAssetPath(path));
    std::filesystem::path place = root / pathFromUtf8(path);
    if (const Result<std::string> inside = relativePathWithin(root, place); !inside) {
        return makeError(ErrorCode::InvalidArgument, "'{}' leads outside the asset directory", path);
    }
    std::error_code error;
    if (std::filesystem::exists(place, error)) {
        return makeError(ErrorCode::AlreadyExists, "there is {} at {} already",
                         std::filesystem::is_directory(place, error) ? "a directory" : "a file", path);
    }
    return place;
}

/// What a message calls the users of an asset.
std::string describeUsers(const AssetDatabase& database, const AssetUsers& users) {
    std::string listed;
    for (const Uuid& user : users.assets) {
        const AssetInfo* asset = database.find(user);
        listed +=
            fmt::format("{}{}", listed.empty() ? "" : ", ", asset != nullptr ? asset->reference() : user.toString());
    }
    if (users.sceneReferences > 0) {
        listed += fmt::format("{}{} propert{} of the open scene", listed.empty() ? "" : ", ", users.sceneReferences,
                              users.sceneReferences == 1 ? "y" : "ies");
    }
    return listed;
}

} // namespace

Result<AssetUsers> findAssetUsers(const EditorContext& context, const Uuid& id) {
    const AssetDatabase& database = context.assets().database();
    const AssetInfo* asset = database.find(id);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset {}", id.toString());
    }
    // The asset, and everything else that is in its file: a model goes whole.
    std::unordered_set<Uuid> inFile;
    for (const AssetInfo& other : database.assets()) {
        if (other.path == asset->path) {
            inFile.insert(other.id);
        }
    }
    AssetUsers users;
    std::unordered_set<Uuid> listed;
    for (const AssetInfo& other : database.assets()) {
        if (!inFile.contains(other.id)) {
            continue;
        }
        for (const Uuid& user : database.dependents(other.id)) {
            if (!inFile.contains(user) && listed.insert(user).second) {
                users.assets.push_back(user);
            }
        }
    }
    // dependents() keeps the order of assets() for each; across the file's assets, sort by it.
    std::ranges::sort(users.assets, {}, [&database](const Uuid& user) {
        const AssetInfo* found = database.find(user);
        return found != nullptr ? static_cast<size_t>(found - database.assets().data()) : database.assets().size();
    });
    for (const AssetReferenceUse& use : collectAssetReferences(context.world(), context.components())) {
        users.sceneReferences += inFile.contains(use.asset) ? 1 : 0;
    }
    return users;
}

Result<AssetDeletion> deleteAsset(EditorContext& context, const Uuid& id, bool force) {
    if (context.isPlaying()) {
        return makeError(ErrorCode::InvalidState, "the scene is being played: stop it before deleting an asset");
    }
    CORDITE_TRY_ASSIGN(const std::filesystem::path root, assetRoot(context));
    const AssetDatabase& database = context.assets().database();
    const AssetInfo* asset = database.find(id);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset {}", id.toString());
    }
    if (asset->isBuiltin()) {
        return makeError(ErrorCode::InvalidArgument, "{} is built into the engine, and has no file to delete",
                         asset->reference());
    }
    if (!asset->subPath.empty()) {
        return makeError(ErrorCode::InvalidArgument, "{} is inside the file {}: delete the file", asset->reference(),
                         asset->path);
    }
    const std::filesystem::path file = root / pathFromUtf8(asset->path);
    std::error_code error;
    if (!context.scenePath().empty() && std::filesystem::equivalent(file, context.scenePath(), error)) {
        return makeError(ErrorCode::InvalidState, "{} is the scene that is open: open another first", asset->path);
    }
    CORDITE_TRY_ASSIGN(AssetUsers users, findAssetUsers(context, id));
    if (!users.empty() && !force) {
        return makeError(ErrorCode::InvalidState,
                         "{} is used by {}: delete it anyway with force, or change those first", asset->path,
                         describeUsers(database, users));
    }

    AssetDeletion deletion{.path = asset->path, .users = std::move(users)};
    if (!std::filesystem::remove(file, error) || error) {
        return makeError(ErrorCode::IoError, "cannot delete {}: {}", deletion.path,
                         error ? error.message() : std::string("it is not there"));
    }
    // The file is gone; a .meta left behind would only be said to be an orphan.
    const std::filesystem::path meta = assetMetaPath(file);
    if (std::filesystem::remove(meta, error); error) {
        CORDITE_LOG_WARN("Editor", "{} is deleted, but its .meta is still there: {}", deletion.path, error.message());
    }
    if (const Status refreshed = context.assets().refresh(); !refreshed) {
        return tl::unexpected(refreshed.error().withContext(
            fmt::format("{} was deleted, but the assets could not be read again", deletion.path)));
    }
    return deletion;
}

Result<std::string> createAssetDirectory(EditorContext& context, std::string_view path) {
    CORDITE_TRY_ASSIGN(const std::filesystem::path root, assetRoot(context));
    CORDITE_TRY_ASSIGN(const std::filesystem::path place, placeOf(root, path));
    CORDITE_TRY(ensureDirectory(place));
    if (const Status refreshed = context.assets().refresh(); !refreshed) {
        return tl::unexpected(
            refreshed.error().withContext(fmt::format("{} was made, but the assets could not be read again", path)));
    }
    return std::string(path);
}

Result<Uuid> createMaterial(EditorContext& context, std::string_view path) {
    CORDITE_TRY_ASSIGN(const std::filesystem::path root, assetRoot(context));
    if (!path.ends_with(kMaterialEnding) || path.size() == kMaterialEnding.size() ||
        path.ends_with("/.material.json")) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is no material: its name ends in {}", path, kMaterialEnding);
    }
    CORDITE_TRY_ASSIGN(const std::filesystem::path place, placeOf(root, path));
    CORDITE_TRY(ensureDirectory(place.parent_path()));
    CORDITE_TRY(writeJsonFile(place, serializeMaterial(MaterialData{})));
    if (const Status refreshed = context.assets().refresh(); !refreshed) {
        return tl::unexpected(
            refreshed.error().withContext(fmt::format("{} was written, but the assets could not be read again", path)));
    }
    const AssetInfo* made = context.assets().database().findByReference(path);
    if (made == nullptr) {
        return makeError(ErrorCode::InvalidState, "{} was written, but is not among the assets", path);
    }
    return made->id;
}

std::string freeAssetPath(const EditorContext& context, std::string_view directory, std::string_view base,
                          std::string_view ending) {
    const std::filesystem::path& root = context.assets().database().rootDirectory();
    const auto pathFor = [&](int number) {
        const std::string name =
            number == 1 ? fmt::format("{}{}", base, ending) : fmt::format("{} {}{}", base, number, ending);
        return directory.empty() ? name : fmt::format("{}/{}", directory, name);
    };
    for (int number = 1; number < kMostNumberedNames; ++number) {
        const std::string path = pathFor(number);
        std::error_code error;
        if (!std::filesystem::exists(root / pathFromUtf8(path), error) && !error) {
            return path;
        }
    }
    return pathFor(kMostNumberedNames);
}

} // namespace cordite
