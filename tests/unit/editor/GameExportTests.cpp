#include "cordite/editor/GameExport.h"

#include "cordite/assets/Image.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Platform.h"

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

    [[nodiscard]] GameExportDesc desc(const std::filesystem::path& destination) const {
        return {.destination = destination, .shaderDirectory = test::sourceDirectory() / "shaders", .player = player};
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
