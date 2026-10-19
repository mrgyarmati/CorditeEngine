#pragma once

/// @file
/// What a person chose about the editor itself - its theme, and how the Assets panel shows
/// what a folder has in it - kept between sessions in a file of theirs, not of a project:
/// the same in every project they open.
///
/// @code
/// const std::filesystem::path file = editorPreferencesFile();   // .../Cordite/editor-preferences.json
/// CORDITE_TRY_ASSIGN(EditorPreferences preferences, loadEditorPreferences(file));
/// preferences.theme = UiTheme::Light;
/// preferences.assetView = AssetBrowserView::List;
/// CORDITE_TRY(saveEditorPreferences(file, preferences));
/// @endcode
///
/// The file is a document like the engine's others (format "cordite.preferences",
/// version 1), and is read as strictly: a member that is not known, or a theme or a view
/// that has no such name, is an error that names it.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/ui/UiTheme.h"

#include <filesystem>

namespace cordite {

struct EditorPreferences {
    /// What the interface looks like.
    UiTheme theme = UiTheme::Dark;
    /// How the Assets panel shows what a folder has in it.
    AssetBrowserView assetView = AssetBrowserView::Grid;

    bool operator==(const EditorPreferences&) const = default;
};

/// Reads preferences from a document. A member that is left out keeps its default. Fails
/// with ParseError for what is not such a document, names an unknown member, or a theme
/// or a view that has no such name, and with VersionMismatch for one of a newer editor.
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

/// Where the editor keeps the preferences of the person who runs it:
/// editor-preferences.json in a directory "Cordite" of their configuration directory
/// (userConfigurationDirectory). Empty when the system does not say where that is.
[[nodiscard]] std::filesystem::path editorPreferencesFile();

} // namespace cordite
