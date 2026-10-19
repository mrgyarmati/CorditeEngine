#include "cordite/shader_compiler/SlangShaderCompiler.h"

#include "cordite/core/Base.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Hash.h"

#include "support/TestSupport.h"

#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;

namespace {

constexpr const char* kCommonSource = R"(#pragma once
float3 applyTint(float3 color)
{
#if defined(TINT_RED)
    return color * float3(1.0, 0.0, 0.0);
#else
    return color;
#endif
}
)";

constexpr const char* kMainSource = R"(#include "common/Tint.slang"

cbuffer Constants : register(b0)
{
    float4 baseColor;
};

Texture2D<float4> albedo : register(t0);
SamplerState albedoSampler : register(s0);

struct Varyings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings vertexMain(uint vertexId : SV_VertexID)
{
    Varyings output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("pixel")]
float4 pixelMain(Varyings input) : SV_Target0
{
    const float4 texel = albedo.Sample(albedoSampler, input.uv);
    return float4(applyTint(texel.rgb * baseColor.rgb), 1.0);
}
)";

/// A small shader tree in a temporary directory and a compiler over it.
struct Fixture {
    Fixture() : root(directory.path() / "shaders") {
        write("common/Tint.slang", kCommonSource);
        write("passes/Main.slang", kMainSource);

        Result<std::unique_ptr<SlangShaderCompiler>> created = SlangShaderCompiler::create({.sourceRoots = {root}});
        REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
        compiler = std::move(*created);
    }

    void write(std::string_view path, std::string_view content) const {
        REQUIRE(writeTextFile(root / pathFromUtf8(path), content).has_value());
    }

    [[nodiscard]] static ShaderRequest request(std::string entryPoint = "pixelMain",
                                               ShaderStage stage = ShaderStage::Pixel) {
        ShaderRequest result;
        result.path = "passes/Main.slang";
        result.entryPoint = std::move(entryPoint);
        result.stage = stage;
        REQUIRE(result.normalize().has_value());
        return result;
    }

    TempDirectory directory;
    std::filesystem::path root;
    std::unique_ptr<SlangShaderCompiler> compiler;
};

} // namespace

TEST_SUITE("SlangShaderCompiler") {
    TEST_CASE("compiles vertex and pixel entry points to SPIR-V") {
        Fixture fixture;

        const Result<ShaderBinary> pixel = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE_MESSAGE(pixel.has_value(), test::errorOf(pixel));
        CHECK(isSpirv(pixel->code));
        CHECK(pixel->stage == ShaderStage::Pixel);
        CHECK(pixel->entryPoint == "pixelMain");
        CHECK(pixel->compilerVersion == fixture.compiler->version(ShaderTarget::Spirv));

        const Result<ShaderBinary> vertex =
            fixture.compiler->compile(Fixture::request("vertexMain", ShaderStage::Vertex), ShaderTarget::Spirv);
        REQUIRE_MESSAGE(vertex.has_value(), test::errorOf(vertex));
        CHECK(isSpirv(vertex->code));
        CHECK(vertex->code != pixel->code);
    }

    TEST_CASE("dependencies list every file read, relative to the shader root, with its hash") {
        Fixture fixture;
        const Result<ShaderBinary> binary = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE(binary.has_value());

        REQUIRE(binary->dependencies.size() == 2);
        CHECK(binary->dependencies[0].path == "common/Tint.slang");
        CHECK(binary->dependencies[1].path == "passes/Main.slang");

        const std::string_view common(kCommonSource);
        CHECK(binary->dependencies[0].contentHash ==
              hashBytes128(std::as_bytes(std::span<const char>(common.data(), common.size()))));
    }

    TEST_CASE("compiling the same request twice gives identical output") {
        Fixture fixture;
        const Result<ShaderBinary> first = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        const Result<ShaderBinary> second = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(*first == *second);
    }

    TEST_CASE("defines select a different permutation") {
        Fixture fixture;
        ShaderRequest tinted = Fixture::request();
        tinted.defines = {{"TINT_RED", "1"}};
        REQUIRE(tinted.normalize().has_value());

        const Result<ShaderBinary> plain = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        const Result<ShaderBinary> red = fixture.compiler->compile(tinted, ShaderTarget::Spirv);
        REQUIRE(plain.has_value());
        REQUIRE_MESSAGE(red.has_value(), test::errorOf(red));
        CHECK(plain->code != red->code);
    }

    TEST_CASE("a syntax error is reported with the file and line") {
        Fixture fixture;
        fixture.write("passes/Main.slang", "float4 pixelMain() : SV_Target0\n{\n    return float4(1.0, 0.0;\n}\n");

        const Result<ShaderBinary> result = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::ParseError);
        CHECK(result.error().message.find("Main.slang") != std::string::npos);
        // Slang reports locations as "<file>:<line>:<column>".
        CHECK(result.error().message.find("Main.slang:3:") != std::string::npos);
    }

    TEST_CASE("an error in an included file names that file") {
        Fixture fixture;
        fixture.write("common/Tint.slang", "float3 applyTint(float3 color)\n{\n    return undefinedSymbol;\n}\n");

        const Result<ShaderBinary> result = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("Tint.slang") != std::string::npos);
        CHECK(result.error().message.find("undefinedSymbol") != std::string::npos);
    }

    TEST_CASE("a missing entry point is an error that names it") {
        Fixture fixture;
        const Result<ShaderBinary> result =
            fixture.compiler->compile(Fixture::request("doesNotExist", ShaderStage::Pixel), ShaderTarget::Spirv);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("doesNotExist") != std::string::npos);
    }

    TEST_CASE("a missing source file is NotFound") {
        Fixture fixture;
        ShaderRequest request = Fixture::request();
        request.path = "passes/Missing.slang";

        const Result<ShaderBinary> result = fixture.compiler->compile(request, ShaderTarget::Spirv);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::NotFound);
        CHECK(result.error().message.find("passes/Missing.slang") != std::string::npos);
    }

    TEST_CASE("a missing include is an error that names it") {
        Fixture fixture;
        fixture.write("passes/Main.slang",
                      "#include \"common/Nowhere.slang\"\nfloat4 pixelMain() : SV_Target0 { return 0; }\n");

        const Result<ShaderBinary> result = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("Nowhere.slang") != std::string::npos);
    }

    TEST_CASE("the compiler stays usable after a failed compilation") {
        Fixture fixture;
        fixture.write("passes/Main.slang", "this is not a shader");
        REQUIRE_FALSE(fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv).has_value());

        fixture.write("passes/Main.slang", kMainSource);
        CHECK(fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv).has_value());
    }

    TEST_CASE("the version identifies the compiler and its options") {
        Fixture fixture;
        const std::string version = fixture.compiler->version(ShaderTarget::Spirv);
        CHECK(version.find("slang-") == 0);

        const auto unoptimized = SlangShaderCompiler::create({.sourceRoots = {fixture.root}, .optimize = false});
        const auto withDebugInfo = SlangShaderCompiler::create({.sourceRoots = {fixture.root}, .debugInfo = true});
        REQUIRE(unoptimized.has_value());
        REQUIRE(withDebugInfo.has_value());
        CHECK((*unoptimized)->version(ShaderTarget::Spirv) != version);
        CHECK((*withDebugInfo)->version(ShaderTarget::Spirv) != version);
        CHECK((*unoptimized)->version(ShaderTarget::Spirv) != (*withDebugInfo)->version(ShaderTarget::Spirv));

        // The option variants still compile.
        CHECK((*unoptimized)->compile(Fixture::request(), ShaderTarget::Spirv).has_value());
        CHECK((*withDebugInfo)->compile(Fixture::request(), ShaderTarget::Spirv).has_value());
    }

    TEST_CASE("later roots are searched when the first does not have the file") {
        Fixture fixture;
        const std::filesystem::path projectRoot = fixture.directory.path() / "project";
        REQUIRE(writeTextFile(projectRoot / "passes" / "Custom.slang",
                              "#include \"common/Tint.slang\"\n"
                              "float4 pixelMain() : SV_Target0 { return float4(applyTint(float3(1, 1, 1)), 1); }\n")
                    .has_value());

        const auto compiler = SlangShaderCompiler::create({.sourceRoots = {projectRoot, fixture.root}});
        REQUIRE(compiler.has_value());

        ShaderRequest request = Fixture::request();
        request.path = "passes/Custom.slang";
        const Result<ShaderBinary> binary = (*compiler)->compile(request, ShaderTarget::Spirv);
        REQUIRE_MESSAGE(binary.has_value(), test::errorOf(binary));
        REQUIRE(binary->dependencies.size() == 2);
        CHECK(binary->dependencies[0].path == "common/Tint.slang");
        CHECK(binary->dependencies[1].path == "passes/Custom.slang");
    }

    TEST_CASE("compiles to signed DXIL on Windows, through DXC, and to nothing where there is none") {
        Fixture fixture;
        CHECK(canCompileTo(ShaderTarget::Spirv));
        const Result<ShaderBinary> spirv = fixture.compiler->compile(Fixture::request(), ShaderTarget::Spirv);
        REQUIRE(spirv.has_value());
        const Result<ShaderBinary> dxil = fixture.compiler->compile(Fixture::request(), ShaderTarget::Dxil);
#if CORDITE_PLATFORM_WINDOWS
        CHECK(canCompileTo(ShaderTarget::Dxil));
        REQUIRE_MESSAGE(dxil.has_value(), test::errorOf(dxil));
        CHECK(dxil->target == ShaderTarget::Dxil);
        CHECK(isDxil(dxil->code));
        CHECK_FALSE(isSpirv(dxil->code));
        CHECK(dxil->stage == ShaderStage::Pixel);
        CHECK(dxil->dependencies == spirv->dependencies);
        // Its version says which DXC wrote it, so that another one makes it anew.
        const std::string version = fixture.compiler->version(ShaderTarget::Dxil);
        CHECK(dxil->compilerVersion == version);
        CHECK(version.find("dxc-1.9.2609") != std::string::npos);
        CHECK(version != fixture.compiler->version(ShaderTarget::Spirv));
        // A vertex shader too, and what does not compile says why, as for SPIR-V.
        CHECK(fixture.compiler->compile(Fixture::request("vertexMain", ShaderStage::Vertex), ShaderTarget::Dxil)
                  .has_value());
        fixture.write("passes/Main.slang", "this is not a shader");
        const Result<ShaderBinary> broken = fixture.compiler->compile(Fixture::request(), ShaderTarget::Dxil);
        REQUIRE_FALSE(broken.has_value());
        CHECK(broken.error().code == ErrorCode::ParseError);
#else
        CHECK_FALSE(canCompileTo(ShaderTarget::Dxil));
        REQUIRE_FALSE(dxil.has_value());
        CHECK(dxil.error().code == ErrorCode::Unsupported);
#endif
    }
}
