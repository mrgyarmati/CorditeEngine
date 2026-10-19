#include "cordite/shaders/ShaderCache.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"
#include "unit/shaders/ShaderTestSupport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;

namespace {

ShaderBinary makeBinary() {
    ShaderBinary binary;
    binary.stage = ShaderStage::Pixel;
    binary.entryPoint = "pixelMain";
    binary.compilerVersion = "test-compiler 1.0";
    binary.code = test::makeFakeSpirv({10, 20, 30, 40});
    binary.dependencies = {
        {"common/Lighting.slang", Hash128{1, 2}},
        {"passes/GBuffer.slang", Hash128{0xffffffffffffffffull, 0x8000000000000000ull}},
    };
    return binary;
}

const Hash128 kKey{0x1122334455667788ull, 0x99aabbccddeeff00ull};

size_t countFiles(const std::filesystem::path& directory) {
    size_t count = 0;
    if (std::filesystem::is_directory(directory)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            (void)entry;
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_SUITE("ShaderCache") {
    TEST_CASE("the file format round-trips every field") {
        const ShaderBinary original = makeBinary();
        const Result<ShaderBinary> decoded = decodeShaderBinary(encodeShaderBinary(original));
        REQUIRE_MESSAGE(decoded.has_value(), test::errorOf(decoded));
        CHECK(*decoded == original);
    }

    TEST_CASE("a binary without dependencies or with empty strings round-trips") {
        ShaderBinary original;
        original.stage = ShaderStage::Compute;
        original.code = test::makeFakeSpirv({});
        const Result<ShaderBinary> decoded = decodeShaderBinary(encodeShaderBinary(original));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == original);
    }

    TEST_CASE("stored entries can be loaded again") {
        TempDirectory directory;
        const ShaderCache cache(directory.path() / "shaders");
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());

        REQUIRE(cache.store(kKey, makeBinary()).has_value());
        const std::optional<ShaderBinary> loaded = cache.load(kKey, ShaderTarget::Spirv);
        REQUIRE(loaded.has_value());
        CHECK(*loaded == makeBinary());

        // A different key is a different entry.
        CHECK_FALSE(cache.load(Hash128{1, 1}, ShaderTarget::Spirv).has_value());
        CHECK(cache.entryPath(kKey, ShaderTarget::Spirv).filename() == "112233445566778899aabbccddeeff00.cshader");
    }

    TEST_CASE("storing replaces an existing entry") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());

        ShaderBinary updated = makeBinary();
        updated.compilerVersion = "test-compiler 2.0";
        REQUIRE(cache.store(kKey, makeBinary()).has_value());
        REQUIRE(cache.store(kKey, updated).has_value());

        CHECK(cache.load(kKey, ShaderTarget::Spirv)->compilerVersion == "test-compiler 2.0");
        CHECK(countFiles(directory.path()) == 1);
    }

    TEST_CASE("a shader that is not SPIR-V is not stored") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());

        ShaderBinary invalid = makeBinary();
        invalid.code = std::vector<std::byte>(16);
        const Status status = cache.store(kKey, invalid);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::InvalidArgument);
        CHECK(countFiles(directory.path()) == 0);
    }

    TEST_CASE("a corrupt entry is discarded with a warning") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());
        REQUIRE(writeTextFile(cache.entryPath(kKey, ShaderTarget::Spirv), "this is not a compiled shader").has_value());

        LogCapture capture(LogLevel::Warn);
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());
        CHECK(capture.count(LogLevel::Warn) == 1);
        CHECK_FALSE(fileExists(cache.entryPath(kKey, ShaderTarget::Spirv)));
    }

    TEST_CASE("an entry from another format version is discarded quietly") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());

        std::vector<std::byte> data = encodeShaderBinary(makeBinary());
        const uint32_t futureVersion = 9999;
        std::memcpy(data.data() + sizeof(uint32_t), &futureVersion, sizeof(futureVersion));
        REQUIRE(writeBinaryFile(cache.entryPath(kKey, ShaderTarget::Spirv), data).has_value());

        const Result<ShaderBinary> decoded = decodeShaderBinary(data);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ErrorCode::VersionMismatch);

        LogCapture capture(LogLevel::Warn);
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());
        CHECK(capture.messages().empty());
        CHECK_FALSE(fileExists(cache.entryPath(kKey, ShaderTarget::Spirv)));
    }

    TEST_CASE("truncated data is rejected at every length") {
        const std::vector<std::byte> data = encodeShaderBinary(makeBinary());
        for (size_t length = 0; length < data.size(); ++length) {
            const Result<ShaderBinary> decoded = decodeShaderBinary(std::span<const std::byte>(data.data(), length));
            REQUIRE_FALSE(decoded.has_value());
        }
        CHECK(decodeShaderBinary(data).has_value());
    }

    TEST_CASE("trailing bytes after a valid entry are rejected") {
        std::vector<std::byte> data = encodeShaderBinary(makeBinary());
        data.push_back(std::byte{0});
        const Result<ShaderBinary> decoded = decodeShaderBinary(data);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ErrorCode::ParseError);
    }

    TEST_CASE("corrupt fields do not cause large allocations or crashes") {
        const std::vector<std::byte> valid = encodeShaderBinary(makeBinary());

        // Flip each byte of the header and the lengths in turn; the decoder must either
        // reject the data or return something that still satisfies its own invariants.
        for (size_t offset = 0; offset < std::min<size_t>(valid.size(), 96); ++offset) {
            std::vector<std::byte> corrupt = valid;
            corrupt[offset] = ~corrupt[offset];
            const Result<ShaderBinary> decoded = decodeShaderBinary(corrupt);
            if (decoded) {
                CHECK(isSpirv(decoded->code));
                CHECK(decoded->dependencies.size() <= 4096);
            }
        }
    }

    TEST_CASE("clear removes cache entries and nothing else") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());
        REQUIRE(cache.store(kKey, makeBinary()).has_value());
        REQUIRE(cache.store(Hash128{5, 6}, makeBinary()).has_value());
        REQUIRE(writeTextFile(directory.path() / "notes.txt", "keep me").has_value());

        REQUIRE(cache.clear().has_value());
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());
        CHECK(countFiles(directory.path()) == 1);
        CHECK(fileExists(directory.path() / "notes.txt"));
    }

    TEST_CASE("clearing a cache that was never written succeeds") {
        TempDirectory directory;
        const ShaderCache cache(directory.path() / "never-created");
        CHECK(cache.clear().has_value());
    }

    TEST_CASE("DXIL is kept beside SPIR-V, under a name of its own, and found only for its target") {
        TempDirectory directory;
        const ShaderCache cache(directory.path());
        ShaderBinary dxil = makeBinary();
        dxil.target = ShaderTarget::Dxil;
        dxil.code = test::makeFakeDxil({7, 8});

        REQUIRE(cache.store(kKey, dxil).has_value());
        CHECK(cache.entryPath(kKey, ShaderTarget::Dxil).filename() == "112233445566778899aabbccddeeff00.dxil.cshader");
        CHECK(fileExists(cache.entryPath(kKey, ShaderTarget::Dxil)));
        // The SPIR-V of the key is another entry, which is not there.
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());
        const std::optional<ShaderBinary> loaded = cache.load(kKey, ShaderTarget::Dxil);
        REQUIRE(loaded.has_value());
        CHECK(*loaded == dxil);

        // Both side by side, each for its own; and a clear takes both.
        REQUIRE(cache.store(kKey, makeBinary()).has_value());
        CHECK(cache.load(kKey, ShaderTarget::Spirv)->target == ShaderTarget::Spirv);
        CHECK(cache.load(kKey, ShaderTarget::Dxil)->target == ShaderTarget::Dxil);
        CHECK(countFiles(directory.path()) == 2);
        REQUIRE(cache.clear().has_value());
        CHECK(countFiles(directory.path()) == 0);
    }

    TEST_CASE("the target is what the code says it is, and code of another target is discarded") {
        // The format has no field for it: the caches of SPIR-V made before stay readable.
        ShaderBinary dxil = makeBinary();
        dxil.target = ShaderTarget::Dxil;
        dxil.code = test::makeFakeDxil();
        std::vector<std::byte> data = encodeShaderBinary(dxil);
        CHECK(data.size() == encodeShaderBinary(makeBinary()).size() - makeBinary().code.size() + dxil.code.size());
        const ShaderBinary decoded = test::unwrap(decodeShaderBinary(data));
        CHECK(decoded.target == ShaderTarget::Dxil);
        CHECK(decoded == dxil);

        // DXIL where SPIR-V is looked for was not written by a cache: it goes.
        TempDirectory directory;
        const ShaderCache cache(directory.path());
        REQUIRE(writeBinaryFile(cache.entryPath(kKey, ShaderTarget::Spirv), data).has_value());
        const LogCapture log(LogLevel::Warn);
        CHECK_FALSE(cache.load(kKey, ShaderTarget::Spirv).has_value());
        CHECK(log.contains("holds dxil, not spirv"));
        CHECK_FALSE(fileExists(cache.entryPath(kKey, ShaderTarget::Spirv)));

        // DXIL that is not signed is no code of any target, and is not stored.
        ShaderBinary unsignedDxil = dxil;
        std::fill_n(unsignedDxil.code.begin() + 4, 16, std::byte{0});
        const Status stored = cache.store(kKey, unsignedDxil);
        REQUIRE_FALSE(stored.has_value());
        CHECK(stored.error().code == ErrorCode::InvalidArgument);
        const Result<ShaderBinary> read = decodeShaderBinary(encodeShaderBinary(unsignedDxil));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().code == ErrorCode::ParseError);
    }
}
