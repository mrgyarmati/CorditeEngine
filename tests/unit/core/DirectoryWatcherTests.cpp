#include "cordite/core/DirectoryWatcher.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;

namespace {

using Paths = std::vector<std::string>;

/// What a watcher gathers within a generous time: it looks on a thread of its own.
Paths changesWithin(DirectoryWatcher& watcher, std::chrono::milliseconds limit = std::chrono::seconds(10)) {
    const auto end = std::chrono::steady_clock::now() + limit;
    Paths changes;
    while (changes.empty() && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        changes = watcher.takeChanges();
    }
    return changes;
}

} // namespace

TEST_SUITE("DirectoryWatcher") {
    TEST_CASE("the files of a directory are stamped by their paths, but for hidden directories") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "a.txt", "one").has_value());
        REQUIRE(writeTextFile(directory.path() / "scripts" / "b.lua", "return {}").has_value());
        REQUIRE(writeTextFile(directory.path() / ".cache" / "c.bin", "not counted").has_value());
        REQUIRE(writeTextFile(directory.path() / "scripts" / ".notes" / "d.txt", "not counted").has_value());
        REQUIRE(writeTextFile(directory.path() / ".hidden-file", "a file, and counted").has_value());
        // What an atomic write leaves for a moment before it renames it.
        REQUIRE(writeTextFile(directory.path() / "scripts" / "b.lua.0aedd5c8.tmp", "not counted").has_value());
        REQUIRE(ensureDirectory(directory.path() / "empty").has_value());

        const auto files = stampFiles(directory.path());
        Paths paths;
        for (const auto& [path, stamp] : files) {
            paths.push_back(path);
        }
        CHECK(paths == Paths{".hidden-file", "a.txt", "scripts/b.lua"});
        CHECK(files.at("a.txt").size == 3);
        CHECK(files.at("scripts/b.lua").size == 9);
        CHECK(stampFiles(directory.path() / "nowhere").empty());
    }

    TEST_CASE("what was added, removed and written is found between two lists of files") {
        const auto time = std::filesystem::file_time_type::clock::now();
        const std::map<std::string, FileStamp> before = {
            {"kept", {.size = 1, .written = time}},
            {"removed", {.size = 2, .written = time}},
            {"resized", {.size = 3, .written = time}},
            {"touched", {.size = 4, .written = time}},
        };
        const std::map<std::string, FileStamp> after = {
            {"added", {.size = 5, .written = time}},
            {"kept", {.size = 1, .written = time}},
            {"resized", {.size = 30, .written = time}},
            {"touched", {.size = 4, .written = time + std::chrono::seconds(1)}},
            {"zz-added", {.size = 6, .written = time}},
        };
        CHECK(changedFiles(before, after) == Paths{"added", "removed", "resized", "touched", "zz-added"});
        CHECK(changedFiles(after, before) == Paths{"added", "removed", "resized", "touched", "zz-added"});
        CHECK(changedFiles(before, before).empty());
        CHECK(changedFiles({}, {}).empty());
        CHECK(changedFiles({}, before) == Paths{"kept", "removed", "resized", "touched"});
    }

    TEST_CASE("a watcher gathers what changes until it is asked, and forgets it after a new baseline") {
        const TempDirectory directory;
        REQUIRE(writeTextFile(directory.path() / "kept.txt", "kept").has_value());
        REQUIRE(writeTextFile(directory.path() / "removed.txt", "removed").has_value());
        DirectoryWatcher watcher({.directory = directory.path(), .interval = std::chrono::milliseconds(10)});
        CHECK(watcher.takeChanges().empty());

        REQUIRE(writeTextFile(directory.path() / "scripts" / "added.lua", "return {}").has_value());
        std::filesystem::remove(directory.path() / "removed.txt");
        Paths changes = changesWithin(watcher);
        // Both may be seen in one look or in two.
        if (changes.size() == 1) {
            const Paths more = changesWithin(watcher);
            changes.insert(changes.end(), more.begin(), more.end());
            std::ranges::sort(changes);
        }
        CHECK(changes == Paths{"removed.txt", "scripts/added.lua"});
        CHECK(watcher.takeChanges().empty());

        // A file that is written again, with another size.
        REQUIRE(writeTextFile(directory.path() / "kept.txt", "written again").has_value());
        CHECK(changesWithin(watcher) == Paths{"kept.txt"});

        // What the program wrote itself is no change, once it says so.
        REQUIRE(writeTextFile(directory.path() / "own.txt", "the program's").has_value());
        watcher.rebaseline();
        CHECK(watcher.takeChanges().empty());
        CHECK(changesWithin(watcher, std::chrono::seconds(1)).empty());
    }

    TEST_CASE("a file on its way to being written is no change, and the file it becomes is") {
        const TempDirectory directory;
        DirectoryWatcher watcher({.directory = directory.path(), .interval = std::chrono::milliseconds(10)});
        CHECK(watcher.takeChanges().empty());
        const std::filesystem::path temporary = directory.path() / "kept.txt.0aedd5c8.tmp";
        REQUIRE(writeTextFile(temporary, "on its way").has_value());
        CHECK(changesWithin(watcher, std::chrono::milliseconds(300)).empty());
        std::filesystem::rename(temporary, directory.path() / "kept.txt");
        CHECK(changesWithin(watcher) == Paths{"kept.txt"});
    }

    TEST_CASE("a watcher of a directory that is not there sees it come") {
        const TempDirectory directory;
        DirectoryWatcher watcher({.directory = directory.path() / "assets", .interval = std::chrono::milliseconds(10)});
        REQUIRE(writeTextFile(directory.path() / "assets" / "new.txt", "new").has_value());
        CHECK(changesWithin(watcher) == Paths{"new.txt"});
    }
}
