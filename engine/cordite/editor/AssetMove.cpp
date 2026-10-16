#include "cordite/editor/AssetMove.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/GltfImporter.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/StringUtils.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace cordite {
namespace {

/// Checks that `to` says a place inside the asset directory as references do: relative,
/// with forward slashes, without parts that leave or skip a directory, and without parts
/// that the database does not read.
Status checkMoveTarget(std::string_view to) {
    if (to.empty()) {
        return makeError(ErrorCode::InvalidArgument, "say where the file is to go");
    }
    // A colon is a drive on Windows, or a stream of a file that nobody would see.
    if (to.starts_with('/') || to.find('\\') != std::string_view::npos || to.find(':') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is no place inside the asset directory: give a relative path with forward slashes", to);
    }
    size_t begin = 0;
    while (begin <= to.size()) {
        const size_t end = std::min(to.find('/', begin), to.size());
        const std::string_view part = to.substr(begin, end - begin);
        if (part.empty() || part == "." || part == "..") {
            return makeError(ErrorCode::InvalidArgument, "'{}' has a part that is {}", to,
                             part.empty() ? std::string("empty") : fmt::format("'{}'", part));
        }
        if (part.starts_with('.')) {
            return makeError(ErrorCode::InvalidArgument,
                             "'{}' has a part that begins with a dot, '{}', and what is there is never read for assets",
                             to, part);
        }
        begin = end + 1;
    }
    return {};
}

/// The directory of a reference: what comes before its last slash; empty at the top.
std::string_view directoryOf(std::string_view reference) {
    const size_t slash = reference.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : reference.substr(0, slash);
}

/// "'a', 'b' and 'c'".
std::string quotedList(const std::vector<std::string>& names) {
    std::string list;
    for (size_t index = 0; index < names.size(); ++index) {
        if (index > 0) {
            list += index + 1 == names.size() ? " and " : ", ";
        }
        list += fmt::format("'{}'", names[index]);
    }
    return list;
}

/// Refuses a move that would break what names the file by where it is: the files a glTF
/// file names, left behind in another directory, and a glTF file that names a texture.
Status checkNamedByPath(const AssetDatabase& database, const AssetInfo& asset, std::string_view to) {
    if (asset.type == AssetType::Model && directoryOf(asset.path) != directoryOf(to)) {
        const Result<GltfDocument> model =
            importGltf(database.sourceFile(asset), {.loadData = false, .rootDirectory = database.rootDirectory()});
        if (!model) {
            return makeError(ErrorCode::InvalidArgument,
                             "cannot tell which files {} names by where they are, and would leave behind: {}",
                             asset.path, model.error().describe());
        }
        if (!model->externalFiles.empty()) {
            return makeError(ErrorCode::InvalidArgument,
                             "{} names {} by where they are, which another directory would leave behind: rename it "
                             "where it is, or move them along by hand",
                             asset.path, quotedList(model->externalFiles));
        }
    }
    if (asset.type == AssetType::Texture) {
        for (const Uuid& user : database.dependents(asset.id)) {
            const AssetInfo* material = database.find(user);
            const AssetInfo* file =
                material != nullptr && !material->subPath.empty() ? database.findByReference(material->path) : nullptr;
            if (file != nullptr && file->type == AssetType::Model) {
                return makeError(ErrorCode::InvalidArgument, "{} names {} by where it is, and would lose it",
                                 file->path, asset.path);
            }
        }
    }
    return {};
}

/// Whether two paths are one file - also when they differ in case, on a file system
/// that does not tell.
bool isSameFile(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code error;
    return !first.empty() && std::filesystem::equivalent(first, second, error) && !error;
}

/// Refuses a place that holds another file, or another .meta.
Status checkPlaceIsFree(const std::filesystem::path& source, const std::filesystem::path& target, std::string_view to) {
    const std::array<std::pair<std::filesystem::path, std::filesystem::path>, 2> places = {
        std::pair(source, target), std::pair(assetMetaPath(source), assetMetaPath(target))};
    for (const auto& [from, place] : places) {
        std::error_code error;
        if (std::filesystem::exists(place, error) && !isSameFile(from, place)) {
            return makeError(ErrorCode::AlreadyExists, "there is a file at {} already",
                             place == target ? std::string(to) : fmt::format("{}.meta", to));
        }
    }
    return {};
}

/// Moves the file and what keeps its id. When the second cannot follow, the first goes
/// back, and the failure says whether that worked.
Status moveFiles(const std::filesystem::path& source, const std::filesystem::path& target, const Uuid& id,
                 std::string_view from, std::string_view to) {
    CORDITE_TRY(ensureDirectory(target.parent_path()));
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot move {} to {}: {}", from, to, error.message());
    }

    // The id goes along: in the .meta it has, or in one written now - without one, the id
    // of the file would be made of its new path.
    Status kept;
    if (const std::filesystem::path meta = assetMetaPath(source); fileExists(meta)) {
        std::filesystem::rename(meta, assetMetaPath(target), error);
        if (error) {
            kept = makeError(ErrorCode::IoError, "cannot move {}.meta to {}.meta: {}", from, to, error.message());
        }
    } else {
        kept = writeAssetMeta(target, id);
    }
    if (kept) {
        return {};
    }
    std::filesystem::rename(target, source, error);
    if (error) {
        return tl::unexpected(kept.error().withContext(
            fmt::format("{} is at {} without its id, and could not be moved back: {}", from, to, error.message())));
    }
    return kept;
}

/// Whether `text` holds a path at `position`, `length` long, as a whole and not as a part of
/// a longer one.
bool isWholePath(std::string_view text, size_t position, size_t length) {
    const auto partOfPath = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-' ||
               character == '.' || character == '/';
    };
    const size_t end = position + length;
    return (position == 0 || !partOfPath(text[position - 1])) && (end == text.size() || !partOfPath(text[end]));
}

/// The lines of the scripts, scenes and prefabs of the project that name `reference`.
std::vector<FileMention> findFileMentions(const AssetDatabase& database, std::string_view reference) {
    std::vector<FileMention> mentions;
    for (const AssetInfo& asset : database.assets()) {
        const bool isText =
            asset.type == AssetType::Script || asset.type == AssetType::Scene || asset.type == AssetType::Prefab;
        if (!isText || !asset.subPath.empty()) {
            continue;
        }
        const Result<std::string> text = readTextFile(database.sourceFile(asset));
        if (!text) {
            CORDITE_LOG_WARN("Editor", "{} could not be read to find what still names {}: {}", asset.path, reference,
                             text.error().describe());
            continue;
        }
        uint32_t lineNumber = 0;
        size_t begin = 0;
        while (begin <= text->size()) {
            const size_t end = std::min(text->find('\n', begin), text->size());
            const std::string_view line = std::string_view(*text).substr(begin, end - begin);
            ++lineNumber;
            for (size_t found = line.find(reference); found != std::string_view::npos;
                 found = line.find(reference, found + 1)) {
                if (isWholePath(line, found, reference.size())) {
                    mentions.push_back({.file = asset.path, .line = lineNumber, .text = std::string(trim(line))});
                    break;
                }
            }
            begin = end + 1;
        }
    }
    return mentions;
}

/// The values in the components of the open scene that are `reference`, whole.
std::vector<EntityMention> findEntityMentions(const EditorContext& context, std::string_view reference) {
    std::vector<EntityMention> mentions;
    const World& world = context.world();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        for (const ComponentDescriptor* descriptor : context.components().components()) {
            const void* instance = descriptor->getConst(world.registry(), entity.handle);
            if (instance == nullptr) {
                continue;
            }
            // flatten() follows a value as deep as it goes; these are the component's own,
            // whose free-form data was checked for its depth on the way in (checkJsonData).
            const Json flat = descriptor->type.toJson(instance).flatten();
            for (const auto& value : flat.items()) {
                if (value.value().is_string() && value.value().get_ref<const std::string&>() == reference) {
                    mentions.push_back(
                        {.entity = entity, .component = descriptor->type.name(), .property = value.key()});
                }
            }
        }
    });
    return mentions;
}

} // namespace

Result<AssetMove> moveAsset(EditorContext& context, Uuid id, std::string to) {
    AssetManager& assets = context.assets();
    const AssetDatabase& database = assets.database();
    const AssetInfo* asset = database.find(id);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset {}", id.toString());
    }
    if (asset->isBuiltin()) {
        return makeError(ErrorCode::InvalidArgument, "{} is built into the engine, and has no file to move",
                         asset->name);
    }
    if (!asset->subPath.empty()) {
        return makeError(ErrorCode::InvalidArgument, "{} is inside {}: move that file", asset->reference(),
                         asset->path);
    }
    CORDITE_TRY(checkMoveTarget(to));
    // What the asset is, kept: reading the assets again leaves `asset` pointing nowhere.
    const std::string from = asset->path;
    if (to == from) {
        return makeError(ErrorCode::InvalidArgument, "{} is where it is to go", from);
    }
    if (assetFileExtension(to) != assetFileExtension(from)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' does not end as {} does, with '{}': it would be another kind of file, or none", to, from,
                         assetFileExtension(from));
    }
    CORDITE_TRY(checkNamedByPath(database, *asset, to));

    const std::filesystem::path root = database.rootDirectory();
    const std::filesystem::path source = root / pathFromUtf8(from);
    const std::filesystem::path target = root / pathFromUtf8(to);
    // Also where a directory on the way is a link: what is made there must be inside.
    if (const Result<std::string> inside = relativePathWithin(root, target); !inside) {
        return makeError(ErrorCode::InvalidArgument, "'{}' leads outside the asset directory", to);
    }
    CORDITE_TRY(checkPlaceIsFree(source, target, to));

    const bool sceneFollows = isSameFile(context.scenePath(), source);
    CORDITE_TRY(moveFiles(source, target, id, from, to));
    if (sceneFollows) {
        context.setScenePath(target);
    }
    if (const Status refreshed = assets.refresh(); !refreshed) {
        return tl::unexpected(refreshed.error().withContext(
            fmt::format("{} was moved to {}, but the assets could not be read again", from, to)));
    }
    if (const AssetInfo* moved = database.find(id);
        moved == nullptr || !isSameFile(database.sourceFile(*moved), target)) {
        return makeError(ErrorCode::InvalidState, "{} was moved to {}, but the assets do not find {} there", from, to,
                         id.toString());
    }

    AssetMove moved{.asset = id, .from = from, .to = std::move(to)};
    moved.files = findFileMentions(database, from);
    moved.entities = findEntityMentions(context, from);
    return moved;
}

Json describeAssetMove(const EditorContext& context, const AssetMove& move) {
    Json mentions = Json::array();
    for (const FileMention& mention : move.files) {
        Json entry = Json::object();
        entry["file"] = mention.file;
        entry["line"] = mention.line;
        entry["text"] = mention.text;
        mentions.push_back(std::move(entry));
    }
    for (const EntityMention& mention : move.entities) {
        Json entry = Json::object();
        entry["entity"] = context.entityPath(mention.entity);
        entry["id"] = context.world().id(mention.entity).toString();
        entry["component"] = mention.component;
        entry["property"] = mention.property;
        mentions.push_back(std::move(entry));
    }
    Json result = Json::object();
    result["id"] = move.asset.toString();
    result["from"] = move.from;
    result["to"] = move.to;
    result["mentions"] = std::move(mentions);
    return result;
}

} // namespace cordite
