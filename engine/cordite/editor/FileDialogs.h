#pragma once

/// @file
/// The dialogs in which a person chooses a file or a folder: the system's own - the file
/// dialog of Windows, the open and save panels of macOS, the file chooser that
/// xdg-desktop-portal shows on Linux - which the File menu of the editor shows for the
/// paths it needs, and what it asks of them.
///
/// @code
/// const std::unique_ptr<FileDialogs> dialogs = makeSystemFileDialogs();
/// const FileDialogRequest request = openSceneRequest(project.assetDirectory(), context.scenePath());
/// CORDITE_TRY_ASSIGN(const std::optional<std::filesystem::path> chosen, dialogs->ask(request, window.systemWindow()));
/// if (chosen) {
///     CORDITE_TRY(context.openScene(*chosen));
/// }
/// @endcode
///
/// A dialog of the system holds the thread that shows it until it is answered, and on
/// macOS must be shown on the main thread: the editor shows one between two frames, never
/// in the middle of one. A program without a window - a batch run, a test - has none: the
/// commands that take paths are its way. What a dialog is asked (FileDialogRequest) is
/// worked out by functions of their own, which tests hold to what each item of the menu
/// asks; tests stand in for the system with a FileDialogs of their own.

#include "cordite/core/Result.h"
#include "cordite/editor/GameExport.h"
#include "cordite/platform/Window.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// A kind of file a dialog shows: what it is called, and how the names of such files
/// end, without the dot - "scene.json", "png".
struct FileTypeFilter {
    std::string name;
    std::vector<std::string> extensions;

    [[nodiscard]] bool operator==(const FileTypeFilter&) const = default;
};

/// What a dialog asks for.
enum class FileDialogKind : uint8_t {
    /// A file that is there.
    Open,
    /// Where a file is to be made, and its name: what is there may be replaced.
    Save,
    /// A folder.
    Folder,
};

struct FileDialogRequest {
    FileDialogKind kind = FileDialogKind::Open;
    /// What the dialog says it is for, as its title.
    std::string title;
    /// The kinds of files it shows; none for every file. Of no meaning for a folder.
    std::vector<FileTypeFilter> filters;
    /// Where it opens: a directory, or nothing for where the system likes.
    std::filesystem::path directory;
    /// The name a file to save is given at first. Of no meaning but for Save.
    std::string name;

    [[nodiscard]] bool operator==(const FileDialogRequest&) const = default;
};

/// Asks a person for a path.
class FileDialogs {
public:
    FileDialogs() = default;
    virtual ~FileDialogs() = default;
    CORDITE_NON_COPYABLE(FileDialogs);

    /// Shows the dialog of `request`, over `parent` where there is one, until it is
    /// answered: the path that was chosen, or none when it was cancelled. Fails with
    /// Unavailable when the system cannot show a dialog - on Linux without
    /// xdg-desktop-portal - with what the system said. Holds the thread until then: call
    /// it between frames, on the main thread.
    [[nodiscard]] virtual Result<std::optional<std::filesystem::path>> ask(const FileDialogRequest& request,
                                                                           const SystemWindow& parent) = 0;
};

/// The dialogs of the system the program runs on (nativefiledialog-extended). Fails with
/// Unsupported, saying why, when the system has none to give.
[[nodiscard]] Result<std::unique_ptr<FileDialogs>> makeSystemFileDialogs();

/// Runs `work` where a dialog of the system can be shown from, and waits for it. On Windows
/// that is a thread of its own, in a single-threaded apartment of COM, while the messages of
/// the calling thread are dispatched: the dialogs of the shell want such an apartment, and
/// the editor's main thread is a multithreaded one once miniaudio has made its context there.
/// Asked for from that thread, a dialog over its window waits for the thread to answer, and
/// the thread waits for the dialog - the editor froze. On a thread of its own the dialog has
/// its apartment, and the window it is shown over keeps answering it. Elsewhere `work` runs
/// on the calling thread, where macOS needs its dialogs.
void runForSystemDialog(const std::function<void()>& work);

/// How the dialogs of a system tell files apart by their names.
enum class FileNameMatching : uint8_t {
    /// By how a name ends, whatever is before it: "*.scene.json" (Windows, Linux).
    ByEnding,
    /// By the last extension of a name alone, which is what macOS types a file by: a
    /// scene there is a JSON file.
    ByLastExtension,
};

/// How the dialogs of the system the program runs on tell files apart.
[[nodiscard]] FileNameMatching nativeFileNameMatching();

/// What a dialog that tells files apart by `matching` is given for `filter`: its endings,
/// separated by commas - each shortened to its last extension for ByLastExtension, and
/// each once.
[[nodiscard]] std::string filterSpec(const FileTypeFilter& filter, FileNameMatching matching);

/// What File > Open scene asks for: a scene, from the directory of the open scene's file,
/// or else from "scenes" of the assets where there is one, or else from the assets.
[[nodiscard]] FileDialogRequest openSceneRequest(const std::filesystem::path& assetDirectory,
                                                 const std::filesystem::path& scenePath);

/// What File > Save as asks for: where the scene goes, under the name of its file - or
/// `sceneName`, as a scene file, when it has none - from where Open scene opens.
[[nodiscard]] FileDialogRequest saveSceneRequest(const std::filesystem::path& assetDirectory,
                                                 const std::filesystem::path& scenePath, std::string_view sceneName);

/// What File > Open project asks for: a project file, from the directory that the open
/// project - if any - is in.
[[nodiscard]] FileDialogRequest openProjectRequest(const std::filesystem::path& projectDirectory);

/// What File > New project asks for: where the project is made, and what it is called -
/// a directory to be, whose name is the project's; from the directory that the open
/// project - if any - is in.
[[nodiscard]] FileDialogRequest newProjectRequest(const std::filesystem::path& projectDirectory);

/// What File > Export game asks for, where the game of the project in `projectDirectory`,
/// called `gameName`, goes: a folder, from where it went by default (defaultGameDirectory()),
/// for a game laid out in a directory - or, for an application bundle, which is the
/// destination itself, the bundle to make, "<gameName>.app".
[[nodiscard]] FileDialogRequest exportGameRequest(const std::filesystem::path& projectDirectory,
                                                  std::string_view gameName, GameLayout layout);

} // namespace cordite
