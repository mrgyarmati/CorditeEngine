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
/// What the project file was called before it opened with a double click.
constexpr std::string_view kEarlierProjectFileName = "cordite.project.json";

TypeInfo describeProjectSettings() {
    return TypeBuilder<ProjectSettings>("Project")
        .property("name", &ProjectSettings::name, {.description = "What the project is called"})
        .property("startScene", &ProjectSettings::startScene,
                  {.description = "The scene the game starts with, as a path inside the asset directory"})
        .property("inputMap", &ProjectSettings::inputMap,
                  {.description = "The controls of the game - its actions and axes - as a path inside the "
                                  "asset directory"})
        .build();
}

/// Checks that a setting which names a file names one inside the asset directory, or
/// none. `example` is what such a name looks like, for whoever got it wrong.
Status validateAssetPath(std::string_view setting, std::string_view path, std::string_view example) {
    if (path.empty()) {
        return {};
    }
    // Resolved as a path next to a file at the top of the asset directory; whatever
    // leaves the directory, or is not a plain relative path, comes back empty.
    if (resolveAssetReference("project", path) != path) {
        return makeError(ErrorCode::ParseError,
                         "'{}' must be a path inside the asset directory, such as \"{}\"; got '{}'", setting, example,
                         path);
    }
    return {};
}

/// Checks the files that the settings of a project name.
Status validateSettings(const ProjectSettings& settings) {
    CORDITE_TRY(validateAssetPath("startScene", settings.startScene, "scenes/main.scene.json"));
    return validateAssetPath("inputMap", settings.inputMap, "input/controls.input.json");
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

std::filesystem::path Project::inputMapPath() const {
    return settings.inputMap.empty() ? std::filesystem::path() : assetDirectory() / pathFromUtf8(settings.inputMap);
}

Result<Project> loadProject(const std::filesystem::path& path) {
    Project project;
    // Either the directory or the file in it - also the file of before, to be told about.
    const std::string fileName = pathToUtf8(path.filename());
    project.directory = fileName == kProjectFileName || fileName == kEarlierProjectFileName ? path.parent_path() : path;
    if (!fileExists(project.file())) {
        if (fileExists(project.directory / pathFromUtf8(kEarlierProjectFileName))) {
            return makeError(ErrorCode::NotFound,
                             "{} is a project of an earlier version of Cordite: rename its {} to {}, which is "
                             "what a project file is called now",
                             pathToUtf8(project.directory), kEarlierProjectFileName, kProjectFileName);
        }
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
    if (const Status valid = validateSettings(project.settings); !valid) {
        return fail(valid.error());
    }
    return project;
}

Status saveProject(const Project& project) {
    if (project.directory.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the project has no directory to be saved in");
    }
    CORDITE_TRY(validateSettings(project.settings));
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
