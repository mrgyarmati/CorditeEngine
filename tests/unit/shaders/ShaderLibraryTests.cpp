#include "cordite/shaders/ShaderLibrary.h"

#include "cordite/core/FileSystem.h"

#include "support/TestSupport.h"
#include "unit/shaders/ShaderTestSupport.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::FakeShaderCompiler;
using cordite::test::TempDirectory;

namespace {

/// A source tree with one shader, a cache directory, and a fake compiler over them.
struct Fixture {
    Fixture() : sourceRoot(directory.path() / "shaders"), cacheRoot(directory.path() / "cache"), compiler(sourceRoot) {
        writeSource("passes/Blit.slang", "// version 1");
    }

    void writeSource(std::string_view path, std::string_view content) const {
        REQUIRE(writeTextFile(sourceRoot / pathFromUtf8(path), content).has_value());
    }

    [[nodiscard]] ShaderLibraryDesc desc() {
        ShaderLibraryDesc result;
        result.sourceRoots = {sourceRoot};
        result.cacheDirectory = cacheRoot;
        result.compiler = &compiler;
        return result;
    }

    [[nodiscard]] static ShaderRequest request() {
        ShaderRequest result;
        result.path = "passes/Blit.slang";
        result.entryPoint = "blitPixel";
        result.stage = ShaderStage::Pixel;
        return result;
    }

    TempDirectory directory;
    std::filesystem::path sourceRoot;
    std::filesystem::path cacheRoot;
    FakeShaderCompiler compiler;
};

} // namespace

TEST_SUITE("ShaderLibrary") {
    TEST_CASE("the first load compiles and later loads come from memory") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());

        const auto first = library.load(Fixture::request());
        REQUIRE_MESSAGE(first.has_value(), test::errorOf(first));
        CHECK(isSpirv((*first)->code));
        CHECK((*first)->entryPoint == "blitPixel");
        CHECK(fixture.compiler.compileCount == 1);

        const auto second = library.load(Fixture::request());
        REQUIRE(second.has_value());
        CHECK(second->get() == first->get());
        CHECK(fixture.compiler.compileCount == 1);

        const ShaderLibraryStats stats = library.stats();
        CHECK(stats.compilations == 1);
        CHECK(stats.memoryHits == 1);
        CHECK(stats.diskHits == 0);
        CHECK(stats.failures == 0);
    }

    TEST_CASE("each permutation is compiled separately") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());

        ShaderRequest plain = Fixture::request();
        ShaderRequest withDefine = Fixture::request();
        withDefine.defines = {{"FLIP", "1"}};

        const auto first = library.load(plain);
        const auto second = library.load(withDefine);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->get() != second->get());
        CHECK((*first)->code != (*second)->code);
        CHECK(fixture.compiler.compileCount == 2);
    }

    TEST_CASE("a new library instance reuses the disk cache") {
        Fixture fixture;
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }
        CHECK(fixture.compiler.compileCount == 1);

        ShaderLibrary library(fixture.desc());
        REQUIRE(library.load(Fixture::request()).has_value());
        CHECK(fixture.compiler.compileCount == 1);
        CHECK(library.stats().diskHits == 1);
        CHECK(library.stats().compilations == 0);
    }

    TEST_CASE("a changed source file invalidates the disk cache") {
        Fixture fixture;
        std::vector<std::byte> originalCode;
        {
            ShaderLibrary library(fixture.desc());
            originalCode = (*library.load(Fixture::request()))->code;
        }

        fixture.writeSource("passes/Blit.slang", "// version 2");
        ShaderLibrary library(fixture.desc());
        const auto reloaded = library.load(Fixture::request());
        REQUIRE(reloaded.has_value());
        CHECK(fixture.compiler.compileCount == 2);
        CHECK((*reloaded)->code != originalCode);
        CHECK(library.stats().diskHits == 0);
    }

    TEST_CASE("a compiler upgrade invalidates the disk cache") {
        Fixture fixture;
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }

        fixture.compiler.compilerVersion = "fake-2";
        ShaderLibrary library(fixture.desc());
        const auto reloaded = library.load(Fixture::request());
        REQUIRE(reloaded.has_value());
        CHECK(fixture.compiler.compileCount == 2);
        CHECK((*reloaded)->compilerVersion == "fake-2");
    }

    TEST_CASE("a deleted source file invalidates the disk cache") {
        Fixture fixture;
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }
        std::filesystem::remove(fixture.sourceRoot / "passes" / "Blit.slang");

        ShaderLibrary library(fixture.desc());
        const auto reloaded = library.load(Fixture::request());
        REQUIRE_FALSE(reloaded.has_value());
        CHECK(library.stats().failures == 1);
    }

    TEST_CASE("without a cache directory nothing is written to disk") {
        Fixture fixture;
        ShaderLibraryDesc desc = fixture.desc();
        desc.cacheDirectory.clear();
        ShaderLibrary library(desc);

        REQUIRE(library.load(Fixture::request()).has_value());
        CHECK_FALSE(directoryExists(fixture.cacheRoot));
    }

    TEST_CASE("cache-only mode serves trusted entries and reports missing ones") {
        Fixture fixture;
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }

        // An exported game: the cache, no compiler and no sources.
        ShaderLibraryDesc shipped;
        shipped.cacheDirectory = fixture.cacheRoot;
        shipped.trustCache = true;
        ShaderLibrary library(shipped);

        const auto cached = library.load(Fixture::request());
        REQUIRE_MESSAGE(cached.has_value(), test::errorOf(cached));
        CHECK(library.stats().diskHits == 1);

        ShaderRequest missing = Fixture::request();
        missing.defines = {{"NOT_EXPORTED", "1"}};
        const auto absent = library.load(missing);
        REQUIRE_FALSE(absent.has_value());
        CHECK(absent.error().code == ErrorCode::NotFound);
        CHECK(absent.error().message.find("NOT_EXPORTED") != std::string::npos);
        CHECK(library.stats().failures == 1);
    }

    TEST_CASE("without trustCache a cache-only library cannot verify entries") {
        Fixture fixture;
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }

        ShaderLibraryDesc unverifiable;
        unverifiable.cacheDirectory = fixture.cacheRoot;
        ShaderLibrary library(unverifiable);
        CHECK_FALSE(library.load(Fixture::request()).has_value());
    }

    TEST_CASE("a compile failure carries the request and the diagnostics") {
        Fixture fixture;
        fixture.compiler.failWith = Error{ErrorCode::ParseError, "Blit.slang(3): error: unexpected '}'"};
        ShaderLibrary library(fixture.desc());

        const auto result = library.load(Fixture::request());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::ParseError);
        CHECK(result.error().message.find("passes/Blit.slang:blitPixel") != std::string::npos);
        CHECK(result.error().message.find("unexpected '}'") != std::string::npos);
        CHECK(library.stats().failures == 1);

        // A failure is not cached: fixing the shader and asking again works.
        fixture.compiler.failWith.reset();
        CHECK(library.load(Fixture::request()).has_value());
    }

    TEST_CASE("an invalid request is rejected before reaching the compiler") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());

        ShaderRequest escaping = Fixture::request();
        escaping.path = "../outside.slang";
        const auto result = library.load(escaping);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::InvalidArgument);
        CHECK(fixture.compiler.compileCount == 0);
    }

    TEST_CASE("invalidateChanged drops exactly the shaders whose sources changed") {
        Fixture fixture;
        fixture.writeSource("passes/Other.slang", "// other");
        ShaderLibrary library(fixture.desc());

        ShaderRequest other = Fixture::request();
        other.path = "passes/Other.slang";
        REQUIRE(library.load(Fixture::request()).has_value());
        REQUIRE(library.load(other).has_value());
        CHECK(library.invalidateChanged().empty());

        fixture.writeSource("passes/Blit.slang", "// edited");
        const std::vector<ShaderRequest> invalidated = library.invalidateChanged();
        REQUIRE(invalidated.size() == 1);
        CHECK(invalidated[0].path == "passes/Blit.slang");

        // The edited shader is rebuilt; the untouched one is still in memory.
        REQUIRE(library.load(Fixture::request()).has_value());
        REQUIRE(library.load(other).has_value());
        CHECK(fixture.compiler.compileCount == 3);
    }

    TEST_CASE("clearMemory forces the next load back to the disk cache") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());
        REQUIRE(library.load(Fixture::request()).has_value());

        library.clearMemory();
        REQUIRE(library.load(Fixture::request()).has_value());
        CHECK(fixture.compiler.compileCount == 1);
        CHECK(library.stats().diskHits == 1);
    }

    TEST_CASE("sources are found in the first root that has them") {
        Fixture fixture;
        const std::filesystem::path overrideRoot = fixture.directory.path() / "project-shaders";
        REQUIRE(writeTextFile(overrideRoot / "passes" / "Blit.slang", "// project override").has_value());

        // The fake compiler reads from the engine root, so a cache entry made from it is
        // stale as soon as an earlier root provides a different file of the same name.
        {
            ShaderLibrary library(fixture.desc());
            REQUIRE(library.load(Fixture::request()).has_value());
        }
        ShaderLibraryDesc desc = fixture.desc();
        desc.sourceRoots = {overrideRoot, fixture.sourceRoot};
        ShaderLibrary library(desc);
        REQUIRE(library.load(Fixture::request()).has_value());
        CHECK(library.stats().diskHits == 0);
        CHECK(library.sourceRoots().size() == 2);
    }

    TEST_CASE("concurrent loads of one shader compile it once") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());

        constexpr int kThreads = 8;
        std::atomic<int> successes{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < kThreads; ++index) {
            threads.emplace_back([&library, &successes] {
                if (library.load(Fixture::request()).has_value()) {
                    ++successes;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }

        CHECK(successes.load() == kThreads);
        CHECK(fixture.compiler.compileCount == 1);
    }
    TEST_CASE("what was asked for is remembered, found or not, kept or forgotten") {
        Fixture fixture;
        ShaderLibrary library(fixture.desc());
        CHECK(library.requested().empty());
        REQUIRE(library.load(Fixture::request()).has_value());
        // Asked twice, it is one permutation.
        REQUIRE(library.load(Fixture::request()).has_value());
        ShaderRequest defined = Fixture::request();
        defined.defines = {{"B", "1"}, {"A", "1"}};
        REQUIRE(library.load(defined).has_value());
        ShaderRequest missing = Fixture::request();
        missing.path = "passes/None.slang";
        CHECK_FALSE(library.load(missing).has_value());
        library.clearMemory();

        const std::vector<ShaderRequest> asked = library.requested();
        REQUIRE(asked.size() == 3);
        for (size_t index = 0; index < asked.size(); ++index) {
            // Normalized - the defines sorted - and in the order of their keys.
            ShaderRequest normalized = asked[index];
            REQUIRE(normalized.normalize().has_value());
            CHECK(normalized.key() == asked[index].key());
            if (index > 0) {
                CHECK(asked[index - 1].key().toHex() < asked[index].key().toHex());
            }
        }
        CHECK(std::ranges::count(asked, std::string("passes/None.slang"), &ShaderRequest::path) == 1);
    }

    TEST_CASE("a library of one target compiles to it, and keeps it beside the code of others") {
        Fixture fixture;
        ShaderLibraryDesc spirvDesc = fixture.desc();
        ShaderLibraryDesc dxilDesc = fixture.desc();
        dxilDesc.target = ShaderTarget::Dxil;
        {
            ShaderLibrary spirv(spirvDesc);
            REQUIRE(spirv.load(Fixture::request()).has_value());
        }
        // The SPIR-V of the cache is no DXIL: the DXIL is compiled, as DXIL.
        ShaderLibrary dxil(dxilDesc);
        CHECK(dxil.target() == ShaderTarget::Dxil);
        const auto compiled = dxil.load(Fixture::request());
        REQUIRE_MESSAGE(compiled.has_value(), test::errorOf(compiled));
        CHECK((*compiled)->target == ShaderTarget::Dxil);
        CHECK(isDxil((*compiled)->code));
        CHECK(fixture.compiler.targets == std::vector<ShaderTarget>{ShaderTarget::Spirv, ShaderTarget::Dxil});
        CHECK((*compiled)->compilerVersion == fixture.compiler.version(ShaderTarget::Dxil));

        // Both stay in the cache, each found by a library of its target.
        ShaderRequest request = Fixture::request();
        REQUIRE(request.normalize().has_value());
        const ShaderCache cache(fixture.cacheRoot);
        CHECK(fileExists(cache.entryPath(request.key(), ShaderTarget::Spirv)));
        CHECK(fileExists(cache.entryPath(request.key(), ShaderTarget::Dxil)));
        ShaderLibrary again(dxilDesc);
        REQUIRE(again.load(Fixture::request()).has_value());
        CHECK(again.stats().diskHits == 1);
        CHECK(fixture.compiler.compileCount == 2);

        // Another version of the compiler of DXIL makes it anew.
        fixture.compiler.compilerVersion = "fake-2";
        ShaderLibrary newer(dxilDesc);
        REQUIRE(newer.load(Fixture::request()).has_value());
        CHECK(newer.stats().compilations == 1);
    }

    TEST_CASE("code of another target than the library's is refused") {
        Fixture fixture;

        // A compiler that answers DXIL with SPIR-V.
        class WrongTarget final : public IShaderCompiler {
        public:
            [[nodiscard]] std::string version(ShaderTarget /*target*/) const override { return "wrong-1"; }

            [[nodiscard]] Result<ShaderBinary> compile(const ShaderRequest& request, ShaderTarget /*target*/) override {
                return ShaderBinary{.stage = request.stage, .code = test::makeFakeSpirv()};
            }
        } wrong;

        ShaderLibraryDesc desc = fixture.desc();
        desc.compiler = &wrong;
        desc.target = ShaderTarget::Dxil;
        ShaderLibrary library(desc);
        const auto loaded = library.load(Fixture::request());
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error().code == ErrorCode::InvalidState);
        CHECK(loaded.error().message.find("no dxil") != std::string::npos);
        CHECK(library.stats().failures == 1);
    }
}
