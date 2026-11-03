#include "cordite/editor/AssetImport.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/FbxImporter.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/editor/AssetMove.h"

#include <fmt/format.h>

#include <algorithm>
#include <string_view>
#include <system_error>
#include <utility>

namespace cordite {
namespace {

/// A file to copy, and where it goes, as AssetInfo::reference() has it.
struct PlannedCopy {
    std::filesystem::path source;
    std::string target;
};

/// Whether two paths are one file.
bool isSameFile(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code error;
    return std::filesystem::equivalent(first, second, error) && !error;
}

/// Where a file that the glTF file going to `model` names as `uri` goes: beside it as it
/// was beside the original. Refused where that is outside the assets.
Result<std::string> besideModel(std::string_view model, std::string_view uri) {
    const size_t slash = model.rfind('/');
    const std::filesystem::path directory =
        slash == std::string_view::npos ? std::filesystem::path() : pathFromUtf8(model.substr(0, slash));
    const std::filesystem::path place = (directory / pathFromUtf8(uri)).lexically_normal();
    if (place.empty() || place.is_absolute() || place.has_root_name() || *place.begin() == "..") {
        return makeError(ErrorCode::InvalidArgument, "{} names '{}', which would land outside the assets", model, uri);
    }
    std::string reference = pathToUtf8(place);
    CORDITE_TRY(checkAssetPath(reference));
    return reference;
}

/// Adds what a model file names by where it is - a glTF file its buffers and images, an
/// OBJ file its MTL files and images - to what is copied.
Status planModelFiles(const std::filesystem::path& model, const std::string& target, std::vector<PlannedCopy>& plan) {
    const Result<GltfDocument> document = importModelFile(model, {.loadData = false});
    if (!document) {
        return makeError(ErrorCode::InvalidArgument, "cannot tell which files {} names by where they are: {}",
                         pathToUtf8(model), document.error().describe());
    }
    for (const std::string& uri : document->externalFiles) {
        const std::filesystem::path source = model.parent_path() / pathFromUtf8(uri);
        if (!fileExists(source)) {
            return makeError(ErrorCode::NotFound, "{} names '{}', which is not there", pathToUtf8(model), uri);
        }
        CORDITE_TRY_ASSIGN(std::string place, besideModel(target, uri));
        plan.push_back({.source = source, .target = std::move(place)});
    }
    return {};
}

/// What one file of the list is to be copied as: itself, and what it brings.
Status planFile(const AssetDatabase& database, const std::filesystem::path& file, std::string_view to,
                std::vector<PlannedCopy>& plan) {
    const std::string shown = pathToUtf8(file);
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return makeError(ErrorCode::NotFound, "{} is not there", shown);
    }
    if (std::filesystem::is_directory(file, error)) {
        return makeError(ErrorCode::InvalidArgument, "{} is a directory: bring in the files in it", shown);
    }
    if (const Result<std::string> inside = relativePathWithin(database.rootDirectory(), file); inside) {
        return makeError(ErrorCode::InvalidArgument, "{} is among the assets already, as {}: asset.move moves it",
                         shown, *inside);
    }
    const std::string name = pathToUtf8(file.filename());
    const std::optional<AssetType> type = assetTypeOfFile(name);
    if (!type) {
        return makeError(ErrorCode::InvalidArgument, "{} is no kind of asset that the engine knows by its ending",
                         name);
    }
    const std::string target = to.empty() ? name : fmt::format("{}/{}", to, name);
    CORDITE_TRY(checkAssetPath(target));
    plan.push_back({.source = file, .target = target});
    if (*type == AssetType::Model) {
        CORDITE_TRY(planModelFiles(file, target, plan));
    }
    return {};
}

/// Makes one copy of what goes to one place twice as one file - a texture that two models
/// name - and refuses two files for one place, and a place that holds a file already or
/// leads outside the assets.
Status checkPlaces(const std::filesystem::path& root, std::vector<PlannedCopy>& plan) {
    std::vector<PlannedCopy> unique;
    for (PlannedCopy& copy : plan) {
        const auto same = std::ranges::find(unique, copy.target, &PlannedCopy::target);
        if (same == unique.end()) {
            unique.push_back(std::move(copy));
        } else if (!isSameFile(same->source, copy.source)) {
            return makeError(ErrorCode::InvalidArgument, "{} and {} would both be {}", pathToUtf8(same->source),
                             pathToUtf8(copy.source), copy.target);
        }
    }
    for (const PlannedCopy& copy : unique) {
        const std::filesystem::path target = root / pathFromUtf8(copy.target);
        std::error_code error;
        if (std::filesystem::exists(target, error)) {
            return makeError(ErrorCode::AlreadyExists, "there is a file at {} already", copy.target);
        }
        // Also where a directory on the way is a link: what is made there must be inside.
        if (const Result<std::string> inside = relativePathWithin(root, target); !inside) {
            return makeError(ErrorCode::InvalidArgument, "'{}' leads outside the asset directory", copy.target);
        }
    }
    plan = std::move(unique);
    return {};
}

/// Copies the files. When one cannot be copied, those that were go again.
Status copyFiles(const std::filesystem::path& root, const std::vector<PlannedCopy>& plan) {
    std::vector<std::filesystem::path> copied;
    for (const PlannedCopy& copy : plan) {
        const std::filesystem::path target = root / pathFromUtf8(copy.target);
        Status done = ensureDirectory(target.parent_path());
        if (done) {
            std::error_code error;
            std::filesystem::copy_file(copy.source, target, std::filesystem::copy_options::none, error);
            if (error) {
                done = makeError(ErrorCode::IoError, "cannot copy {} to {}: {}", pathToUtf8(copy.source), copy.target,
                                 error.message());
            }
        }
        if (!done) {
            for (const std::filesystem::path& file : copied) {
                std::error_code error;
                if (!std::filesystem::remove(file, error) || error) {
                    CORDITE_LOG_WARN("Editor", "{} was copied before a later copy failed, and is still there: {}",
                                     pathToUtf8(file), error.message());
                }
            }
            return done;
        }
        copied.push_back(target);
    }
    return {};
}

} // namespace

Result<AssetImport> importAssets(EditorContext& context, const std::vector<std::filesystem::path>& files,
                                 std::string_view to) {
    AssetManager& assets = context.assets();
    const AssetDatabase& database = assets.database();
    if (database.rootDirectory().empty()) {
        return makeError(ErrorCode::InvalidState, "there is no asset directory to bring files into: open a project");
    }
    if (files.empty()) {
        return makeError(ErrorCode::InvalidArgument, "name the files to bring in");
    }
    if (!to.empty()) {
        CORDITE_TRY(checkAssetPath(to));
    }

    std::vector<PlannedCopy> plan;
    for (const std::filesystem::path& file : files) {
        CORDITE_TRY(planFile(database, file, to, plan));
    }
    CORDITE_TRY(checkPlaces(database.rootDirectory(), plan));
    CORDITE_TRY(copyFiles(database.rootDirectory(), plan));

    AssetImport imported;
    for (PlannedCopy& copy : plan) {
        imported.files.push_back(std::move(copy.target));
    }
    if (const Status refreshed = assets.refresh(); !refreshed) {
        return tl::unexpected(refreshed.error().withContext(
            fmt::format("{} files were brought in, but the assets could not be read again", imported.files.size())));
    }
    return imported;
}

} // namespace cordite
