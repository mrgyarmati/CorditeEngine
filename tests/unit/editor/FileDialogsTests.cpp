#include "cordite/editor/FileDialogs.h"

#include "cordite/core/Base.h"
#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#if CORDITE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>
#endif

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;

namespace {

const FileTypeFilter kScene{.name = "Scene", .extensions = {"scene.json"}};

/// Whether two directories are one, however each is written: a canonical path and a short
/// one of Windows are one directory.
bool sameDirectory(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code error;
    return std::filesystem::equivalent(first, second, error);
}

} // namespace

TEST_SUITE("FileDialogs") {
    TEST_CASE("a filter says how names end, or what they end in where a system types files by that") {
        CHECK(filterSpec(kScene, FileNameMatching::ByEnding) == "scene.json");
        CHECK(filterSpec(kScene, FileNameMatching::ByLastExtension) == "json");
        const FileTypeFilter images{.name = "Images", .extensions = {"png", "jpg", "hdr"}};
        CHECK(filterSpec(images, FileNameMatching::ByEnding) == "png,jpg,hdr");
        CHECK(filterSpec(images, FileNameMatching::ByLastExtension) == "png,jpg,hdr");
        // Endings that come to one extension are given once, and an empty one not at all.
        const FileTypeFilter data{.name = "Data", .extensions = {"scene.json", "prefab.json", "json", ""}};
        CHECK(filterSpec(data, FileNameMatching::ByEnding) == "scene.json,prefab.json,json");
        CHECK(filterSpec(data, FileNameMatching::ByLastExtension) == "json");
        CHECK(filterSpec({.name = "Nothing"}, FileNameMatching::ByEnding).empty());
        // macOS types a file by its last extension.
        CHECK(nativeFileNameMatching() ==
              (CORDITE_PLATFORM_MACOS ? FileNameMatching::ByLastExtension : FileNameMatching::ByEnding));
    }

    TEST_CASE("a scene is opened from the directory of the open one, or from the scenes of the assets") {
        const TempDirectory directory;
        const std::filesystem::path assets = directory.path() / "assets";
        std::filesystem::create_directories(assets / "levels");

        FileDialogRequest request = openSceneRequest(assets, {});
        CHECK(request.kind == FileDialogKind::Open);
        CHECK(request.title == "Open scene");
        CHECK(request.filters == std::vector<FileTypeFilter>{kScene});
        CHECK(request.directory == assets);
        CHECK(request.name.empty());
        std::filesystem::create_directories(assets / "scenes");
        CHECK(openSceneRequest(assets, {}).directory == assets / "scenes");

        // The open scene's directory first - or the nearest above it that is there.
        CHECK(openSceneRequest(assets, assets / "levels" / "yard.scene.json").directory == assets / "levels");
        CHECK(openSceneRequest(assets, assets / "gone" / "deeper" / "yard.scene.json").directory == assets);
        // A scene of no project opens from its own directory, or from where the system likes.
        CHECK(openSceneRequest({}, assets / "levels" / "yard.scene.json").directory == assets / "levels");
        CHECK(openSceneRequest({}, {}).directory.empty());
    }

    TEST_CASE("a scene is saved under the name of its file, or of the scene made a name a file can have") {
        const TempDirectory directory;
        const std::filesystem::path assets = directory.path() / "assets";
        std::filesystem::create_directories(assets / "levels");

        const FileDialogRequest request = saveSceneRequest(assets, assets / "levels" / "yard.scene.json", "Yard");
        CHECK(request.kind == FileDialogKind::Save);
        CHECK(request.title == "Save scene as");
        CHECK(request.filters == std::vector<FileTypeFilter>{kScene});
        CHECK(request.directory == assets / "levels");
        CHECK(request.name == "yard.scene.json");

        CHECK(saveSceneRequest(assets, {}, "Yard").name == "Yard.scene.json");
        CHECK(saveSceneRequest(assets, {}, "Yard").directory == assets);
        // What would be read as a directory, a drive or a pattern is no part of a name.
        CHECK(saveSceneRequest(assets, {}, R"(A/B\C: "D"?*<>|)").name == "A-B-C- -D------.scene.json");
        CHECK(saveSceneRequest(assets, {}, "").name == "scene.scene.json");
    }

    TEST_CASE("a project is opened, and made, from the directory the open one is in") {
        const TempDirectory directory;
        const std::filesystem::path games = directory.path() / "games";
        std::filesystem::create_directories(games / "depot");

        const FileDialogRequest open = openProjectRequest(games / "depot");
        CHECK(open.kind == FileDialogKind::Open);
        CHECK(open.title == "Open project");
        CHECK(open.filters == std::vector<FileTypeFilter>{{.name = "Cordite project", .extensions = {"cordite"}}});
        CHECK(open.directory == games);

        // A new project is a directory to be: its name is the project's.
        const FileDialogRequest made = newProjectRequest(games / "depot");
        CHECK(made.kind == FileDialogKind::Save);
        CHECK(made.filters.empty());
        CHECK(made.directory == games);
        CHECK(made.name == "New project");

        // Without a project, where the system likes.
        CHECK(openProjectRequest({}).directory.empty());
        CHECK(newProjectRequest({}).directory.empty());
    }

    TEST_CASE("a game is exported into a folder, or as the bundle of macOS that is its destination") {
        const TempDirectory directory;
        const std::filesystem::path project = directory.path() / "depot";
        std::filesystem::create_directories(project);

        // Where it would go is not there yet: the folder is picked, or made, beside the project.
        const FileDialogRequest folder = exportGameRequest(project, "Depot", GameLayout::Directory);
        CHECK(folder.kind == FileDialogKind::Folder);
        CHECK(folder.title == "Export the game to");
        CHECK(sameDirectory(folder.directory, directory.path()));
        // Once it is, the dialog opens in it.
        std::filesystem::create_directories(directory.path() / "depot game");
        CHECK(sameDirectory(exportGameRequest(project, "Depot", GameLayout::Directory).directory,
                            directory.path() / "depot game"));

        const FileDialogRequest bundle = exportGameRequest(project, "Depot", GameLayout::Bundle);
        CHECK(bundle.kind == FileDialogKind::Save);
        CHECK(bundle.title == "Export the game as");
        CHECK(bundle.name == "Depot.app");
        CHECK(sameDirectory(bundle.directory, directory.path()));
        CHECK(exportGameRequest(project, "Big: Game", GameLayout::Bundle).name == "Big- Game.app");
    }

    TEST_CASE("a dialog of the system is shown from a thread that can, and its window keeps answering it") {
        std::thread::id ranOn;
        bool singleThreaded = false;
#if CORDITE_PLATFORM_WINDOWS
        // As the editor's main thread is once miniaudio has made its context there: a
        // multithreaded apartment of COM, with a window of its own that a dialog is shown
        // over - and which the dialog tells so, by a message the thread has to answer.
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        REQUIRE(SUCCEEDED(apartment));
        HWND window = CreateWindowExW(0, L"STATIC", L"asks", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                      nullptr, nullptr);
        REQUIRE(window != nullptr);
        DWORD_PTR answered = 0;
        LRESULT sent = 0;
        runForSystemDialog([&] {
            ranOn = std::this_thread::get_id();
            APTTYPE type{};
            APTTYPEQUALIFIER qualifier{};
            singleThreaded =
                SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
            // Unanswered, this would wait for good; the timeout makes it a failure instead.
            sent = SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"answered"), SMTO_BLOCK, 5000,
                                       &answered);
        });
        CHECK(ranOn != std::this_thread::get_id());
        CHECK(singleThreaded);
        CHECK(sent != 0);
        std::array<wchar_t, 16> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        CHECK(std::wstring(text.data()) == L"answered");
        DestroyWindow(window);
        CoUninitialize();
#else
        // macOS shows its dialogs on the main thread, and Linux asks the portal from any.
        runForSystemDialog([&] {
            ranOn = std::this_thread::get_id();
            singleThreaded = true;
        });
        CHECK(ranOn == std::this_thread::get_id());
        CHECK(singleThreaded);
#endif
    }

    TEST_CASE("the system's dialogs are made where the system has them, and say why where it has not") {
        // Made and let go of twice: what they take of the system is given back. Nothing is
        // shown - that would wait for a person.
        for (int time = 0; time < 2; ++time) {
            const Result<std::unique_ptr<FileDialogs>> dialogs = makeSystemFileDialogs();
#if CORDITE_PLATFORM_WINDOWS || CORDITE_PLATFORM_MACOS
            REQUIRE_MESSAGE(dialogs.has_value(), test::errorOf(dialogs));
            CHECK(*dialogs != nullptr);
#else
            // Linux asks xdg-desktop-portal over D-Bus, which a machine without a desktop has
            // not got.
            if (!dialogs) {
                CHECK(dialogs.error().code == ErrorCode::Unsupported);
                CHECK(dialogs.error().message.find("dialogs") != std::string::npos);
            }
#endif
        }
    }
}
