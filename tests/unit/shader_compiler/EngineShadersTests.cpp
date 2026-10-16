/// @file
/// Compiles every shader the engine ships, on every platform, without needing a GPU.
/// A shader that does not compile fails here instead of at run time.

#include "cordite/shaders/EngineShaders.h"

#include "cordite/shader_compiler/SlangShaderCompiler.h"

#include "support/TestSupport.h"

#include <vector>

using namespace cordite;

TEST_SUITE("EngineShaders") {
    TEST_CASE("every engine shader compiles without warnings") {
        const auto compiler = SlangShaderCompiler::create({.sourceRoots = {test::sourceDirectory() / "shaders"}});
        REQUIRE_MESSAGE(compiler.has_value(), test::errorOf(compiler));

        test::LogCapture capture(LogLevel::Warn);
        for (const ShaderRequest& request : engineShaderRequests()) {
            CAPTURE(request.describe());

            const Result<ShaderBinary> binary = (*compiler)->compile(request);
            REQUIRE_MESSAGE(binary.has_value(), test::errorOf(binary));
            CHECK(isSpirv(binary->code));
            CHECK_FALSE(binary->dependencies.empty());
        }

        for (const LogMessage& message : capture.messages()) {
            FAIL_CHECK("unexpected shader compiler output: " << message.text);
        }
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
