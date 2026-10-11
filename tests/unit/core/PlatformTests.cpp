#include "cordite/core/Platform.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <string>

using namespace cordite;

TEST_SUITE("Platform") {
    TEST_CASE("executablePath points at the running test binary") {
        const std::filesystem::path path = executablePath();
        REQUIRE_FALSE(path.empty());
        CHECK(path.is_absolute());
        CHECK(fileExists(path));
        CHECK(pathToUtf8(path.filename()).find("cordite_core_tests") != std::string::npos);
        CHECK(executableDirectory() == path.parent_path());
    }

    TEST_CASE("environmentVariable returns nullopt for unset variables") {
        CHECK_FALSE(environmentVariable("CORDITE_TEST_VARIABLE_THAT_IS_NEVER_SET").has_value());
        // PATH exists on every supported platform.
        const std::optional<std::string> path = environmentVariable("PATH");
        REQUIRE(path.has_value());
        CHECK_FALSE(path->empty());
    }

    TEST_CASE("hardwareThreadCount is at least one") {
        CHECK(hardwareThreadCount() >= 1);
    }

    TEST_CASE("no debugger is attached during automated tests") {
        CHECK_FALSE(isDebuggerPresent());
    }
}
