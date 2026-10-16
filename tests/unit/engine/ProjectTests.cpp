#include "cordite/engine/Project.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"

#include "support/TestSupport.h"

#include <string>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Writes a project file with the given text and returns what loading it reports.
Error loadFailure(const TempDirectory& directory, std::string_view text) {
    REQUIRE(writeTextFile(directory.path() / "project.cordite", text).has_value());
    const Result<Project> project = loadProject(directory.path());
    REQUIRE_FALSE(project.has_value());
    return project.error();
}

} // namespace

TEST_SUITE("Project") {
    TEST_CASE("a new project has a file, an asset directory and its name") {
        const TempDirectory directory;
        const std::filesystem::path root = directory.path() / "my-game";
        const Project project = unwrap(createProject(root, "My Game"));

        CHECK(project.directory == root);
        CHECK(project.settings.name == "My Game");
        CHECK(project.settings.startScene.empty());
        CHECK(project.settings.inputMap.empty());
        CHECK(project.inputMapPath().empty());
        CHECK(project.file() == root / "project.cordite");
        CHECK(project.assetDirectory() == root / "assets");
        CHECK(project.cacheDirectory() == root / ".cordite");
        CHECK(project.startScenePath().empty());

        CHECK(directoryExists(root / "assets"));
        CHECK(unwrap(readTextFile(project.file())) ==
              "{\n    \"format\": \"cordite.project\",\n    \"version\": 1,\n    \"name\": \"My Game\",\n"
              "    \"startScene\": \"\",\n    \"inputMap\": \"\"\n}\n");
    }

    TEST_CASE("a project is loaded from its directory or from its file") {
        const TempDirectory directory;
        Project created = unwrap(createProject(directory.path(), "Depot"));
        created.settings.startScene = "scenes/depot.scene.json";
        created.settings.inputMap = "input/controls.input.json";
        REQUIRE(saveProject(created).has_value());

        for (const std::filesystem::path& path : {directory.path(), directory.path() / "project.cordite"}) {
            const Project project = unwrap(loadProject(path));
            CHECK(project.directory == directory.path());
            CHECK(project.settings == created.settings);
            CHECK(project.startScenePath() == directory.path() / "assets" / "scenes" / "depot.scene.json");
            CHECK(project.inputMapPath() == directory.path() / "assets" / "input" / "controls.input.json");
        }
    }

    TEST_CASE("the kProjectFileName constant is the name that is looked for") {
        CHECK(kProjectFileName == "project.cordite");
    }

    TEST_CASE("a project file may have comments and leave settings out") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "project.cordite", R"({
            // Only what differs from the defaults.
            "format": "cordite.project",
            "version": 1
        })")
                    .has_value());
        const Project project = unwrap(loadProject(directory.path()));
        CHECK(project.settings == ProjectSettings());
        CHECK(project.settings.name == "Untitled");
    }

    TEST_CASE("creating a project where there is one already fails and changes nothing") {
        const TempDirectory directory;
        REQUIRE(createProject(directory.path(), "First").has_value());
        const Result<Project> second = createProject(directory.path(), "Second");
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::AlreadyExists);
        CHECK(unwrap(loadProject(directory.path())).settings.name == "First");
    }

    TEST_CASE("a project of an earlier version is told what its file is to be called now") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "cordite.project.json",
                              R"({"format": "cordite.project", "version": 1, "name": "Old"})")
                    .has_value());
        // Named by its directory or by that file.
        for (const std::filesystem::path& path : {directory.path(), directory.path() / "cordite.project.json"}) {
            const Result<Project> project = loadProject(path);
            REQUIRE_FALSE(project.has_value());
            CHECK(project.error().code == ErrorCode::NotFound);
            CHECK(test::errorOf(project).find("earlier version") != std::string::npos);
            CHECK(test::errorOf(project).find("rename its cordite.project.json to project.cordite") !=
                  std::string::npos);
        }
        // Renamed, it is the project it was.
        std::filesystem::rename(directory.path() / "cordite.project.json", directory.path() / "project.cordite");
        CHECK(unwrap(loadProject(directory.path())).settings.name == "Old");
    }

    TEST_CASE("a directory without a project file is not a project") {
        const TempDirectory directory;
        const Result<Project> project = loadProject(directory.path());
        REQUIRE_FALSE(project.has_value());
        CHECK(project.error().code == ErrorCode::NotFound);
        CHECK(project.error().message.find("it has no project.cordite") != std::string::npos);

        CHECK(loadProject(directory.path() / "nowhere").error().code == ErrorCode::NotFound);
    }

    TEST_CASE("project files that are wrong are refused, naming the file") {
        const TempDirectory directory;
        const std::string file = pathToUtf8(directory.path() / "project.cordite");

        SUBCASE("not JSON") {
            CHECK(loadFailure(directory, "name = Showcase").code == ErrorCode::ParseError);
        }
        SUBCASE("another kind of document") {
            const Error error = loadFailure(directory, R"({"format": "cordite.scene", "version": 1})");
            CHECK(error.message == file + ": not a Cordite project: 'format' must be \"cordite.project\"");
        }
        SUBCASE("a newer version") {
            const Error error = loadFailure(directory, R"({"format": "cordite.project", "version": 2})");
            CHECK(error.code == ErrorCode::VersionMismatch);
            CHECK(error.message.find(file) != std::string::npos);
        }
        SUBCASE("an unknown setting") {
            const Error error =
                loadFailure(directory, R"({"format": "cordite.project", "version": 1, "startscene": "a.scene.json"})");
            CHECK(error.message.find("startscene") != std::string::npos);
            CHECK(error.message.find(file) != std::string::npos);
        }
        SUBCASE("a setting of the wrong type") {
            const Error error = loadFailure(directory, R"({"format": "cordite.project", "version": 1, "name": 7})");
            CHECK(error.message.find("name") != std::string::npos);
        }
    }

    TEST_CASE("a start scene must be a plain path inside the asset directory") {
        const TempDirectory directory;
        for (const char* startScene : {"../outside.scene.json", "/abs/a.scene.json", "C:/a.scene.json",
                                       "scenes\\\\a.scene.json", "./scenes/a.scene.json", "scenes//a.scene.json"}) {
            CAPTURE(startScene);
            const Error error = loadFailure(
                directory,
                fmt::format(R"({{"format": "cordite.project", "version": 1, "startScene": "{}"}})", startScene));
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message.find("'startScene' must be a path inside the asset directory") != std::string::npos);
        }

        // The same rule holds when saving.
        Project project;
        project.directory = directory.path();
        project.settings.startScene = "../outside.scene.json";
        CHECK_FALSE(saveProject(project).has_value());
    }

    TEST_CASE("an input map must be a plain path inside the asset directory as well") {
        const TempDirectory directory;
        for (const char* inputMap : {"../controls.input.json", "/abs/controls.input.json", "C:/controls.input.json",
                                     "./input/controls.input.json"}) {
            CAPTURE(inputMap);
            const Error error = loadFailure(
                directory, fmt::format(R"({{"format": "cordite.project", "version": 1, "inputMap": "{}"}})", inputMap));
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message.find("'inputMap' must be a path inside the asset directory, such as "
                                     "\"input/controls.input.json\"") != std::string::npos);
        }
        Project project;
        project.directory = directory.path();
        project.settings.inputMap = "../controls.input.json";
        CHECK_FALSE(saveProject(project).has_value());
        // A project that names one says where it is; whether it is there is for whoever reads it.
        project.settings.inputMap = "input/controls.input.json";
        REQUIRE(saveProject(project).has_value());
        CHECK(unwrap(loadProject(directory.path())).inputMapPath() ==
              directory.path() / "assets" / "input" / "controls.input.json");
    }

    TEST_CASE("a project without a directory cannot be saved") {
        const Status saved = saveProject(Project());
        REQUIRE_FALSE(saved.has_value());
        CHECK(saved.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("the name of a game is made fit to name files and directories") {
        CHECK(gameFileName("Showcase") == "Showcase");
        CHECK(gameFileName("  Depot: the Game!  ") == "Depot the Game");
        CHECK(gameFileName("Café-Bar_2") == "Café-Bar_2");
        CHECK(gameFileName("") == "Game");
        CHECK(gameFileName("?*/") == "Game");
        // What Windows keeps for its devices is no name for a file, whatever the case.
        CHECK(gameFileName("con") == "Game");
        CHECK(gameFileName("Lpt1") == "Game");
        CHECK(gameFileName("COM0") == "Game");
        CHECK(gameFileName("com¹") == "Game");
        CHECK(gameFileName("Console") == "Console");
        CHECK(gameFileName("COM10") == "COM10");
        // Nor is a directory beside the program of a game, where programs have no ending.
        CHECK(gameFileName("Assets") == "Game");
        CHECK(gameFileName("licenses.") == "Game");
    }

    TEST_CASE("the settings are described for tools") {
        const TypeInfo& type = projectSettingsTypeInfo();
        CHECK(type.properties().size() == 3);
        for (const PropertyInfo& property : type.properties()) {
            CHECK_FALSE(property.attributes().description.empty());
        }
    }
}
