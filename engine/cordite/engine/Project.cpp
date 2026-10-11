#include "cordite/engine/Project.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include <algorithm>
#include <array>
#include <utility>

namespace cordite {
namespace {

constexpr DocumentFormat kProjectFormat{.name = "cordite.project", .version = 1, .kind = "project"};
constexpr std::array<std::string_view, 2> kHeaderMembers = {"format", "version"};

TypeInfo describeProjectSettings() {
    return TypeBuilder<ProjectSettings>("Project")
        .property("name", &ProjectSettings::name, {.description = "What the project is called"})
        .property("startScene", &ProjectSettings::startScene,
                  {.description = "The scene the game starts with, as a path inside the asset directory"})
        .build();
}

/// Checks that a start scene names a file inside the asset directory.
Status validateStartScene(std::string_view startScene) {
    if (startScene.empty()) {
        return {};
    }
    // Resolved as a path next to a file at the top of the asset directory; whatever
    // leaves the directory, or is not a plain relative path, comes back empty.
    if (resolveAssetReference("project", startScene) != startScene) {
        return makeError(ErrorCode::ParseError,
                         "'startScene' must be a path inside the asset directory, such as \"scenes/main.scene.json\"; "
                         "got '{}'",
                         startScene);
    }
    return {};
}

Json serializeProject(const ProjectSettings& settings) {
    Json document = makeDocument(kProjectFormat);
    document.update(projectSettingsTypeInfo().toJson(&settings));
    return document;
}

} // namespace

const TypeInfo& projectSettingsTypeInfo() {
    static const TypeInfo kType = describeProjectSettings();
    return kType;
}

std::filesystem::path Project::file() const {
    return directory / pathFromUtf8(kProjectFileName);
}

std::filesystem::path Project::assetDirectory() const {
    return directory / "assets";
}

std::filesystem::path Project::cacheDirectory() const {
    return directory / ".cordite";
}

std::filesystem::path Project::startScenePath() const {
    return settings.startScene.empty() ? std::filesystem::path() : assetDirectory() / pathFromUtf8(settings.startScene);
}

Result<Project> loadProject(const std::filesystem::path& path) {
    Project project;
    // Either the directory or the file in it.
    project.directory = pathToUtf8(path.filename()) == kProjectFileName ? path.parent_path() : path;
    if (!fileExists(project.file())) {
        return makeError(ErrorCode::NotFound, "{} is not a project: it has no {}", pathToUtf8(project.directory),
                         kProjectFileName);
    }

    const auto fail = [&project](const Error& error) {
        return tl::unexpected(error.withContext(pathToUtf8(project.file())));
    };
    const Result<Json> document = readJsonFile(project.file());
    if (!document) {
        return tl::unexpected(document.error());
    }
    if (const Result<uint32_t> version = readDocumentHeader(*document, kProjectFormat); !version) {
        return fail(version.error());
    }

    Json settings = Json::object();
    for (const auto& [key, value] : document->items()) {
        if (std::ranges::find(kHeaderMembers, std::string_view(key)) == kHeaderMembers.end()) {
            settings[key] = value;
        }
    }
    if (const Status parsed = projectSettingsTypeInfo().fromJson(&project.settings, settings); !parsed) {
        return fail(parsed.error());
    }
    if (const Status valid = validateStartScene(project.settings.startScene); !valid) {
        return fail(valid.error());
    }
    return project;
}

Status saveProject(const Project& project) {
    if (project.directory.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the project has no directory to be saved in");
    }
    CORDITE_TRY(validateStartScene(project.settings.startScene));
    CORDITE_TRY(ensureDirectory(project.assetDirectory()));
    return writeJsonFile(project.file(), serializeProject(project.settings));
}

Result<Project> createProject(const std::filesystem::path& directory, std::string name) {
    Project project;
    project.directory = directory;
    project.settings.name = std::move(name);
    if (fileExists(project.file())) {
        return makeError(ErrorCode::AlreadyExists, "{} already holds a project", pathToUtf8(directory));
    }
    CORDITE_TRY(saveProject(project));
    return project;
}

} // namespace cordite
