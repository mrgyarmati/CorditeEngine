#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;

TEST_SUITE("FileSystem") {
    TEST_CASE("text files round-trip, including empty ones") {
        TempDirectory directory;

        const std::filesystem::path file = directory.path() / "note.txt";
        REQUIRE(writeTextFile(file, "line one\nline two\n").has_value());
        const Result<std::string> text = readTextFile(file);
        REQUIRE(text.has_value());
        CHECK(*text == "line one\nline two\n");

        const std::filesystem::path empty = directory.path() / "empty.txt";
        REQUIRE(writeTextFile(empty, "").has_value());
        const Result<std::string> emptyText = readTextFile(empty);
        REQUIRE(emptyText.has_value());
        CHECK(emptyText->empty());
    }

    TEST_CASE("binary files round-trip without newline translation") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "data.bin";

        std::vector<std::byte> data;
        for (int value = 0; value < 256; ++value) {
            data.push_back(static_cast<std::byte>(value));
        }
        data.push_back(std::byte{'\r'});
        data.push_back(std::byte{'\n'});

        REQUIRE(writeBinaryFile(file, data).has_value());
        const Result<std::vector<std::byte>> read = readBinaryFile(file);
        REQUIRE(read.has_value());
        CHECK(*read == data);
    }

    TEST_CASE("writing creates missing parent directories") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "a" / "b" / "c.txt";
        REQUIRE(writeTextFile(file, "nested").has_value());
        CHECK(fileExists(file));
        CHECK(directoryExists(directory.path() / "a" / "b"));
    }

    TEST_CASE("writing replaces an existing file and leaves no temporary files") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "settings.json";

        REQUIRE(writeTextFile(file, "first version, quite long").has_value());
        REQUIRE(writeTextFile(file, "second").has_value());

        const Result<std::string> text = readTextFile(file);
        REQUIRE(text.has_value());
        CHECK(*text == "second");

        size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
            (void)entry;
            ++entries;
        }
        CHECK(entries == 1);
    }

    TEST_CASE("reading a missing file reports NotFound with the path") {
        TempDirectory directory;
        const Result<std::string> result = readTextFile(directory.path() / "missing.txt");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::NotFound);
        CHECK(result.error().message.find("missing.txt") != std::string::npos);
    }

    TEST_CASE("reading a directory fails cleanly instead of treating it as a file") {
        TempDirectory directory;
        const Result<std::vector<std::byte>> result = readBinaryFile(directory.path());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::IoError);
        CHECK(result.error().message.find("not a regular file") != std::string::npos);
    }

    TEST_CASE("concurrent writers to one file never corrupt it") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "shared.txt";

        constexpr int kWriters = 8;
        std::vector<std::thread> writers;
        for (int writer = 0; writer < kWriters; ++writer) {
            writers.emplace_back([&file, writer] {
                const std::string content(4096, static_cast<char>('a' + writer));
                for (int iteration = 0; iteration < 20; ++iteration) {
                    // A write may lose the race to replace the file; it must not tear it.
                    const Status status = writeTextFile(file, content);
                    (void)status;
                }
            });
        }
        for (std::thread& writer : writers) {
            writer.join();
        }

        const Result<std::string> text = readTextFile(file);
        REQUIRE(text.has_value());
        REQUIRE(text->size() == 4096);
        CHECK(text->find_first_not_of(text->front()) == std::string::npos);

        size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
            (void)entry;
            ++entries;
        }
        CHECK(entries == 1);
    }

    TEST_CASE("writing where a directory already exists fails") {
        TempDirectory directory;
        const std::filesystem::path occupied = directory.path() / "occupied";
        REQUIRE(ensureDirectory(occupied).has_value());

        const Status status = writeTextFile(occupied, "cannot replace a directory");
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::IoError);
        CHECK(directoryExists(occupied));
    }

    TEST_CASE("ensureDirectory is idempotent and rejects files") {
        TempDirectory directory;
        const std::filesystem::path nested = directory.path() / "x" / "y";
        CHECK(ensureDirectory(nested).has_value());
        CHECK(ensureDirectory(nested).has_value());

        const std::filesystem::path file = directory.path() / "file";
        REQUIRE(writeTextFile(file, "data").has_value());
        CHECK_FALSE(ensureDirectory(file).has_value());
    }

    TEST_CASE("fileExists and directoryExists distinguish the two") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / "file.txt";
        REQUIRE(writeTextFile(file, "x").has_value());

        CHECK(fileExists(file));
        CHECK_FALSE(directoryExists(file));
        CHECK(directoryExists(directory.path()));
        CHECK_FALSE(fileExists(directory.path()));
        CHECK_FALSE(fileExists(directory.path() / "nope"));
    }

    TEST_CASE("paths convert to and from UTF-8 with forward slashes") {
        const std::filesystem::path path = pathFromUtf8("assets/m\xc3\xa9tal/t\xc3\xb4le.png");
        CHECK(pathToUtf8(path) == "assets/m\xc3\xa9tal/t\xc3\xb4le.png");

        const std::filesystem::path native = std::filesystem::path("assets") / "textures" / "wall.png";
        CHECK(pathToUtf8(native) == "assets/textures/wall.png");
    }

    TEST_CASE("files with non-ASCII names can be written and read") {
        TempDirectory directory;
        const std::filesystem::path file = directory.path() / pathFromUtf8("\xe9\x87\x91\xe5\xb1\x9e.txt");
        REQUIRE(writeTextFile(file, "metal").has_value());
        const Result<std::string> text = readTextFile(file);
        REQUIRE(text.has_value());
        CHECK(*text == "metal");
    }

    TEST_CASE("relativePathWithin accepts paths inside the base and rejects escapes") {
        TempDirectory directory;
        const std::filesystem::path base = directory.path() / "project";
        REQUIRE(ensureDirectory(base / "assets").has_value());

        const Result<std::string> inside = relativePathWithin(base, base / "assets" / "rock.png");
        REQUIRE(inside.has_value());
        CHECK(*inside == "assets/rock.png");

        const Result<std::string> dotted = relativePathWithin(base, base / "assets" / ".." / "assets" / "rock.png");
        REQUIRE(dotted.has_value());
        CHECK(*dotted == "assets/rock.png");

        const Result<std::string> escape = relativePathWithin(base, base / ".." / "secret.txt");
        REQUIRE_FALSE(escape.has_value());
        CHECK(escape.error().code == ErrorCode::InvalidArgument);

        CHECK_FALSE(relativePathWithin(base, base).has_value());
        CHECK_FALSE(relativePathWithin(base / "assets", base / "assets-other" / "x.png").has_value());
    }
}
