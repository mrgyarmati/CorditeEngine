/// @file
/// Compiles every shader the engine ships, on every platform, without needing a GPU.
/// A shader that does not compile fails here instead of at run time.

#include "cordite/shaders/EngineShaders.h"

#include "cordite/shader_compiler/EngineShaderCache.h"
#include "cordite/shader_compiler/SlangShaderCompiler.h"
#include "cordite/shaders/ShaderLibrary.h"

#include "support/TestSupport.h"

#include <vector>

using namespace cordite;

TEST_SUITE("EngineShaders") {
    TEST_CASE("every engine shader compiles without warnings, into a cache that a game without a compiler reads") {
        const test::TempDirectory directory;
        {
            test::LogCapture capture(LogLevel::Warn);
            const Status compiled =
                compileEngineShaders(test::sourceDirectory() / "shaders", directory.path() / "shaders");
            REQUIRE_MESSAGE(compiled.has_value(), test::errorOf(compiled));
            for (const LogMessage& message : capture.messages()) {
                FAIL_CHECK("unexpected shader compiler output: " << message.text);
            }
        }

        // What an exported game has: the cache, trusted, without a compiler or sources.
        ShaderLibrary shipped({.cacheDirectory = directory.path() / "shaders", .trustCache = true});
        const std::vector<ShaderRequest> requests = engineShaderRequests();
        for (const ShaderRequest& request : requests) {
            CAPTURE(request.describe());
            const Result<std::shared_ptr<const ShaderBinary>> binary = shipped.load(request);
            REQUIRE_MESSAGE(binary.has_value(), test::errorOf(binary));
            CHECK(isSpirv((*binary)->code));
            CHECK_FALSE((*binary)->dependencies.empty());
        }
        CHECK(shipped.stats().diskHits == requests.size());
        CHECK(shipped.stats().compilations == 0);
    }

    TEST_CASE("the compiler an engine is made with, and what a cache cannot be made of") {
        const Result<std::unique_ptr<IShaderCompiler>> compiler =
            makeSlangShaderCompiler(test::sourceDirectory() / "shaders");
        REQUIRE_MESSAGE(compiler.has_value(), test::errorOf(compiler));
        CHECK_FALSE((*compiler)->version().empty());

        // Without the sources, the first permutation of the list is named.
        const test::TempDirectory directory;
        const Status failed = compileEngineShaders(directory.path() / "none", directory.path() / "shaders");
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message.find(engineShaderRequests().front().path) != std::string::npos);
    }
    TEST_CASE("the list of the engine's shaders is normalized, each permutation once, in order") {
        const std::vector<ShaderRequest> requests = engineShaderRequests();
        REQUIRE_FALSE(requests.empty());
        for (size_t index = 0; index < requests.size(); ++index) {
            ShaderRequest normalized = requests[index];
            REQUIRE(normalized.normalize().has_value());
            CHECK(normalized.key() == requests[index].key());
            if (index > 0) {
                CAPTURE(requests[index].describe());
                CHECK(requests[index - 1].describe() < requests[index].describe());
            }
        }
    }
}
