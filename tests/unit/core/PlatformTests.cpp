#include "cordite/core/Platform.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <cstdio>
#include <filesystem>
#include <optional>
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

    TEST_CASE("a program is called by its name and what programs end with on the system") {
        // The test binary itself is named so.
        CHECK(executablePath().filename() == pathFromUtf8(executableFileName("cordite_core_tests")));
#if CORDITE_PLATFORM_WINDOWS
        CHECK(executableFileName("Depot") == "Depot.exe");
        CHECK(executableFileName("Café 2") == "Café 2.exe");
#else
        CHECK(executableFileName("Depot") == "Depot");
#endif
    }

    TEST_CASE("a program in an application bundle of macOS is told by the shape of its path") {
        const std::filesystem::path bundle = pathFromUtf8("games/Café Depot.app");
        CHECK(applicationBundleOf(bundle / "Contents" / "MacOS" / "Café Depot") == bundle);
        CHECK(applicationBundleOf(pathFromUtf8("/Applications/Depot.app/Contents/MacOS/Depot")) ==
              pathFromUtf8("/Applications/Depot.app"));
        // Anything else is no bundle: a program beside its game, one a level too deep or too
        // shallow, a directory that only looks like one, and nothing at all.
        CHECK_FALSE(applicationBundleOf(pathFromUtf8("games/Depot/Depot.exe")).has_value());
        CHECK_FALSE(applicationBundleOf(bundle / "Contents" / "MacOS" / "bin" / "Depot").has_value());
        CHECK_FALSE(applicationBundleOf(bundle / "Contents" / "Depot").has_value());
        CHECK_FALSE(applicationBundleOf(pathFromUtf8("games/Depot/Contents/MacOS/Depot")).has_value());
        CHECK_FALSE(applicationBundleOf(pathFromUtf8("games/.app/Contents/MacOS/Depot")).has_value());
        CHECK_FALSE(applicationBundleOf(bundle / "Contents" / "MacOS" / "").has_value());
        CHECK_FALSE(applicationBundleOf({}).has_value());
        // The test binary is in a build directory, not in a bundle.
        CHECK_FALSE(applicationBundleOf(executablePath()).has_value());
    }

    TEST_CASE("a library that a bundle brings is in its Frameworks, and nowhere else") {
        const test::TempDirectory directory;
        const std::filesystem::path bundle = directory.path() / "Depot.app";
        const std::filesystem::path program = bundle / "Contents" / "MacOS" / "Depot";
        const std::filesystem::path frameworks = bundle / "Contents" / "Frameworks";
        CHECK_FALSE(bundledLibrary(program, "libMoltenVK.dylib").has_value());
        REQUIRE(writeTextFile(frameworks / "libMoltenVK.dylib", "a library").has_value());
        CHECK(bundledLibrary(program, "libMoltenVK.dylib") == frameworks / "libMoltenVK.dylib");
        CHECK_FALSE(bundledLibrary(program, "libOther.dylib").has_value());
        CHECK_FALSE(bundledLibrary(program, "").has_value());
        // A directory of that name is no library.
        REQUIRE(ensureDirectory(frameworks / "libFolder.dylib").has_value());
        CHECK_FALSE(bundledLibrary(program, "libFolder.dylib").has_value());
        // A program in no bundle brings none, whatever is beside it.
        const std::filesystem::path plain = directory.path() / "Depot" / "Contents";
        REQUIRE(writeTextFile(plain / "Frameworks" / "libMoltenVK.dylib", "a library").has_value());
        CHECK_FALSE(bundledLibrary(plain / "MacOS" / "Depot", "libMoltenVK.dylib").has_value());
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

    TEST_CASE("an error dialog asks nobody once dialogs are suppressed") {
        suppressErrorDialogs();
        // A dialog would wait here until the suite is stopped for taking too long.
        CHECK_NOTHROW(showErrorDialog("Cordite tests", "Nobody is there to close this. \xC3\xA9"));
    }

    TEST_CASE("a program that writes to pipes goes on writing to them after looking for a console") {
        // The tests write to the pipes of the test runner, or to the console they have: both stay.
        attachToParentConsole();
        attachToParentConsole();
        CHECK(std::fputs("", stdout) >= 0);
        CHECK(std::fflush(stdout) == 0);
        CHECK(std::fflush(stderr) == 0);
        CHECK(std::ferror(stdout) == 0);
    }
}
