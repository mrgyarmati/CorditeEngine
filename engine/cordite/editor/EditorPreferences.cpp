#include "cordite/editor/EditorPreferences.h"

#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/engine/Project.h"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cordite {
namespace {

constexpr DocumentFormat kPreferencesFormat{
    .name = "cordite.preferences", .version = 1, .kind = "file of editor preferences"};
constexpr std::array<std::string_view, 5> kMembers = {"format", "version", "theme", "assetView", "recentProjects"};

/// The names of the themes, for a message that says which there are.
std::string themeNames() {
    std::string names;
    for (const UiTheme theme : kUiThemes) {
        if (!names.empty()) {
            names += ", ";
        }
        names += nameOf(theme);
    }
    return names;
}

Result<UiTheme> parseTheme(const Json& value) {
    if (!value.is_string()) {
        return makeError(ErrorCode::ParseError, "'theme' must be the name of a theme ({}), got {}", themeNames(),
                         dumpJson(value));
    }
    const std::string name = value.get<std::string>();
    const std::optional<UiTheme> theme = uiThemeFromName(name);
    if (!theme) {
        return makeError(ErrorCode::ParseError, "there is no theme '{}'; there are {}", name, themeNames());
    }
    return *theme;
}

Result<AssetBrowserView> parseAssetView(const Json& value) {
    const auto views = fmt::format("{}, {}", nameOf(AssetBrowserView::List), nameOf(AssetBrowserView::Grid));
    if (!value.is_string()) {
        return makeError(ErrorCode::ParseError, "'assetView' must be the name of a view ({}), got {}", views,
                         dumpJson(value));
    }
    const std::string name = value.get<std::string>();
    const std::optional<AssetBrowserView> view = assetBrowserViewFromName(name);
    if (!view) {
        return makeError(ErrorCode::ParseError, "there is no view of the assets '{}'; there are {}", name, views);
    }
    return *view;
}

Result<std::vector<std::filesystem::path>> parseRecentProjects(const Json& value) {
    if (!value.is_array()) {
        return makeError(ErrorCode::ParseError,
                         "'recentProjects' must be a list of the directories of projects, got {}", dumpJson(value));
    }
    if (value.size() > kMaxRecentProjects) {
        return makeError(ErrorCode::ParseError, "'recentProjects' lists {} projects; at most {} are kept", value.size(),
                         kMaxRecentProjects);
    }
    std::vector<std::filesystem::path> projects;
    projects.reserve(value.size());
    for (const Json& entry : value) {
        if (!entry.is_string() || entry.get<std::string>().empty()) {
            return makeError(ErrorCode::ParseError,
                             "an entry of 'recentProjects' must be the directory of a project, got {}",
                             dumpJson(entry));
        }
        std::filesystem::path directory = pathFromUtf8(entry.get<std::string>());
        if (!directory.is_absolute()) {
            return makeError(ErrorCode::ParseError, "the recent project '{}' is not an absolute path",
                             entry.get<std::string>());
        }
        projects.push_back(std::move(directory));
    }
    return projects;
}

/// The directory as the list keeps it: absolute, with what is there of it as the file
/// system has it - so that one project reached two ways is one entry.
std::filesystem::path recentProjectKey(const std::filesystem::path& directory) {
    // Absolute first: of a path that is not there, weakly_canonical keeps what it was given.
    std::error_code error;
    std::filesystem::path key = std::filesystem::absolute(directory, error);
    if (error) {
        key = directory;
    }
    if (std::filesystem::path canonical = std::filesystem::weakly_canonical(key, error); !error) {
        key = std::move(canonical);
    }
    key = key.lexically_normal();
    // "game/assets/.." is "game/" to lexically_normal, and a directory is the same without it.
    if (key.has_relative_path() && key.filename().empty()) {
        key = key.parent_path();
    }
    return key;
}

} // namespace

Result<EditorPreferences> parseEditorPreferences(const Json& document) {
    CORDITE_TRY(readDocumentHeader(document, kPreferencesFormat));
    CORDITE_TRY(rejectUnknownMembers(document, kMembers));

    EditorPreferences preferences;
    if (const auto theme = document.find("theme"); theme != document.end()) {
        CORDITE_TRY_ASSIGN(preferences.theme, parseTheme(*theme));
    }
    if (const auto view = document.find("assetView"); view != document.end()) {
        CORDITE_TRY_ASSIGN(preferences.assetView, parseAssetView(*view));
    }
    if (const auto recent = document.find("recentProjects"); recent != document.end()) {
        CORDITE_TRY_ASSIGN(preferences.recentProjects, parseRecentProjects(*recent));
    }
    return preferences;
}

Json editorPreferencesDocument(const EditorPreferences& preferences) {
    Json document = makeDocument(kPreferencesFormat);
    document["theme"] = std::string(nameOf(preferences.theme));
    document["assetView"] = std::string(nameOf(preferences.assetView));
    Json recent = Json::array();
    for (const std::filesystem::path& directory : preferences.recentProjects) {
        recent.push_back(pathToUtf8(directory));
    }
    document["recentProjects"] = std::move(recent);
    return document;
}

Result<EditorPreferences> loadEditorPreferences(const std::filesystem::path& file) {
    if (!fileExists(file)) {
        return EditorPreferences{};
    }
    CORDITE_TRY_ASSIGN(const Json document, readJsonFile(file));
    Result<EditorPreferences> preferences = parseEditorPreferences(document);
    if (!preferences) {
        return tl::unexpected(preferences.error().withContext(pathToUtf8(file)));
    }
    return preferences;
}

Status saveEditorPreferences(const std::filesystem::path& file, const EditorPreferences& preferences) {
    return writeJsonFile(file, editorPreferencesDocument(preferences));
}

void rememberRecentProject(EditorPreferences& preferences, const std::filesystem::path& directory) {
    const std::filesystem::path key = recentProjectKey(directory);
    std::erase_if(preferences.recentProjects,
                  [&key](const std::filesystem::path& entry) { return recentProjectKey(entry) == key; });
    preferences.recentProjects.insert(preferences.recentProjects.begin(), key);
    if (preferences.recentProjects.size() > kMaxRecentProjects) {
        preferences.recentProjects.resize(kMaxRecentProjects);
    }
}

bool sameProjectDirectory(const std::filesystem::path& first, const std::filesystem::path& second) {
    return recentProjectKey(first) == recentProjectKey(second);
}

bool recentProjectExists(const std::filesystem::path& directory) {
    return fileExists(directory / pathFromUtf8(kProjectFileName));
}

size_t forgetMissingRecentProjects(EditorPreferences& preferences) {
    return std::erase_if(preferences.recentProjects,
                         [](const std::filesystem::path& entry) { return !recentProjectExists(entry); });
}

std::filesystem::path editorPreferencesFile() {
    const std::filesystem::path directory = userConfigurationDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory / pathFromUtf8(kEngineName) / "editor-preferences.json";
}

} // namespace cordite
