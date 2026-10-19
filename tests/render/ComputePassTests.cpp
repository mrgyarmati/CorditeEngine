#include "cordite/renderer/ComputePass.h"

#include "cordite/core/FileSystem.h"
#include "cordite/rhi/TextureReadback.h"

#include "RenderTestSupport.h"

#include <glm/gtc/packing.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace cordite;
using cordite::test::RenderContext;

namespace {

/// A shader of the tests' own: every texel of the volume it is given holds where it is.
constexpr std::string_view kFillVolume = R"(
[format("rgba16f")] RWTexture3D<float4> volume : register(u0, space0);

[numthreads(4, 4, 4)]
void fillVolumeCompute(uint3 id : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint depth;
    volume.GetDimensions(width, height, depth);
    if (any(id >= uint3(width, height, depth)))
    {
        return;
    }
    volume[id] = float4(float3(id), 1.0);
}
)";

/// The four half floats of the texel at `x`, `y` of a slice read back from an RGBA16F texture.
Vec4 texelOf(const TexturePixels& pixels, uint32_t x, uint32_t y) {
    std::array<uint16_t, 4> halves{};
    std::memcpy(halves.data(), pixels.data.data() + (static_cast<size_t>(y) * pixels.width + x) * 8, sizeof(halves));
    return {glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]), glm::unpackHalf1x16(halves[2]),
            glm::unpackHalf1x16(halves[3])};
}

} // namespace

TEST_SUITE("ComputePass") {
    TEST_CASE("thread groups cover what they are given, the last one in part") {
        static_assert(groupsFor(17, 4) == 5);
        CHECK(groupsFor(0, 8) == 0);
        CHECK(groupsFor(1, 8) == 1);
        CHECK(groupsFor(8, 8) == 1);
        CHECK(groupsFor(9, 8) == 2);
        CHECK(groupsFor(5, 0) == 0);
        CHECK(groupsFor(0xFFFFFFFFu, 2) == 0x80000000u);
    }

    TEST_CASE("a compute shader writes every texel of a volume, and each of its slices is read back") {
        RenderContext context;
        const test::TempDirectory sources;
        REQUIRE(writeTextFile(sources.path() / "FillVolume.slang", kFillVolume));
        Result<std::unique_ptr<SlangShaderCompiler>> created =
            SlangShaderCompiler::create({.sourceRoots = {sources.path(), test::shaderDirectory()}});
        REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
        const std::unique_ptr<SlangShaderCompiler> compiler = std::move(*created);
        ShaderLibraryDesc libraryDesc;
        libraryDesc.target = shaderTargetFor(context.device()->getGraphicsAPI());
        libraryDesc.sourceRoots = {sources.path(), test::shaderDirectory()};
        libraryDesc.compiler = compiler.get();
        ShaderLibrary library(std::move(libraryDesc));
        ShaderManager shaders(context.device(), library);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.registerSpace = 0;
        layoutDesc.registerSpaceIsDescriptorSet = true;
        layoutDesc.bindings = {nvrhi::BindingLayoutItem::Texture_UAV(0)};
        const nvrhi::BindingLayoutHandle layout = context.device()->createBindingLayout(layoutDesc);
        REQUIRE(layout);

        // Of sizes that no group size divides, so that the groups at the far ends are partly
        // outside it.
        nvrhi::TextureDesc volumeDesc;
        volumeDesc.dimension = nvrhi::TextureDimension::Texture3D;
        volumeDesc.width = 5;
        volumeDesc.height = 6;
        volumeDesc.depth = 7;
        volumeDesc.format = nvrhi::Format::RGBA16_FLOAT;
        volumeDesc.isUAV = true;
        volumeDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        volumeDesc.keepInitialState = true;
        volumeDesc.debugName = "Filled volume";
        const nvrhi::TextureHandle volume = context.device()->createTexture(volumeDesc);
        REQUIRE(volume);
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {nvrhi::BindingSetItem::Texture_UAV(0, volume)};
        const nvrhi::BindingSetHandle bindingSet = context.device()->createBindingSet(setDesc, layout);
        REQUIRE(bindingSet);

        Result<ComputePass> pass = ComputePass::create(
            context.device(), shaders,
            {.debugName = "Fill volume",
             .shader = {.path = "FillVolume.slang", .entryPoint = "fillVolumeCompute", .stage = ShaderStage::Compute},
             .bindingLayouts = {layout}});
        REQUIRE_MESSAGE(pass.has_value(), test::errorOf(pass));
        test::recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            const std::array<nvrhi::IBindingSet*, 1> sets = {bindingSet.Get()};
            pass->dispatch(commandList, sets, UVec3(groupsFor(5, 4), groupsFor(6, 4), groupsFor(7, 4)));
        });
        context.graphics->waitIdle();

        for (uint32_t z = 0; z < 7; ++z) {
            const Result<TexturePixels> slice = readTexture(*context.graphics, volume, 0, z);
            REQUIRE_MESSAGE(slice.has_value(), test::errorOf(slice));
            REQUIRE(slice->width == 5);
            REQUIRE(slice->height == 6);
            for (uint32_t y = 0; y < 6; ++y) {
                for (uint32_t x = 0; x < 5; ++x) {
                    CHECK(texelOf(*slice, x, y) ==
                          Vec4(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), 1.0f));
                }
            }
        }
        // As deep as the volume is, and no deeper.
        const Result<TexturePixels> beyond = readTexture(*context.graphics, volume, 0, 7);
        REQUIRE_FALSE(beyond.has_value());
        CHECK(beyond.error().code == ErrorCode::OutOfRange);
    }

    TEST_CASE("a pass is not made of a shader that is not a compute shader, or of none") {
        RenderContext context;
        const Result<ComputePass> pixel = ComputePass::create(
            context.device(), *context.shaders,
            {.debugName = "Not compute",
             .shader = {.path = "passes/Blit.slang", .entryPoint = "blitPixel", .stage = ShaderStage::Pixel}});
        REQUIRE_FALSE(pixel.has_value());
        CHECK(pixel.error().code == ErrorCode::InvalidArgument);
        CHECK(pixel.error().message.find("no compute shader") != std::string::npos);

        const Result<ComputePass> missing = ComputePass::create(
            context.device(), *context.shaders,
            {.debugName = "Missing",
             .shader = {.path = "passes/NoSuchShader.slang", .entryPoint = "main", .stage = ShaderStage::Compute}});
        CHECK_FALSE(missing.has_value());
    }
}
