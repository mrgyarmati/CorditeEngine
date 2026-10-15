#include "cordite/editor/FileAssociation.h"

#include "cordite/core/Base.h"
#include "cordite/core/Uuid.h"

#include "support/TestSupport.h"

#include <optional>
#include <string>

#if CORDITE_PLATFORM_WINDOWS
#include <windows.h>
#endif

using namespace cordite;

namespace {

FileAssociation exampleAssociation(std::string program = "C:/Games/Cordite/cordite-editor.exe") {
    return {.extension = ".cordtest",
            .programId = "Cordite.Test",
            .description = "Cordite test file",
            .program = std::move(program)};
}

/// The code of a failure, or none for a success - whose error a test must not take.
template <typename T>
std::optional<ErrorCode> failureOf(const Result<T>& result) {
    return result ? std::nullopt : std::optional<ErrorCode>(result.error().code);
}

#if CORDITE_PLATFORM_WINDOWS

std::wstring wideOf(const std::string& text) {
    return {text.begin(), text.end()};
}

/// Classes of a test's own, under HKEY_CURRENT_USER\Software\CorditeTests, deleted when the
/// test ends: no test touches the classes of the person who runs it.
class TestClasses {
public:
    TestClasses() : m_root("Software\\CorditeTests\\" + Uuid::generate().toString()) {}

    ~TestClasses() {
        RegDeleteTreeW(HKEY_CURRENT_USER, wideOf(m_root).c_str());
        // Gone once no test has a key under it; refused while one has.
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\CorditeTests");
    }

    TestClasses(const TestClasses&) = delete;
    TestClasses& operator=(const TestClasses&) = delete;

    [[nodiscard]] std::string classes() const { return m_root + "\\Classes"; }

    /// Whether a key exists under the classes.
    [[nodiscard]] bool has(const std::string& key) const {
        HKEY handle = nullptr;
        const LSTATUS status =
            RegOpenKeyExW(HKEY_CURRENT_USER, wideOf(classes() + "\\" + key).c_str(), 0, KEY_READ, &handle);
        if (status == ERROR_SUCCESS) {
            RegCloseKey(handle);
        }
        return status == ERROR_SUCCESS;
    }

private:
    std::string m_root;
};

#endif

} // namespace

TEST_SUITE("FileAssociation") {
    TEST_CASE("project files are opened by the editor that associates them") {
        const FileAssociation projects = projectFileAssociation("C:/Cordite/cordite-editor.exe");
        CHECK(projects.extension == ".cordite");
        CHECK(projects.programId == "Cordite.Project");
        CHECK(projects.description == "Cordite project");
        CHECK(projects.program == std::filesystem::path("C:/Cordite/cordite-editor.exe"));
    }

    TEST_CASE("a file is given to its program in quotes, as is the program") {
        const std::string command = openCommand("C:/Program Files/Cordite/cordite-editor.exe");
        CHECK(command.starts_with('"'));
        CHECK(command.ends_with("\" \"%1\""));
#if CORDITE_PLATFORM_WINDOWS
        // The system reads the path of the program the way of Windows.
        CHECK(command == "\"C:\\Program Files\\Cordite\\cordite-editor.exe\" \"%1\"");
#endif
    }

    TEST_CASE("what is no type of file is refused before anything is changed") {
        FileAssociation noDot = exampleAssociation();
        noDot.extension = "cordite";
        FileAssociation dotAlone = exampleAssociation();
        dotAlone.extension = ".";
        FileAssociation nested = exampleAssociation();
        nested.extension = ".a\\b";
        FileAssociation noId = exampleAssociation();
        noId.programId.clear();
        FileAssociation noProgram = exampleAssociation();
        noProgram.program.clear();
#if CORDITE_PLATFORM_WINDOWS
        // Should a check be missing, what it lets through goes to keys of the test's own.
        const TestClasses registry;
        const std::string classes = registry.classes();
#else
        const std::string classes = "Software\\CorditeTests\\never";
#endif
        for (const FileAssociation& wrong : {noDot, dotAlone, nested, noId, noProgram}) {
            CAPTURE(wrong.extension);
            CHECK(failureOf(isAssociated(wrong, classes)) == ErrorCode::InvalidArgument);
            CHECK(failureOf(associate(wrong, classes)) == ErrorCode::InvalidArgument);
            CHECK(failureOf(dissociate(wrong, classes)) == ErrorCode::InvalidArgument);
        }
    }

#if CORDITE_PLATFORM_WINDOWS
    TEST_CASE("a type is associated with its program, and is no more once dissociated") {
        const TestClasses registry;
        const FileAssociation association = exampleAssociation();
        CHECK_FALSE(test::unwrap(isAssociated(association, registry.classes())));

        REQUIRE(associate(association, registry.classes()).has_value());
        CHECK(test::unwrap(isAssociated(association, registry.classes())));
        CHECK(registry.has(".cordtest"));
        CHECK(registry.has("Cordite.Test\\shell\\open\\command"));
        // Associating again changes nothing.
        REQUIRE(associate(association, registry.classes()).has_value());
        CHECK(test::unwrap(isAssociated(association, registry.classes())));

        REQUIRE(dissociate(association, registry.classes()).has_value());
        CHECK_FALSE(test::unwrap(isAssociated(association, registry.classes())));
        CHECK_FALSE(registry.has(".cordtest"));
        CHECK_FALSE(registry.has("Cordite.Test"));
        // There is nothing left to undo, which is no failure.
        CHECK(dissociate(association, registry.classes()).has_value());
    }

    TEST_CASE("files of a type open with the program that associated them last") {
        const TestClasses registry;
        const FileAssociation here = exampleAssociation("C:/Builds/Debug/cordite-editor.exe");
        const FileAssociation there = exampleAssociation("D:/Builds/Release/cordite-editor.exe");
        REQUIRE(associate(here, registry.classes()).has_value());
        // An editor that was moved, or another build of it, is not the one files open with.
        CHECK_FALSE(test::unwrap(isAssociated(there, registry.classes())));
        REQUIRE(associate(there, registry.classes()).has_value());
        CHECK(test::unwrap(isAssociated(there, registry.classes())));
        CHECK_FALSE(test::unwrap(isAssociated(here, registry.classes())));
    }

    TEST_CASE("an extension that names the type of another program is left to it") {
        const TestClasses registry;
        const FileAssociation ours = exampleAssociation();
        FileAssociation theirs = exampleAssociation("C:/Other/viewer.exe");
        theirs.programId = "Someone.Else";
        REQUIRE(associate(theirs, registry.classes()).has_value());
        CHECK_FALSE(test::unwrap(isAssociated(ours, registry.classes())));

        // Ours, registered and then left by the extension: the extension says what opens a
        // file, whatever else is registered.
        REQUIRE(associate(ours, registry.classes()).has_value());
        REQUIRE(associate(theirs, registry.classes()).has_value());
        CHECK(registry.has("Cordite.Test\\shell\\open\\command"));
        CHECK_FALSE(test::unwrap(isAssociated(ours, registry.classes())));

        // Undoing ours does not undo theirs.
        REQUIRE(dissociate(ours, registry.classes()).has_value());
        CHECK(test::unwrap(isAssociated(theirs, registry.classes())));
        CHECK(registry.has(".cordtest"));
    }
#else
    TEST_CASE("files are associated with programs only on Windows") {
        CHECK(failureOf(isAssociated(exampleAssociation())) == ErrorCode::Unsupported);
        CHECK(failureOf(associate(exampleAssociation())) == ErrorCode::Unsupported);
        CHECK(failureOf(dissociate(exampleAssociation())) == ErrorCode::Unsupported);
    }
#endif
}
