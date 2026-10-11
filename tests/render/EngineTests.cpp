#include "cordite/engine/Engine.h"

#include "cordite/assets/ImageCompare.h"

#include "render/RenderTestSupport.h"

#include <memory>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::TempDirectory;

namespace {

EngineDesc headlessDesc(UVec2 size = UVec2(320, 180)) {
    EngineDesc desc;
    desc.applicationName = "Cordite engine tests";
    desc.headless = true;
    desc.validation = true;
    desc.renderSize = size;
    desc.fixedTimeStep = 1.0 / 60.0;
    // Tests must not depend on where the engine would look for shaders by default.
    desc.shaderDirectory = test::shaderDirectory();
    return desc;
}

std::unique_ptr<Engine> createEngine(const EngineDesc& desc) {
    Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
    REQUIRE_MESSAGE(engine.has_value(), engine.error().message);
    return std::move(*engine);
}

void runFrames(Engine& engine, int count) {
    for (int frame = 0; frame < count; ++frame) {
        const Result<bool> keepRunning = engine.runFrame();
        REQUIRE_MESSAGE(keepRunning.has_value(), keepRunning.error().message);
        REQUIRE(*keepRunning);
    }
}

} // namespace

TEST_SUITE("Engine") {
    TEST_CASE("a headless engine renders frames with a fixed time step") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        CHECK(engine->window() == nullptr);
        CHECK(engine->frameIndex() == 0);
        CHECK(engine->renderSize() == UVec2(320, 180));

        runFrames(*engine, 30);
        CHECK(engine->frameIndex() == 30);
        CHECK(engine->timeSeconds() == doctest::Approx(0.5));

        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("captureFrame returns the last rendered frame") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(200, 100)));

        const Result<Image> tooEarly = engine->captureFrame();
        REQUIRE_FALSE(tooEarly.has_value());
        CHECK(tooEarly.error().code == ErrorCode::InvalidState);

        runFrames(*engine, 1);
        const Result<Image> frame = engine->captureFrame();
        REQUIRE_MESSAGE(frame.has_value(), frame.error().message);
        CHECK(frame->width() == 200);
        CHECK(frame->height() == 100);
        CHECK(frame->format() == ImageFormat::Rgba8);
        // The first frame is rendered at time zero: the test pattern's highlight bar sits
        // on the left edge, and alpha is opaque everywhere.
        CHECK(frame->pixel(0, 50).b > 0.5f);
        CHECK(frame->pixel(100, 50).a == 1.0f);

        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("two engines given the same inputs render identical frames") {
        // One engine at a time: each owns the process-wide window system.
        const auto renderTwelveFrames = [] {
            const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
            runFrames(*engine, 12);
            Result<Image> frame = engine->captureFrame();
            REQUIRE_MESSAGE(frame.has_value(), frame.error().message);
            return std::move(*frame);
        };
        const Image firstFrame = renderTwelveFrames();
        const Image secondFrame = renderTwelveFrames();

        const ImageComparison comparison =
            test::unwrap(compareImages(firstFrame, secondFrame, {.channelTolerance = 0}));
        CHECK(comparison.matches);
        CHECK(comparison.maxChannelDifference == 0);
    }

    TEST_CASE("frames differ as time advances") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 1);
        const Image early = test::unwrap(engine->captureFrame());
        runFrames(*engine, 60);
        const Image late = test::unwrap(engine->captureFrame());

        const ImageComparison comparison = test::unwrap(compareImages(early, late, {.channelTolerance = 8}));
        CHECK_FALSE(comparison.matches);
        CHECK(comparison.differingPixels > 100);
    }

    TEST_CASE("requestExit ends the frame loop") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 2);

        engine->requestExit();
        const Result<bool> keepRunning = engine->runFrame();
        REQUIRE(keepRunning.has_value());
        CHECK_FALSE(*keepRunning);
        CHECK(engine->frameIndex() == 2);
    }

    TEST_CASE("the shader cache directory is used across engine instances") {
        TempDirectory cache;
        EngineDesc desc = headlessDesc();
        desc.cacheDirectory = cache.path();

        {
            const std::unique_ptr<Engine> engine = createEngine(desc);
            runFrames(*engine, 1);
            CHECK(engine->shaders().stats().compilations == 3);
            CHECK(engine->shaders().stats().diskHits == 0);
        }

        const std::unique_ptr<Engine> engine = createEngine(desc);
        runFrames(*engine, 1);
        CHECK(engine->shaders().stats().compilations == 0);
        CHECK(engine->shaders().stats().diskHits == 3);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("only one engine can exist at a time") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        const Result<std::unique_ptr<Engine>> second = Engine::create(headlessDesc());
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidState);

        // The failed attempt did not disturb the engine that exists.
        runFrames(*engine, 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("invalid descriptions are rejected") {
        EngineDesc zeroSize = headlessDesc();
        zeroSize.renderSize = UVec2(0, 100);
        const Result<std::unique_ptr<Engine>> first = Engine::create(zeroSize);
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().code == ErrorCode::InvalidArgument);

        EngineDesc negativeStep = headlessDesc();
        negativeStep.fixedTimeStep = -1.0;
        CHECK_FALSE(Engine::create(negativeStep).has_value());

        EngineDesc missingShaders = headlessDesc();
        missingShaders.shaderDirectory = test::sourceDirectory() / "no-such-directory";
        const Result<std::unique_ptr<Engine>> third = Engine::create(missingShaders);
        REQUIRE_FALSE(third.has_value());
        CHECK(third.error().code == ErrorCode::NotFound);
    }

    TEST_CASE("an engine can be created again after a failed creation") {
        EngineDesc broken = headlessDesc();
        broken.shaderDirectory = test::sourceDirectory() / "no-such-directory";
        REQUIRE_FALSE(Engine::create(broken).has_value());

        // The failed attempt must have released the window system and the device.
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("a windowed engine presents and follows window resizes") {
        EngineDesc desc = headlessDesc();
        desc.headless = false;
        desc.window.title = "Cordite engine test";
        desc.window.width = 320;
        desc.window.height = 200;
        desc.window.visible = false;

        const std::unique_ptr<Engine> engine = createEngine(desc);
        REQUIRE(engine->window() != nullptr);
        runFrames(*engine, 5);
        CHECK(engine->renderSize() == engine->window()->framebufferSize());

        engine->window()->setLogicalSize(UVec2(400, 240));
        runFrames(*engine, 3);
        CHECK(engine->renderSize() == engine->window()->framebufferSize());

        const Result<Image> frame = engine->captureFrame();
        REQUIRE(frame.has_value());
        CHECK(frame->width() == engine->renderSize().x);
        CHECK(frame->height() == engine->renderSize().y);

        engine->window()->requestClose();
        const Result<bool> keepRunning = engine->runFrame();
        REQUIRE(keepRunning.has_value());
        CHECK_FALSE(*keepRunning);

        checkNoValidationIssues(engine->graphics());
    }
}
