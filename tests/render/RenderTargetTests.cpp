#include "cordite/renderer/RenderTarget.h"

#include "render/RenderTestSupport.h"

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::unwrap;

TEST_SUITE("RenderTarget") {
    TEST_CASE("a default-constructed target is empty") {
        const RenderTarget target;
        CHECK_FALSE(target.isValid());
        CHECK(target.color() == nullptr);
        CHECK(target.depth() == nullptr);
        CHECK(target.framebuffer() == nullptr);
    }

    TEST_CASE("a target has the requested size, format and attachments") {
        RenderContext context;

        const RenderTarget colorOnly =
            unwrap(RenderTarget::create(context.device(), {.size = {64, 32}, .debugName = "Color only"}));
        CHECK(colorOnly.isValid());
        CHECK(colorOnly.size() == UVec2(64, 32));
        REQUIRE(colorOnly.color() != nullptr);
        CHECK(colorOnly.color()->getDesc().width == 64);
        CHECK(colorOnly.color()->getDesc().height == 32);
        CHECK(colorOnly.color()->getDesc().format == nvrhi::Format::RGBA8_UNORM);
        CHECK(colorOnly.depth() == nullptr);
        CHECK(colorOnly.framebuffer()->getFramebufferInfo().depthFormat == nvrhi::Format::UNKNOWN);

        const RenderTarget withDepth = unwrap(RenderTarget::create(
            context.device(), {.size = {48, 48}, .colorFormat = nvrhi::Format::RGBA16_FLOAT, .withDepth = true}));
        CHECK(withDepth.color()->getDesc().format == nvrhi::Format::RGBA16_FLOAT);
        REQUIRE(withDepth.depth() != nullptr);
        CHECK(withDepth.depth()->getDesc().format == nvrhi::Format::D32);
        CHECK(withDepth.framebuffer()->getFramebufferInfo().depthFormat == nvrhi::Format::D32);

        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("sizes no GPU can provide are rejected") {
        RenderContext context;
        for (const UVec2 size : {UVec2(0, 16), UVec2(16, 0), UVec2(0, 0), UVec2(20000, 16), UVec2(16, 20000)}) {
            CAPTURE(size.x);
            CAPTURE(size.y);
            const Result<RenderTarget> target = RenderTarget::create(context.device(), {.size = size});
            REQUIRE_FALSE(target.has_value());
            CHECK(target.error().code == ErrorCode::InvalidArgument);
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("clear fills the color texture and the depth buffer") {
        RenderContext context;
        RenderTarget target = unwrap(RenderTarget::create(
            context.device(), {.size = {16, 8}, .withDepth = true, .clearColor = {0.2f, 0.4f, 0.6f, 1.0f}}));

        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) { target.clear(commandList); });
        const Image first = readToImage(*context.graphics, target.color());
        REQUIRE(first.width() == 16);
        REQUIRE(first.height() == 8);
        for (uint32_t y = 0; y < first.height(); ++y) {
            for (uint32_t x = 0; x < first.width(); ++x) {
                const Vec4 pixel = first.pixel(x, y);
                REQUIRE(pixel.r == doctest::Approx(0.2f).epsilon(0.02));
                REQUIRE(pixel.g == doctest::Approx(0.4f).epsilon(0.02));
                REQUIRE(pixel.b == doctest::Approx(0.6f).epsilon(0.02));
                REQUIRE(pixel.a == doctest::Approx(1.0f));
            }
        }

        // The clear color can be changed afterwards.
        target.setClearColor({1.0f, 0.0f, 0.0f, 1.0f});
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) { target.clear(commandList); });
        const Vec4 pixel = readToImage(*context.graphics, target.color()).pixel(3, 3);
        CHECK(pixel.r == doctest::Approx(1.0f));
        CHECK(pixel.g == doctest::Approx(0.0f));

        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("resize replaces the textures and keeps everything else") {
        RenderContext context;
        RenderTarget target = unwrap(RenderTarget::create(
            context.device(),
            {.size = {32, 32}, .withDepth = true, .clearColor = {0.0f, 1.0f, 0.0f, 1.0f}, .debugName = "Resizable"}));
        const nvrhi::ITexture* originalColor = target.color();

        // The same size changes nothing.
        REQUIRE(target.resize(context.device(), UVec2(32, 32)).has_value());
        CHECK(target.color() == originalColor);

        REQUIRE(target.resize(context.device(), UVec2(80, 40)).has_value());
        CHECK(target.size() == UVec2(80, 40));
        CHECK(target.color() != originalColor);
        CHECK(target.color()->getDesc().width == 80);
        REQUIRE(target.depth() != nullptr);
        CHECK(target.depth()->getDesc().height == 40);
        CHECK(target.desc().debugName == "Resizable");

        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) { target.clear(commandList); });
        CHECK(readToImage(*context.graphics, target.color()).pixel(79, 39).g == doctest::Approx(1.0f));

        // A failed resize leaves the target usable.
        CHECK_FALSE(target.resize(context.device(), UVec2(0, 40)).has_value());
        CHECK(target.isValid());
        CHECK(target.size() == UVec2(80, 40));

        // An empty target can be given its first size this way too.
        RenderTarget empty;
        CHECK(empty.resize(context.device(), UVec2(8, 8)).has_value());
        CHECK(empty.isValid());

        checkNoValidationIssues(*context.graphics);
    }
}
