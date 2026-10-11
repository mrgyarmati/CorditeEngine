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
        {.path = "passes/DebugLines.slang", .entryPoint = "debugLineVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/DebugLines.slang", .entryPoint = "debugLinePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DisplayEncode.slang", .entryPoint = "displayEncodePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomPrefilterPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomDownsamplePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Bloom.slang", .entryPoint = "bloomUpsamplePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/Exposure.slang", .entryPoint = "exposureMeterPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/PostProcess.slang", .entryPoint = "postProcessPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DebugView.slang", .entryPoint = "debugViewPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DeferredLighting.slang", .entryPoint = "deferredLightingPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/DeferredLighting.slang",
         .entryPoint = "deferredLightingPixel",
         .stage = ShaderStage::Pixel,
         .defines = {{"LIGHTING_ONLY", "1"}}},
        {.path = "passes/MeshDebug.slang", .entryPoint = "meshWireframePixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDebug.slang", .entryPoint = "meshOverdrawPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshDepth.slang", .entryPoint = "meshDepthMaskedPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshForward.slang", .entryPoint = "meshForwardPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshGBuffer.slang", .entryPoint = "meshGBufferPixel", .stage = ShaderStage::Pixel},
        {.path = "passes/MeshVertex.slang", .entryPoint = "meshVertex", .stage = ShaderStage::Vertex},
        {.path = "passes/TestPattern.slang", .entryPoint = "testPatternPixel", .stage = ShaderStage::Pixel},
        {.path = "ui/ImGui.slang", .entryPoint = "uiVertex", .stage = ShaderStage::Vertex},
        {.path = "ui/ImGui.slang", .entryPoint = "uiPixel", .stage = ShaderStage::Pixel},
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
