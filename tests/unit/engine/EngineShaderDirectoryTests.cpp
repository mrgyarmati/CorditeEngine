#include "cordite/core/FileSystem.h"
#include "cordite/engine/Engine.h"

#include "support/TestSupport.h"

#include <filesystem>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

TEST_SUITE("EngineShaderDirectory") {
    TEST_CASE("the engine's shaders are where they are asked for, or found beside the program") {
        const TempDirectory directory;
        CHECK(unwrap(engineShaderDirectory(directory.path())) == directory.path());

        const Result<std::filesystem::path> missing = engineShaderDirectory(directory.path() / "none");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);

        // Unasked: the repository's in a development build, the copy beside the program in
        // any other - both hold the sources of every pass.
        const std::filesystem::path found = unwrap(engineShaderDirectory());
        CHECK(fileExists(found / "passes" / "MeshVertex.slang"));
    }
}
