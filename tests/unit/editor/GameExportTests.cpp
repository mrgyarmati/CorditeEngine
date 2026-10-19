#include "cordite/editor/GameExport.h"

#include "cordite/assets/Image.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/engine/Game.h"
#include "cordite/engine/Project.h"
#include "cordite/rhi/GraphicsDevice.h"

#include "support/TestSupport.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A project with a scene, its .meta, a texture and a directory that the assets do not
/// count; and beside it a stand-in for cordite-player with a notice, as the build leaves
/// them, to see what an export copies of them.
struct ExportFixture {
    TempDirectory directory;
    Project project = unwrap(createProject(directory.path() / "project", "Depot: the Game!"));
    std::filesystem::path player = directory.path() / "build" / pathFromUtf8(executableFileName("cordite-player"));

    ExportFixture() {
        const std::filesystem::path assets = project.assetDirectory();
        REQUIRE(writeTextFile(assets / "scenes" / "depot.scene.json",
                              R"({"format": "cordite.scene", "version": 1, "entities": []})")
                    .has_value());
        REQUIRE(
            writeTextFile(assets / "scenes" / "depot.scene.json.meta",
                          R"({"format": "cordite.asset", "version": 1, "id": "22222222-2222-4222-8222-222222222222"})")
                .has_value());
        const Image image = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        REQUIRE(ensureDirectory(assets / "textures").has_value());
        REQUIRE(image.savePng(assets / "textures" / "wall.png").has_value());
        REQUIRE(writeTextFile(assets / ".scratch" / "notes.txt", "not for the game\n").has_value());
        project.settings.startScene = "scenes/depot.scene.json";
        REQUIRE(saveProject(project).has_value());

        REQUIRE(writeTextFile(player, "a program").has_value());
        REQUIRE(writeTextFile(player.parent_path() / "licenses" / "fmt.txt", "MIT").has_value());
    }

    /// An export laid out as `layout` - a directory unless it is told otherwise, on every
    /// system, so that what is tested is the same everywhere.
    [[nodiscard]] GameExportDesc desc(const std::filesystem::path& destination,
                                      GameLayout layout = GameLayout::Directory) const {
        return {.destination = destination,
                .shaderDirectory = test::sourceDirectory() / "shaders",
                .player = player,
                .layout = layout};
    }

    /// The error of an export that does not happen.
    [[nodiscard]] Error refusal(const GameExportDesc& desc) const {
        const Result<GameExport> made = exportGame(project, desc);
        REQUIRE_FALSE(made.has_value());
        return made.error();
    }
};

} // namespace

TEST_SUITE("GameExport") {
    TEST_CASE("the program of a game is named after it, as files may be named") {
        CHECK(gameProgramName("Showcase") == executableFileName("Showcase"));
        CHECK(gameProgramName("  Depot: the Game!  ") == executableFileName("Depot the Game"));
        // As gameFileName() has it (ProjectTests), and what a program is called here.
        CHECK(gameProgramName("con") == executableFileName("Game"));
    }

    TEST_CASE("a game of macOS is an application bundle, and a person may leave out what a bundle is called") {
        CHECK(nameOf(GameLayout::Directory) == "directory");
        CHECK(nameOf(GameLayout::Bundle) == "bundle");
#if CORDITE_PLATFORM_MACOS
        CHECK(nativeGameLayout() == GameLayout::Bundle);
#else
        CHECK(nativeGameLayout() == GameLayout::Directory);
#endif
        const std::filesystem::path out = pathFromUtf8("out");
        CHECK(gameDestinationFor(out / "Depot", GameLayout::Bundle) == out / "Depot.app");
        CHECK(gameDestinationFor(out / "Depot.app", GameLayout::Bundle) == out / "Depot.app");
        CHECK(gameDestinationFor(out / "Depot" / "", GameLayout::Bundle) == out / "Depot.app");
        CHECK(gameDestinationFor(out / "Depot 2.0", GameLayout::Bundle) == out / "Depot 2.0.app");
        CHECK(gameDestinationFor(out / "Depot", GameLayout::Directory) == out / "Depot");
        CHECK(gameDestinationFor({}, GameLayout::Bundle).empty());
    }

    TEST_CASE("what a bundle tells macOS of its game is its name, its program and what it is known by") {
        ProjectSettings settings{.name = "Depot & <Sons>"};
        const std::string plain = bundleInfoPlist(settings, "Depot Sons");
        CHECK(plain.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist"));
        CHECK(plain.find("<key>CFBundleExecutable</key>\n    <string>Depot Sons</string>") != std::string::npos);
        // What a name holds that XML would read otherwise is written as XML holds it.
        CHECK(plain.find("<key>CFBundleName</key>\n    <string>Depot &amp; &lt;Sons&gt;</string>") !=
              std::string::npos);
        CHECK(plain.find("<string>local.cordite.Depot-Sons</string>") != std::string::npos);
        CHECK(plain.find("<key>CFBundlePackageType</key>\n    <string>APPL</string>") != std::string::npos);
        CHECK(plain.find("<key>NSHighResolutionCapable</key>\n    <true/>") != std::string::npos);
        // The oldest macOS it runs on only where the build says.
        CHECK(plain.find("LSMinimumSystemVersion") == std::string::npos);
        CHECK(bundleInfoPlist(settings, "Depot", "11.0")
                  .find("<key>LSMinimumSystemVersion</key>\n    <string>11.0</string>") != std::string::npos);
        // What the project says it is known by.
        settings.bundleIdentifier = "com.example.depot";
        CHECK(bundleInfoPlist(settings, "Depot")
                  .find("<key>CFBundleIdentifier</key>\n    "
                        "<string>com.example.depot</string>") != std::string::npos);
        CHECK(plain.ends_with("</dict>\n</plist>\n"));
    }

    TEST_CASE("a game exported as a bundle has its program in Contents/MacOS and the rest of it in Resources") {
        ExportFixture fixture;
        const std::filesystem::path bundle = fixture.directory.path() / "games" / "Depot.app";
        const GameExport made = unwrap(exportGame(fixture.project, fixture.desc(bundle, GameLayout::Bundle)));
        const std::string program = gameProgramName("Depot the Game");
        CHECK(made.program == bundle / "Contents" / "MacOS" / pathFromUtf8(program));
        CHECK(made.game == bundle / "Contents" / "Resources");
        CHECK(unwrap(readTextFile(made.program)) == "a program");
        CHECK(unwrap(readTextFile(bundle / "Contents" / "Info.plist")).find("<string>" + program + "</string>") !=
              std::string::npos);
        // Where the program finds its game.
        CHECK(gameDirectoryOf(made.program) == made.game);
        const Project exported = unwrap(loadProject(made.game));
        CHECK(exported.settings == fixture.project.settings);
        CHECK(fileExists(made.game / "assets" / "textures" / "wall.png"));
        CHECK(unwrap(readTextFile(made.game / "licenses" / "fmt.txt")) == "MIT");
        CHECK(directoryExists(exported.cacheDirectory() / "shaders"));
        const Json record = unwrap(readJsonFile(exported.cacheDirectory() / "export.json"));
        CHECK(record["layout"] == "bundle");
        CHECK(record["version"] == 2);
        CHECK(record["program"] == program);
        // Nothing beside the bundle's Contents.
        size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(bundle)) {
            CHECK(entry.path().filename() == "Contents");
            ++entries;
        }
        CHECK(entries == 1);
#if !CORDITE_PLATFORM_WINDOWS
        // A program that runs, which a copy may not have kept.
        CHECK((std::filesystem::status(made.program).permissions() & std::filesystem::perms::owner_exec) !=
              std::filesystem::perms::none);
#endif

        // Exported again it is replaced - a name of its own, another program - and what a
        // person put beside its Contents stays.
        REQUIRE(writeTextFile(bundle / "README.txt", "how to play").has_value());
        fixture.project.settings.name = "Depot 2";
        REQUIRE(saveProject(fixture.project).has_value());
        const GameExport again = unwrap(exportGame(fixture.project, fixture.desc(bundle, GameLayout::Bundle)));
        CHECK(fileExists(again.program));
        CHECK_FALSE(std::filesystem::exists(made.program));
        CHECK(unwrap(loadProject(again.game)).settings.name == "Depot 2");
        CHECK(unwrap(readTextFile(bundle / "README.txt")) == "how to play");
    }

    TEST_CASE("a bundle brings the Vulkan that is beside the player, and says so when there is none") {
        const ExportFixture fixture;
        const std::filesystem::path bundle = fixture.directory.path() / "games" / "Depot.app";
        const std::filesystem::path frameworks = bundle / "Contents" / "Frameworks";
        {
            const test::LogCapture capture(LogLevel::Warn);
            REQUIRE(exportGame(fixture.project, fixture.desc(bundle, GameLayout::Bundle)).has_value());
            CHECK(capture.contains(std::string(kBundledVulkanLibrary) + " is not beside"));
        }
        CHECK_FALSE(std::filesystem::exists(frameworks));

        // Where the build put MoltenVK beside the player, the bundle brings it - where its
        // program looks for it - and counts it.
        REQUIRE(writeTextFile(fixture.player.parent_path() / pathFromUtf8(kBundledVulkanLibrary), "Vulkan on Metal")
                    .has_value());
        const test::LogCapture capture(LogLevel::Warn);
        const GameExport made = unwrap(exportGame(fixture.project, fixture.desc(bundle, GameLayout::Bundle)));
        CHECK_FALSE(capture.contains("is not beside"));
        CHECK(unwrap(readTextFile(frameworks / pathFromUtf8(kBundledVulkanLibrary))) == "Vulkan on Metal");
        CHECK(bundledLibrary(made.program, kBundledVulkanLibrary) == frameworks / pathFromUtf8(kBundledVulkanLibrary));
        // A game of Windows and Linux has the Vulkan of its system.
        const std::filesystem::path directory = fixture.directory.path() / "games" / "Depot";
        const GameExport plain = unwrap(exportGame(fixture.project, fixture.desc(directory)));
        CHECK_FALSE(std::filesystem::exists(directory / pathFromUtf8(kBundledVulkanLibrary)));
        // Beside what a directory holds, the bundle has its Info.plist and its Vulkan.
        CHECK(made.files == plain.files + 2);
    }

    TEST_CASE("a bundle is called so, and a game is not replaced by one laid out the other way") {
        const ExportFixture fixture;
        const Error unnamed =
            fixture.refusal(fixture.desc(fixture.directory.path() / "games" / "Depot", GameLayout::Bundle));
        CHECK(unnamed.code == ErrorCode::InvalidArgument);
        CHECK(unnamed.message.find(".app") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path() / "games"));

        // A directory where a bundle was exported, ...
        const std::filesystem::path bundle = fixture.directory.path() / "games" / "Depot.app";
        REQUIRE(exportGame(fixture.project, fixture.desc(bundle, GameLayout::Bundle)).has_value());
        const Error intoBundle = fixture.refusal(fixture.desc(bundle));
        CHECK(intoBundle.code == ErrorCode::AlreadyExists);
        CHECK(intoBundle.message.find("exported as a bundle") != std::string::npos);
        CHECK(directoryExists(bundle / "Contents" / "MacOS"));
        // ... and a bundle where a directory was.
        const std::filesystem::path directory = fixture.directory.path() / "games" / "Plain.app";
        REQUIRE(exportGame(fixture.project, fixture.desc(directory)).has_value());
        const Error intoDirectory = fixture.refusal(fixture.desc(directory, GameLayout::Bundle));
        CHECK(intoDirectory.code == ErrorCode::AlreadyExists);
        CHECK(intoDirectory.message.find("exported as a directory") != std::string::npos);
        CHECK(fileExists(directory / "project.cordite"));
    }

    TEST_CASE("a game is its program, its project, its assets, its shaders compiled and its notices") {
        const ExportFixture fixture;
        const std::filesystem::path game = fixture.directory.path() / "games" / "Depot";
        const GameExport made = unwrap(exportGame(fixture.project, fixture.desc(game)));
        CHECK(made.program == game / pathFromUtf8(executableFileName("Depot the Game")));
        CHECK(unwrap(readTextFile(made.program)) == "a program");
        CHECK(unwrap(readTextFile(game / "licenses" / "fmt.txt")) == "MIT");
        // The project as it is, so that the game finds its start scene.
        const Project exported = unwrap(loadProject(game));
        CHECK(exported.settings == fixture.project.settings);
        // The assets with the ids in their .meta files; what the assets do not count stays.
        CHECK(fileExists(game / "assets" / "scenes" / "depot.scene.json"));
        CHECK(unwrap(readTextFile(game / "assets" / "scenes" / "depot.scene.json.meta")) ==
              unwrap(readTextFile(fixture.project.assetDirectory() / "scenes" / "depot.scene.json.meta")));
        CHECK(fileExists(game / "assets" / "textures" / "wall.png"));
        CHECK_FALSE(std::filesystem::exists(game / "assets" / ".scratch"));
        // The engine's shaders, where the engine of a game looks for them.
        size_t compiled = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(exported.cacheDirectory() / "shaders")) {
            compiled += entry.is_regular_file() ? 1 : 0;
        }
        CHECK(compiled > 0);
        // And a record that an export made the game, which another export may replace.
        CHECK(fileExists(exported.cacheDirectory() / "export.json"));
        // Counted: the program, a notice, the project, three assets, the shaders and the record.
        CHECK(made.files == 7 + compiled);
        CHECK(made.bytes > 0);
        // The project is as it was.
        CHECK_FALSE(std::filesystem::exists(fixture.project.cacheDirectory()));

        // Into a directory that is there, and empty, as well.
        const std::filesystem::path empty = fixture.directory.path() / "empty";
        REQUIRE(ensureDirectory(empty).has_value());
        CHECK(unwrap(exportGame(fixture.project, fixture.desc(empty))).files == made.files);
    }

    TEST_CASE("a game exported again replaces what the export wrote, and nothing else") {
        ExportFixture fixture;
        const std::filesystem::path game = fixture.directory.path() / "games" / "Depot";
        const GameExport first = unwrap(exportGame(fixture.project, fixture.desc(game)));
        REQUIRE(writeTextFile(game / "README.txt", "how to play").has_value());
        // What the project no longer has, the game no longer has; another name, another program.
        std::filesystem::remove(fixture.project.assetDirectory() / "textures" / "wall.png");
        fixture.project.settings.name = "Depot 2";
        REQUIRE(saveProject(fixture.project).has_value());
        const GameExport second = unwrap(exportGame(fixture.project, fixture.desc(game)));
        CHECK(second.program == game / pathFromUtf8(executableFileName("Depot 2")));
        CHECK(fileExists(second.program));
        CHECK_FALSE(std::filesystem::exists(first.program));
        CHECK_FALSE(std::filesystem::exists(game / "assets" / "textures" / "wall.png"));
        CHECK(unwrap(loadProject(game)).settings.name == "Depot 2");
        CHECK(second.files == first.files - 1);
        CHECK(unwrap(readTextFile(game / "README.txt")) == "how to play");
    }

    TEST_CASE("a game that an export of an earlier version made is a directory, and is replaced as one") {
        const ExportFixture fixture;
        const std::filesystem::path game = fixture.directory.path() / "games" / "Depot";
        REQUIRE(exportGame(fixture.project, fixture.desc(game)).has_value());
        // As version 1 wrote it, without a layout: every game was a directory then.
        Json record = unwrap(readJsonFile(game / ".cordite" / "export.json"));
        record.erase("layout");
        record["version"] = 1;
        REQUIRE(writeJsonFile(game / ".cordite" / "export.json", record).has_value());
        const GameExport again = unwrap(exportGame(fixture.project, fixture.desc(game)));
        CHECK(unwrap(readJsonFile(game / ".cordite" / "export.json"))["layout"] == "directory");
        CHECK(fileExists(again.program));
        // A record that says another layout is no record of this one.
        record["layout"] = "folder";
        REQUIRE(writeJsonFile(game / ".cordite" / "export.json", record).has_value());
        CHECK(fixture.refusal(fixture.desc(game)).code == ErrorCode::AlreadyExists);
    }

    TEST_CASE("the record of an export names a file in its directory, or is no record") {
        const ExportFixture fixture;
        const std::filesystem::path games = fixture.directory.path() / "games";
        const std::filesystem::path game = games / "Depot";
        REQUIRE(writeTextFile(games / "keep.txt", "mine").has_value());
        for (const std::string_view program : {"../keep.txt", "..", ".", "", "a/b", "a\\b", "C:keep.txt"}) {
            CAPTURE(program);
            Json record = Json::object();
            record["format"] = "cordite.export";
            record["version"] = 1;
            record["program"] = std::string(program);
            REQUIRE(writeJsonFile(game / ".cordite" / "export.json", record).has_value());
            CHECK(fixture.refusal(fixture.desc(game)).code == ErrorCode::AlreadyExists);
        }
        CHECK(unwrap(readTextFile(games / "keep.txt")) == "mine");
        // Nor is anything else a reason to replace what is there.
        for (const std::string_view text : {R"({"format": "cordite.scene", "program": "keep.txt"})", "not JSON"}) {
            CAPTURE(text);
            REQUIRE(writeTextFile(game / ".cordite" / "export.json", text).has_value());
            CHECK(fixture.refusal(fixture.desc(game)).code == ErrorCode::AlreadyExists);
        }
        CHECK(fileExists(game / ".cordite" / "export.json"));
    }

    TEST_CASE("a game is not made where it cannot be, nor of what is not there") {
        const ExportFixture fixture;
        const std::filesystem::path game = fixture.directory.path() / "games" / "Depot";
        CHECK(fixture.refusal(fixture.desc({})).code == ErrorCode::InvalidArgument);
        // Into the project it would copy itself.
        CHECK(fixture.refusal(fixture.desc(fixture.project.directory / "build")).code == ErrorCode::InvalidArgument);
        CHECK(fixture.refusal(fixture.desc(fixture.project.directory)).code == ErrorCode::InvalidArgument);
        // What is there stays as it is.
        REQUIRE(writeTextFile(fixture.directory.path() / "taken" / "file.txt", "mine").has_value());
        CHECK(fixture.refusal(fixture.desc(fixture.directory.path() / "taken")).code == ErrorCode::AlreadyExists);
        CHECK(unwrap(readTextFile(fixture.directory.path() / "taken" / "file.txt")) == "mine");
        const Error file = fixture.refusal(fixture.desc(fixture.directory.path() / "taken" / "file.txt"));
        CHECK(file.code == ErrorCode::AlreadyExists);
        CHECK(file.message.find("is a file") != std::string::npos);

        GameExportDesc noPlayer = fixture.desc(game);
        noPlayer.player = fixture.directory.path() / "nowhere" / "cordite-player";
        CHECK(fixture.refusal(noPlayer).code == ErrorCode::NotFound);
        GameExportDesc noSources = fixture.desc(game);
        noSources.shaderDirectory = fixture.directory.path() / "nowhere";
        CHECK(fixture.refusal(noSources).code == ErrorCode::NotFound);
        std::filesystem::remove_all(fixture.player.parent_path() / "licenses");
        const Error noNotices = fixture.refusal(fixture.desc(game));
        CHECK(noNotices.code == ErrorCode::NotFound);
        CHECK(noNotices.message.find("notices") != std::string::npos);
        REQUIRE(writeTextFile(fixture.player.parent_path() / "licenses" / "fmt.txt", "MIT").has_value());

        std::filesystem::remove_all(fixture.project.assetDirectory());
        CHECK(fixture.refusal(fixture.desc(game)).code == ErrorCode::NotFound);
        // None of it wrote anything.
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path() / "games"));
    }

    TEST_CASE("what an export that fails has written is taken away again") {
        const ExportFixture fixture;
        // Shaders that do not compile are found out after the files were copied.
        GameExportDesc noShaders = fixture.desc(fixture.directory.path() / "games" / "Depot");
        noShaders.shaderDirectory = fixture.directory.path() / "empty-shaders";
        REQUIRE(ensureDirectory(noShaders.shaderDirectory).has_value());
        CHECK(fixture.refusal(noShaders).message.find(".slang") != std::string::npos);
        // The directories it made go, up to the one that was there.
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path() / "games"));
        CHECK(directoryExists(fixture.directory.path()));

        // From a directory that was there, empty, what went in goes; the directory stays.
        const std::filesystem::path empty = fixture.directory.path() / "empty";
        REQUIRE(ensureDirectory(empty).has_value());
        noShaders.destination = empty;
        CHECK(fixture.refusal(noShaders).message.find(".slang") != std::string::npos);
        CHECK(directoryExists(empty));
        CHECK(std::filesystem::is_empty(empty));

        // A game exported again that fails takes the game away, and leaves what else is there.
        const std::filesystem::path again = fixture.directory.path() / "again";
        REQUIRE(exportGame(fixture.project, fixture.desc(again)).has_value());
        REQUIRE(writeTextFile(again / "README.txt", "how to play").has_value());
        noShaders.destination = again;
        CHECK(fixture.refusal(noShaders).message.find(".slang") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(again / "assets"));
        CHECK_FALSE(std::filesystem::exists(again / ".cordite"));
        CHECK_FALSE(std::filesystem::exists(again / "licenses"));
        CHECK_FALSE(std::filesystem::exists(again / "project.cordite"));
        CHECK(unwrap(readTextFile(again / "README.txt")) == "how to play");
    }
}
