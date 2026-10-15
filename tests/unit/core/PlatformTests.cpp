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

    TEST_CASE("the configuration directory of the person who runs the program is found") {
        const std::filesystem::path directory = userConfigurationDirectory();
        REQUIRE_FALSE(directory.empty());
        CHECK(directory.is_absolute());
        // On every machine that runs the tests it has been made long before.
        CHECK(std::filesystem::is_directory(directory));
    }

    TEST_CASE("the configuration directory of Linux follows the XDG rule") {
        const std::optional<std::string> none;
        const std::optional<std::string> home = "/home/someone";
        CHECK(xdgConfigurationDirectory("/etc/config-of-someone", home) == pathFromUtf8("/etc/config-of-someone"));
        CHECK(xdgConfigurationDirectory(none, home) == pathFromUtf8("/home/someone") / ".config");
        // Set but empty is not set.
        CHECK(xdgConfigurationDirectory("", home) == pathFromUtf8("/home/someone") / ".config");
        // A path that does not begin at the root is not used: it would depend on where the
        // program happens to be started.
        CHECK(xdgConfigurationDirectory("config", home) == pathFromUtf8("/home/someone") / ".config");
        CHECK(xdgConfigurationDirectory("./config", none).empty());
        CHECK(xdgConfigurationDirectory(none, "someone").empty());
        CHECK(xdgConfigurationDirectory(none, none).empty());
        // Text that is not ASCII stays what it was.
        CHECK(pathToUtf8(xdgConfigurationDirectory("/home/J\xC3\xBCrgen/.cfg", none)) == "/home/J\xC3\xBCrgen/.cfg");
    }

    TEST_CASE("hardwareThreadCount is at least one") {
        CHECK(hardwareThreadCount() >= 1);
    }

    TEST_CASE("no debugger is attached during automated tests") {
        CHECK_FALSE(isDebuggerPresent());
    }

    TEST_CASE("error dialogs can be suppressed any number of times") {
        // The test harness has already done it; doing it again changes nothing.
        CHECK_NOTHROW(suppressErrorDialogs());
        CHECK_NOTHROW(suppressErrorDialogs());
    }
}
