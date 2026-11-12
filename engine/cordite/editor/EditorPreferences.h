#pragma once

/// @file
/// What a person chose about the editor itself - its theme, and how the Assets panel shows
/// what a folder has in it - and the projects they opened last, kept between sessions in a
/// file of theirs, not of a project: the same in every project they open.
///
/// @code
/// const std::filesystem::path file = editorPreferencesFile();   // .../Cordite/editor-preferences.json
/// CORDITE_TRY_ASSIGN(EditorPreferences preferences, loadEditorPreferences(file));
/// preferences.theme = UiTheme::Light;
/// preferences.assetView = AssetBrowserView::List;
/// rememberRecentProject(preferences, pathFromUtf8("D:/games/depot"));   // first of File > Open recent
/// CORDITE_TRY(saveEditorPreferences(file, preferences));
/// @endcode
///
/// The file is a document like the engine's others (format "cordite.preferences",
/// version 1), and is read as strictly: a member that is not known, a theme or a view that
/// has no such name, or a recent project that is no absolute path or one too many, is an
/// error that names it.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/ui/UiTheme.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace cordite {

/// How many projects File > Open recent lists, at the most.
inline constexpr size_t kMaxRecentProjects = 10;

struct EditorPreferences {
    /// What the interface looks like.
    UiTheme theme = UiTheme::Dark;
    /// How the Assets panel shows what a folder has in it.
    AssetBrowserView assetView = AssetBrowserView::Grid;
    /// The directories of the projects opened last, the newest first: absolute, each once,
    /// at most kMaxRecentProjects. Kept by rememberRecentProject().
    std::vector<std::filesystem::path> recentProjects;

    bool operator==(const EditorPreferences&) const = default;
};

/// Reads preferences from a document. A member that is left out keeps its default. Fails
/// with ParseError for what is not such a document, names an unknown member, a theme or a
/// view that has no such name, or recent projects that are not a list of at most
/// kMaxRecentProjects absolute paths, and with VersionMismatch for one of a newer editor.
[[nodiscard]] Result<EditorPreferences> parseEditorPreferences(const Json& document);

/// The document of preferences, with every member in it.
[[nodiscard]] Json editorPreferencesDocument(const EditorPreferences& preferences);

/// Reads the preferences of a file. A file that is not there gives the defaults: nothing
/// was chosen yet. Fails like parseEditorPreferences, and with the error of the file
/// system for a file that cannot be read.
[[nodiscard]] Result<EditorPreferences> loadEditorPreferences(const std::filesystem::path& file);

/// Writes the preferences to a file, and the directories it is in, replacing what was
/// there whole (writeJsonFile).
[[nodiscard]] Status saveEditorPreferences(const std::filesystem::path& file, const EditorPreferences& preferences);

/// Puts the project in `directory` first among the recent projects - as an absolute path, so
/// that it is the same project wherever the editor runs from - taking out an entry that names
/// the same directory, and the oldest when there are more than kMaxRecentProjects.
void rememberRecentProject(EditorPreferences& preferences, const std::filesystem::path& directory);

/// Whether two paths name the same directory of a project, as the list of recent projects
/// judges it: absolute, and as the file system has what is there of them.
[[nodiscard]] bool sameProjectDirectory(const std::filesystem::path& first, const std::filesystem::path& second);

/// Whether a recent project is still there: whether its directory has a project file.
[[nodiscard]] bool recentProjectExists(const std::filesystem::path& directory);

/// Takes the recent projects that are no longer there out of the list, the others in their
/// order. Returns how many were taken out.
size_t forgetMissingRecentProjects(EditorPreferences& preferences);

/// Where the editor keeps the preferences of the person who runs it:
/// editor-preferences.json in a directory "Cordite" of their configuration directory
/// (userConfigurationDirectory). Empty when the system does not say where that is.
[[nodiscard]] std::filesystem::path editorPreferencesFile();

} // namespace cordite
