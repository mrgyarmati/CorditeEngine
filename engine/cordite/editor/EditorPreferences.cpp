#include "cordite/editor/EditorPreferences.h"

#include "cordite/core/Document.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"

#include <fmt/format.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace cordite {
namespace {

constexpr DocumentFormat kPreferencesFormat{
    .name = "cordite.preferences", .version = 1, .kind = "file of editor preferences"};
constexpr std::array<std::string_view, 4> kMembers = {"format", "version", "theme", "assetView"};

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
    return preferences;
}

Json editorPreferencesDocument(const EditorPreferences& preferences) {
    Json document = makeDocument(kPreferencesFormat);
    document["theme"] = std::string(nameOf(preferences.theme));
    document["assetView"] = std::string(nameOf(preferences.assetView));
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

std::filesystem::path editorPreferencesFile() {
    const std::filesystem::path directory = userConfigurationDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory / pathFromUtf8(kEngineName) / "editor-preferences.json";
}

} // namespace cordite
