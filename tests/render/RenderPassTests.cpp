#include "cordite/renderer/BlitPass.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/renderer/TestPatternPass.h"
#include "cordite/shaders/ShaderLibrary.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::createColorTarget;
using cordite::test::readToImage;
using cordite::test::RenderContext;

namespace {

void execute(RenderContext& context, nvrhi::ICommandList* commandList) {
    context.device()->executeCommandList(commandList);
}

/// Renders the test pattern into a new texture of the given size and format.
nvrhi::TextureHandle renderPattern(RenderContext& context, TestPatternPass& pattern, uint32_t width, uint32_t height,
                                   const TestPatternParameters& parameters = {},
                                   nvrhi::Format format = nvrhi::Format::RGBA8_UNORM) {
    nvrhi::TextureHandle target = createColorTarget(context.device(), width, height, format);
    const nvrhi::FramebufferHandle framebuffer =
        context.device()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

    const nvrhi::CommandListHandle commandList = context.device()->createCommandList();
    commandList->open();
    pattern.draw(commandList, framebuffer, parameters);
    commandList->close();
    execute(context, commandList);
    return target;
}

ShaderRequest blitRequest() {
    ShaderRequest request;
    request.path = "passes/Blit.slang";
    request.entryPoint = "blitPixel";
    request.stage = ShaderStage::Pixel;
    return request;
}

} // namespace

TEST_SUITE("ShaderManager") {
    TEST_CASE("a device runs the shaders of its graphics API, and a manager is made with those alone") {
        CHECK(shaderTargetFor(nvrhi::GraphicsAPI::VULKAN) == ShaderTarget::Spirv);
        CHECK(shaderTargetFor(nvrhi::GraphicsAPI::D3D12) == ShaderTarget::Dxil);

        RenderContext context;
        CHECK(context.library->target() == shaderTargetFor(context.device()->getGraphicsAPI()));
#if defined(CORDITE_ENABLE_ASSERTS)
        // A library of the other kind of code is a mistake of whoever made the two.
        ShaderLibraryDesc other;
        other.target = context.library->target() == ShaderTarget::Spirv ? ShaderTarget::Dxil : ShaderTarget::Spirv;
        ShaderLibrary otherLibrary(std::move(other));
        CHECK_THROWS_AS(ShaderManager(context.device(), otherLibrary), test::AssertionFailure);
#endif
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the same permutation is created once and shared") {
        RenderContext context;

        const Result<nvrhi::ShaderHandle> first = context.shaders->get(blitRequest());
        const Result<nvrhi::ShaderHandle> second = context.shaders->get(blitRequest());
        REQUIRE_MESSAGE(first.has_value(), test::errorOf(first));
        REQUIRE(second.has_value());
        CHECK(first->Get() == second->Get());
        // Made once: compiled, or read from what a test before compiled.
        const ShaderLibraryStats stats = context.library->stats();
        CHECK(stats.compilations + stats.diskHits == 1);

        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the tests of a run compile a shader once, and read it from the cache they share after") {
        {
            const RenderContext first;
            REQUIRE(first.shaders->get(blitRequest()).has_value());
        }
        const RenderContext second;
        REQUIRE(second.shaders->get(blitRequest()).has_value());
        CHECK(second.library->stats().compilations == 0);
        CHECK(second.library->stats().diskHits == 1);
    }

    TEST_CASE("evicted shaders are created again on the next request") {
        RenderContext context;
        const nvrhi::ShaderHandle first = test::unwrap(context.shaders->get(blitRequest()));

        const std::vector<ShaderRequest> requests = {blitRequest()};
        context.shaders->evict(requests);
        const nvrhi::ShaderHandle second = test::unwrap(context.shaders->get(blitRequest()));
        CHECK(first.Get() != second.Get());

        context.shaders->clear();
        CHECK(context.shaders->get(blitRequest()).has_value());
        // The SPIR-V itself stayed in the library the whole time: made once.
        const ShaderLibraryStats stats = context.library->stats();
        CHECK(stats.compilations + stats.diskHits == 1);
    }

    TEST_CASE("a shader that fails to compile is an error, not a crash") {
        RenderContext context;
        ShaderRequest missing = blitRequest();
        missing.path = "passes/DoesNotExist.slang";

        const Result<nvrhi::ShaderHandle> shader = context.shaders->get(missing);
        REQUIRE_FALSE(shader.has_value());
        CHECK(shader.error().message.find("DoesNotExist") != std::string::npos);

        ShaderRequest invalid = blitRequest();
        invalid.path = "../escape.slang";
        CHECK_FALSE(context.shaders->get(invalid).has_value());
    }
}

TEST_SUITE("TestPatternPass") {
    TEST_CASE("the pattern matches its reference image") {
        RenderContext context;
        Result<TestPatternPass> pattern = TestPatternPass::create(context.device(), *context.shaders);
        REQUIRE_MESSAGE(pattern.has_value(), test::errorOf(pattern));

        const nvrhi::TextureHandle target = renderPattern(context, *pattern, 320, 180);
        checkAgainstReference(readToImage(*context.graphics, target), "test-pattern-320x180");
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the image has the documented orientation") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));

        // Time 2 puts the highlight bar in the middle, away from the corners sampled below.
        const Image image =
            readToImage(*context.graphics, renderPattern(context, pattern, 256, 256, {.timeSeconds = 2.0f}));

        const Vec4 topLeft = image.pixel(2, 2);
        const Vec4 topRight = image.pixel(253, 2);
        const Vec4 bottomLeft = image.pixel(2, 253);
        // Red grows to the right and green downward: uv (0, 0) is the top-left corner.
        CHECK(topRight.r > topLeft.r + 0.4f);
        CHECK(bottomLeft.g > topLeft.g + 0.4f);
        CHECK(topRight.g < 0.1f);
        CHECK(bottomLeft.r < 0.1f);
        CHECK(image.pixel(128, 128).a == 1.0f);
    }

    TEST_CASE("parameters change the image") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));

        const Image atStart = readToImage(*context.graphics, renderPattern(context, pattern, 128, 128));
        const Image later =
            readToImage(*context.graphics, renderPattern(context, pattern, 128, 128, {.timeSeconds = 2.0f}));
        const Image redOnly =
            readToImage(*context.graphics, renderPattern(context, pattern, 128, 128, {.tint = Vec3(1.0f, 0.0f, 0.0f)}));

        // The bar moved from the left edge to the centre.
        CHECK(later.pixel(64, 64).b > atStart.pixel(64, 64).b + 0.2f);
        // A red tint removes green and blue everywhere.
        for (const UVec2 point : {UVec2(5, 5), UVec2(120, 10), UVec2(10, 120), UVec2(100, 100)}) {
            CHECK(redOnly.pixel(point.x, point.y).g == 0.0f);
            CHECK(redOnly.pixel(point.x, point.y).b == 0.0f);
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("rendering the same frame twice gives identical pixels") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));

        const Image first =
            readToImage(*context.graphics, renderPattern(context, pattern, 200, 120, {.timeSeconds = 1.25f}));
        const Image second =
            readToImage(*context.graphics, renderPattern(context, pattern, 200, 120, {.timeSeconds = 1.25f}));

        const ImageComparison comparison = test::unwrap(compareImages(first, second, {.channelTolerance = 0}));
        CHECK(comparison.matches);
        CHECK(comparison.maxChannelDifference == 0);
    }

    TEST_CASE("one pass draws into targets of different formats") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));

        // Each framebuffer layout needs its own pipeline; the pass creates them on demand.
        const Image rgba = readToImage(*context.graphics, renderPattern(context, pattern, 64, 64));
        const Image bgra =
            readToImage(*context.graphics, renderPattern(context, pattern, 64, 64, {}, nvrhi::Format::BGRA8_UNORM));
        const Image rgbaAgain = readToImage(*context.graphics, renderPattern(context, pattern, 64, 64));

        CHECK((*compareImages(rgba, bgra, {.channelTolerance = 1})).matches);
        CHECK((*compareImages(rgba, rgbaAgain, {.channelTolerance = 0})).matches);
        checkNoValidationIssues(*context.graphics);
    }
}

TEST_SUITE("BlitPass") {
    TEST_CASE("a same-size blit copies the image exactly") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));
        Result<BlitPass> blit = BlitPass::create(context.device(), *context.shaders);
        REQUIRE_MESSAGE(blit.has_value(), test::errorOf(blit));

        const nvrhi::TextureHandle source = renderPattern(context, pattern, 96, 64);
        const nvrhi::TextureHandle destination = createColorTarget(context.device(), 96, 64);
        const nvrhi::FramebufferHandle framebuffer =
            context.device()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(destination));

        const nvrhi::CommandListHandle commandList = context.device()->createCommandList();
        commandList->open();
        blit->draw(commandList, source, framebuffer);
        commandList->close();
        execute(context, commandList);

        const ImageComparison comparison =
            *compareImages(readToImage(*context.graphics, destination), readToImage(*context.graphics, source),
                           {.channelTolerance = 0});
        CHECK(comparison.matches);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a blit to a larger target scales the image") {
        RenderContext context;
        TestPatternPass pattern = test::unwrap(TestPatternPass::create(context.device(), *context.shaders));
        BlitPass blit = test::unwrap(BlitPass::create(context.device(), *context.shaders));

        const nvrhi::TextureHandle source = renderPattern(context, pattern, 64, 64, {.timeSeconds = 2.0f});
        const nvrhi::TextureHandle destination = createColorTarget(context.device(), 256, 256);
        const nvrhi::FramebufferHandle framebuffer =
            context.device()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(destination));

        const nvrhi::CommandListHandle commandList = context.device()->createCommandList();
        commandList->open();
        blit.draw(commandList, source, framebuffer);
        commandList->close();
        execute(context, commandList);

        const Image small = readToImage(*context.graphics, source);
        const Image large = readToImage(*context.graphics, destination);
        REQUIRE(large.width() == 256);

        // The centre of each source checker cell lands on the corresponding spot in the
        // enlarged image; cell centres are far from edges, so filtering does not matter.
        for (const UVec2 cell : {UVec2(0, 0), UVec2(7, 0), UVec2(0, 7), UVec2(7, 7), UVec2(2, 5)}) {
            const Vec4 expected = small.pixel(cell.x * 8 + 4, cell.y * 8 + 4);
            const Vec4 actual = large.pixel(cell.x * 32 + 16, cell.y * 32 + 16);
            CHECK_NEAR(actual, expected, 0.05f);
        }
        checkNoValidationIssues(*context.graphics);
    }
}
