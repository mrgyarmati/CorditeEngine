#include "cordite/renderer/FramePacer.h"

#include "render/RenderTestSupport.h"

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;

namespace {

/// A device and a frame's worth of work for it: enough clears of a large texture that
/// recording is much quicker than drawing.
struct PacerFixture {
    PacerFixture() : target(test::createColorTarget(context.device(), 2048, 2048)) {}

    // A command list does not keep alive what it only clears: the GPU is to be done
    // with the texture before the texture goes.
    ~PacerFixture() { context.graphics->waitIdle(); }

    PacerFixture(const PacerFixture&) = delete;
    PacerFixture& operator=(const PacerFixture&) = delete;

    void submitFrame() {
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            for (int clear = 0; clear < 16; ++clear) {
                commandList->clearTextureFloat(target, nvrhi::AllSubresources,
                                               nvrhi::Color(static_cast<float>(clear) / 16.0f));
            }
        });
    }

    RenderContext context;
    nvrhi::TextureHandle target;
};

} // namespace

TEST_SUITE("FramePacer") {
    TEST_CASE("the default lets a few frames wait for the GPU") {
        PacerFixture fixture;
        {
            const FramePacer pacer(fixture.context.device());
            CHECK(pacer.maxFramesInFlight() == FramePacer::kDefaultFramesInFlight);
            CHECK(FramePacer::kDefaultFramesInFlight == 3);
            CHECK(pacer.framesInFlight() == 0);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("no more frames wait for the GPU than the pacer allows") {
        PacerFixture fixture;
        {
            FramePacer pacer(fixture.context.device(), 2);
            CHECK(pacer.maxFramesInFlight() == 2);
            for (int frame = 0; frame < 48; ++frame) {
                fixture.submitFrame();
                pacer.frameSubmitted();
                CHECK(pacer.framesInFlight() <= 2);
            }
            // Once the GPU has caught up, nothing is waiting.
            fixture.context.graphics->waitIdle();
            CHECK(pacer.framesInFlight() == 0);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("a pacer that allows one frame lets the GPU finish each before the next but one") {
        PacerFixture fixture;
        {
            FramePacer pacer(fixture.context.device(), 1);
            for (int frame = 0; frame < 8; ++frame) {
                fixture.submitFrame();
                pacer.frameSubmitted();
                CHECK(pacer.framesInFlight() <= 1);
            }
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }

    TEST_CASE("the pacer need not be told of every frame one by one") {
        PacerFixture fixture;
        {
            FramePacer pacer(fixture.context.device(), 2);
            // Three frames handed over before it hears of any: one call covers them all.
            fixture.submitFrame();
            fixture.submitFrame();
            fixture.submitFrame();
            pacer.frameSubmitted();
            for (int frame = 0; frame < 6; ++frame) {
                fixture.submitFrame();
                pacer.frameSubmitted();
                CHECK(pacer.framesInFlight() <= 2);
            }
            fixture.context.graphics->waitIdle();
            CHECK(pacer.framesInFlight() == 0);
        }
        checkNoValidationIssues(*fixture.context.graphics);
    }
}
