#include "cordite/renderer/GpuTimer.h"

#include "render/RenderTestSupport.h"

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;

namespace {

/// A device, a texture to work on, and a stretch of GPU work to time.
struct TimerFixture {
    TimerFixture() : target(test::createColorTarget(context.device(), 256, 256)) {}

    /// Records and executes one frame: a clear of the texture, timed by `timer`.
    void frame(GpuTimer& timer) {
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            timer.begin(commandList);
            commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(1.0f));
            timer.end(commandList);
        });
    }

    [[nodiscard]] bool canMeasure() const { return context.graphics->adapter().supportsTimerQueries; }

    RenderContext context;
    nvrhi::TextureHandle target;
};

} // namespace

TEST_SUITE("GpuTimer") {
    TEST_CASE("a timer on an adapter that cannot time its work never has a result") {
        TimerFixture fixture;
        {
            GpuTimer timer(fixture.context.device(), false);
            fixture.frame(timer);
            fixture.context.graphics->waitIdle();
            fixture.frame(timer);

            CHECK_FALSE(timer.latestMilliseconds().has_value());
            CHECK(timer.measurementCount() == 0);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("a measurement arrives once the GPU has done the work") {
        TimerFixture fixture;
        if (!fixture.canMeasure()) {
            MESSAGE("this adapter cannot time its work; nothing to test");
            return;
        }
        {
            GpuTimer timer(fixture.context.device(), true);
            fixture.frame(timer);
            // Results are taken in when the next stretch begins, not before.
            CHECK_FALSE(timer.latestMilliseconds().has_value());

            fixture.context.graphics->waitIdle();
            fixture.frame(timer);
            REQUIRE(timer.latestMilliseconds().has_value());
            CHECK(*timer.latestMilliseconds() > 0.0);
            // Clearing a small texture takes nowhere near a second.
            CHECK(*timer.latestMilliseconds() < 1000.0);
            CHECK(timer.measurementCount() == 1);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("frames that come faster than the GPU finishes them are skipped, not mixed up") {
        TimerFixture fixture;
        if (!fixture.canMeasure()) {
            MESSAGE("this adapter cannot time its work; nothing to test");
            return;
        }
        {
            GpuTimer timer(fixture.context.device(), true);
            constexpr uint64_t kFrames = 24;
            for (uint64_t index = 0; index < kFrames; ++index) {
                fixture.frame(timer);
            }
            fixture.context.graphics->waitIdle();
            fixture.frame(timer);

            // At least the last frames that were measured have come back, and no frame
            // was counted twice.
            CHECK(timer.measurementCount() >= 1);
            CHECK(timer.measurementCount() <= kFrames);
            REQUIRE(timer.latestMilliseconds().has_value());
            CHECK(*timer.latestMilliseconds() < 1000.0);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("a stretch that is begun and never ended is forgotten") {
        TimerFixture fixture;
        if (!fixture.canMeasure()) {
            MESSAGE("this adapter cannot time its work; nothing to test");
            return;
        }
        {
            GpuTimer timer(fixture.context.device(), true);
            // Ending what was never begun is ignored.
            recordAndExecute(fixture.context, [&](nvrhi::ICommandList* commandList) { timer.end(commandList); });
            // A stretch that its command list never ends is dropped when the next begins.
            recordAndExecute(fixture.context, [&](nvrhi::ICommandList* commandList) { timer.begin(commandList); });

            fixture.frame(timer);
            fixture.context.graphics->waitIdle();
            fixture.frame(timer);
            CHECK(timer.measurementCount() == 1);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }
}
