/// @file
/// Compiles every shader the engine ships, on every platform, without needing a GPU.
/// A shader that does not compile fails here instead of at run time.

#include "cordite/shader_compiler/SlangShaderCompiler.h"

#include "support/TestSupport.h"

#include <vector>

using namespace cordite;

namespace {

/// Every shader entry point in shaders/. Add a line when adding a shader or a permutation.
std::vector<ShaderRequest> engineShaders() {
    return {
        {.path = "common/Fullscreen.slang", .entryPoint = "fullscreenVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/Blit.slang", .entryPoint = "blitPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/TestPattern.slang", .entryPoint = "testPatternPixel", .stage = ShaderStage::Pixel},
    };
}

} // namespace

TEST_SUITE("EngineShaders") {
    TEST_CASE("every engine shader compiles without warnings") {
        const auto compiler = SlangShaderCompiler::create({.sourceRoots = {test::sourceDirectory() / "shaders"}});
        REQUIRE_MESSAGE(compiler.has_value(), compiler.error().message);

        test::LogCapture capture(LogLevel::Warn);
        for (ShaderRequest request : engineShaders()) {
            CAPTURE(request.describe());
            REQUIRE(request.normalize().has_value());

            const Result<ShaderBinary> binary = (*compiler)->compile(request);
            REQUIRE_MESSAGE(binary.has_value(), binary.error().message);
            CHECK(isSpirv(binary->code));
            CHECK_FALSE(binary->dependencies.empty());
        }

        for (const LogMessage& message : capture.messages()) {
            FAIL_CHECK("unexpected shader compiler output: " << message.text);
        }
    }
}
