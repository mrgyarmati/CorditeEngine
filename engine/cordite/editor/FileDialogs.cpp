#include "cordite/editor/FileDialogs.h"

#include "cordite/core/Base.h"
#include "cordite/core/FileSystem.h"

#include <fmt/format.h>
#include <nfd.h>

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <utility>

namespace cordite {
namespace {

/// What a scene file is to a dialog.
FileTypeFilter sceneFilter() {
    return {.name = "Scene", .extensions = {"scene.json"}};
}

/// `path` if it is a directory, or else the nearest directory above it that there is;
/// nothing when there is none. A dialog told to open where nothing is opens nowhere on
/// some systems, and on Windows does not open at all when more than the last name is
/// missing.
std::filesystem::path existingDirectory(const std::filesystem::path& path) {
    std::error_code error;
    for (std::filesystem::path directory = path; !directory.empty(); directory = directory.parent_path()) {
        if (std::filesystem::is_directory(directory, error)) {
            return directory;
        }
        if (directory == directory.parent_path()) {
            break;
        }
    }
    return {};
}

/// Where a scene is opened from and saved to, unless the person goes elsewhere: the
/// directory of the open scene's file, or else "scenes" of the assets where there is
/// one, or else the assets.
std::filesystem::path sceneDirectory(const std::filesystem::path& assetDirectory,
                                     const std::filesystem::path& scenePath) {
    if (!scenePath.empty()) {
        if (std::filesystem::path directory = existingDirectory(scenePath.parent_path()); !directory.empty()) {
            return directory;
        }
    }
    if (assetDirectory.empty()) {
        return {};
    }
    std::error_code error;
    if (std::filesystem::path scenes = assetDirectory / "scenes"; std::filesystem::is_directory(scenes, error)) {
        return scenes;
    }
    return existingDirectory(assetDirectory);
}

/// Where a game of a project goes unless the person says otherwise: beside the project,
/// named after its directory - "showcase game" beside "showcase".
std::filesystem::path defaultGameDirectory(const std::filesystem::path& project) {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::weakly_canonical(project, error);
    if (error || directory.filename().empty()) {
        directory = project;
    }
    return directory.parent_path() / pathFromUtf8(pathToUtf8(directory.filename()) + " game");
}

/// A name a file can have on every system: what would be read as a directory, a drive or
/// a pattern becomes a dash.
std::string fileSafeName(std::string_view name) {
    std::string safe(name);
    std::ranges::replace_if(
        safe, [](char character) { return std::string_view(R"(/\:*?"<>|)").find(character) != std::string_view::npos; },
        '-');
    return safe;
}

/// The dialogs of nativefiledialog-extended, which it is initialized for while there are.
class SystemFileDialogs final : public FileDialogs {
public:
    SystemFileDialogs() = default;

    ~SystemFileDialogs() override { NFD_Quit(); }

    CORDITE_NON_MOVABLE(SystemFileDialogs);

    Result<std::optional<std::filesystem::path>> ask(const FileDialogRequest& request,
                                                     const SystemWindow& parent) override {
        // Everything the dialog is given lives until it is answered.
        const FileNameMatching matching = nativeFileNameMatching();
        std::vector<std::string> specs;
        specs.reserve(request.filters.size());
        for (const FileTypeFilter& filter : request.filters) {
            specs.push_back(filterSpec(filter, matching));
        }
        std::vector<nfdu8filteritem_t> filters;
        filters.reserve(request.filters.size());
        for (size_t index = 0; index < request.filters.size(); ++index) {
            filters.push_back({.name = request.filters[index].name.c_str(), .spec = specs[index].c_str()});
        }
        const std::string directory = pathToUtf8(request.directory);
        const char* defaultPath = directory.empty() ? nullptr : directory.c_str();
        nfdwindowhandle_t window{};
        if (parent.kind == SystemWindow::Kind::Windows) {
            window = {.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS, .handle = parent.handle};
        } else if (parent.kind == SystemWindow::Kind::Cocoa) {
            window = {.type = NFD_WINDOW_HANDLE_TYPE_COCOA, .handle = parent.handle};
        }

        nfdu8char_t* chosen = nullptr;
        nfdresult_t result = NFD_ERROR;
        switch (request.kind) {
            case FileDialogKind::Open: {
                nfdopendialogu8args_t arguments{};
                arguments.filterList = filters.empty() ? nullptr : filters.data();
                arguments.filterCount = static_cast<nfdfiltersize_t>(filters.size());
                arguments.defaultPath = defaultPath;
                arguments.parentWindow = window;
                arguments.title = request.title.c_str();
                result = NFD_OpenDialogU8_With(&chosen, &arguments);
                break;
            }
            case FileDialogKind::Save: {
                nfdsavedialogu8args_t arguments{};
                arguments.filterList = filters.empty() ? nullptr : filters.data();
                arguments.filterCount = static_cast<nfdfiltersize_t>(filters.size());
                arguments.defaultPath = defaultPath;
                arguments.defaultName = request.name.empty() ? nullptr : request.name.c_str();
                arguments.parentWindow = window;
                arguments.title = request.title.c_str();
                result = NFD_SaveDialogU8_With(&chosen, &arguments);
                break;
            }
            case FileDialogKind::Folder: {
                nfdpickfolderu8args_t arguments{};
                arguments.defaultPath = defaultPath;
                arguments.parentWindow = window;
                arguments.title = request.title.c_str();
                result = NFD_PickFolderU8_With(&chosen, &arguments);
                break;
            }
        }

        if (result == NFD_CANCEL) {
            return std::optional<std::filesystem::path>();
        }
        if (result != NFD_OKAY || chosen == nullptr) {
            return makeError(ErrorCode::Unsupported, "the system's dialog '{}' could not be shown: {}", request.title,
                             takeError());
        }
        std::filesystem::path path = pathFromUtf8(chosen);
        NFD_FreePathU8(chosen);
        return std::optional<std::filesystem::path>(std::move(path));
    }

    /// What nativefiledialog-extended said went wrong last, and forgets it.
    static std::string takeError() {
        // Copied before it is cleared, which frees what it points to.
        std::string error = "it did not say why";
        if (const char* said = NFD_GetError(); said != nullptr) {
            error = said;
        }
        NFD_ClearError();
        return error;
    }
};

} // namespace

Result<std::unique_ptr<FileDialogs>> makeSystemFileDialogs() {
    if (NFD_Init() != NFD_OKAY) {
        return makeError(ErrorCode::Unsupported, "the system has no dialogs for files to give: {}",
                         SystemFileDialogs::takeError());
    }
    return std::unique_ptr<FileDialogs>(std::make_unique<SystemFileDialogs>());
}

FileNameMatching nativeFileNameMatching() {
#if CORDITE_PLATFORM_MACOS
    return FileNameMatching::ByLastExtension;
#else
    return FileNameMatching::ByEnding;
#endif
}

std::string filterSpec(const FileTypeFilter& filter, FileNameMatching matching) {
    std::vector<std::string_view> endings;
    for (const std::string& extension : filter.extensions) {
        std::string_view ending = extension;
        if (matching == FileNameMatching::ByLastExtension) {
            if (const size_t dot = ending.find_last_of('.'); dot != std::string_view::npos) {
                ending = ending.substr(dot + 1);
            }
        }
        if (!ending.empty() && std::ranges::find(endings, ending) == endings.end()) {
            endings.push_back(ending);
        }
    }
    std::string spec;
    for (const std::string_view ending : endings) {
        if (!spec.empty()) {
            spec += ',';
        }
        spec += ending;
    }
    return spec;
}

FileDialogRequest openSceneRequest(const std::filesystem::path& assetDirectory,
                                   const std::filesystem::path& scenePath) {
    return {.kind = FileDialogKind::Open,
            .title = "Open scene",
            .filters = {sceneFilter()},
            .directory = sceneDirectory(assetDirectory, scenePath)};
}

FileDialogRequest saveSceneRequest(const std::filesystem::path& assetDirectory, const std::filesystem::path& scenePath,
                                   std::string_view sceneName) {
    std::string name = pathToUtf8(scenePath.filename());
    if (name.empty()) {
        name = fileSafeName(sceneName.empty() ? std::string_view("scene") : sceneName) + ".scene.json";
    }
    return {.kind = FileDialogKind::Save,
            .title = "Save scene as",
            .filters = {sceneFilter()},
            .directory = sceneDirectory(assetDirectory, scenePath),
            .name = std::move(name)};
}

FileDialogRequest openProjectRequest(const std::filesystem::path& projectDirectory) {
    return {.kind = FileDialogKind::Open,
            .title = "Open project",
            .filters = {{.name = "Cordite project", .extensions = {"cordite"}}},
            .directory =
                projectDirectory.empty() ? std::filesystem::path() : existingDirectory(projectDirectory.parent_path())};
}

FileDialogRequest newProjectRequest(const std::filesystem::path& projectDirectory) {
    return {.kind = FileDialogKind::Save,
            .title = "New project: where it goes, and what it is called",
            .directory =
                projectDirectory.empty() ? std::filesystem::path() : existingDirectory(projectDirectory.parent_path()),
            .name = "New project"};
}

FileDialogRequest exportGameRequest(const std::filesystem::path& projectDirectory, std::string_view gameName,
                                    GameLayout layout) {
    const std::filesystem::path destination = defaultGameDirectory(projectDirectory);
    if (layout == GameLayout::Bundle) {
        return {.kind = FileDialogKind::Save,
                .title = "Export the game as",
                .directory = existingDirectory(destination.parent_path()),
                .name = fmt::format("{}.app", fileSafeName(gameName))};
    }
    // Inside where it went before, or else where it would go - the person picks or makes
    // the folder there.
    return {.kind = FileDialogKind::Folder, .title = "Export the game to", .directory = existingDirectory(destination)};
}

} // namespace cordite
