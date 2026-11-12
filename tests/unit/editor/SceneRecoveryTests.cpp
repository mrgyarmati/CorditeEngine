#include "cordite/editor/SceneRecovery.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <string>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

TEST_SUITE("SceneRecovery") {
    TEST_CASE("a kept scene is written with the file it was of, and read back as it was") {
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / ".cordite" / std::string(kSceneRecoveryFileName);
        const SceneRecovery kept{.scenePath = directory.path() / "assets" / "yard.scene.json",
                                 .scene = Json::parse(R"({"format": "cordite.scene", "version": 1, "entities": []})")};
        REQUIRE(writeSceneRecovery(file, kept).has_value());
        const SceneRecovery read = unwrap(readSceneRecovery(file));
        CHECK(read.scenePath == kept.scenePath);
        CHECK(read.scene == kept.scene);

        // A scene that never had a file.
        REQUIRE(writeSceneRecovery(file, {.scene = kept.scene}).has_value());
        CHECK(unwrap(readSceneRecovery(file)).scenePath.empty());
    }

    TEST_CASE("a kept scene that is not there, or is no such file, is an error that says so") {
        const TempDirectory directory;
        const std::filesystem::path file = directory.path() / "recovery.json";
        CHECK(readSceneRecovery(file).error().code == ErrorCode::NotFound);

        const auto refused = [&file](std::string_view text, ErrorCode code, std::string_view fragment) {
            REQUIRE(writeTextFile(file, text).has_value());
            const Result<SceneRecovery> read = readSceneRecovery(file);
            REQUIRE_FALSE(read.has_value());
            CHECK(read.error().code == code);
            CHECK_MESSAGE(test::errorOf(read).find(fragment) != std::string::npos, test::errorOf(read));
            // The file is named, so that a person knows which to look at.
            CHECK(test::errorOf(read).find("recovery.json") != std::string::npos);
        };
        refused(R"({"format": "cordite.scene", "version": 1})", ErrorCode::ParseError, "cordite.recovery");
        refused(R"({"format": "cordite.recovery", "version": 1})", ErrorCode::ParseError, "'scene'");
        refused(R"({"format": "cordite.recovery", "version": 1, "scene": []})", ErrorCode::ParseError, "'scene'");
        refused(R"({"format": "cordite.recovery", "version": 1, "scene": {}, "scenePath": 3})", ErrorCode::ParseError,
                "'scenePath'");
        refused(R"({"format": "cordite.recovery", "version": 1, "scene": {}, "when": "now"})", ErrorCode::ParseError,
                "when");
        refused(R"({"format": "cordite.recovery", "version": 99, "scene": {}})", ErrorCode::VersionMismatch, "99");
        REQUIRE(writeTextFile(file, "{\"format\": ").has_value());
        CHECK_FALSE(readSceneRecovery(file).has_value());
    }
}
