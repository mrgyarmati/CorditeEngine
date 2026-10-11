#include "cordite/core/Time.h"

#include "support/TestSupport.h"

#include <chrono>
#include <limits>
#include <thread>

using namespace cordite;

TEST_SUITE("Time") {
    TEST_CASE("Stopwatch measures elapsed time monotonically") {
        Stopwatch stopwatch;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double first = stopwatch.elapsedSeconds();
        CHECK(first >= 0.015);
        CHECK(stopwatch.elapsedMilliseconds() >= first * 1000.0);

        stopwatch.restart();
        CHECK(stopwatch.elapsedSeconds() < first);
    }

    TEST_CASE("FixedTimestep yields one step per step of frame time") {
        FixedTimestep clock(1.0 / 60.0);
        uint32_t steps = 0;
        for (int frame = 0; frame < 600; ++frame) {
            steps += clock.advance(1.0 / 60.0);
        }
        CHECK(steps == 600);
        CHECK(clock.alpha() == doctest::Approx(0.0).epsilon(1e-3));
    }

    TEST_CASE("FixedTimestep accumulates partial frames") {
        FixedTimestep clock(0.1);
        CHECK(clock.advance(0.04) == 0);
        CHECK(clock.alpha() == doctest::Approx(0.4));
        CHECK(clock.advance(0.04) == 0);
        CHECK(clock.advance(0.04) == 1);
        CHECK(clock.alpha() == doctest::Approx(0.2));
    }

    TEST_CASE("FixedTimestep runs several steps for a slow frame") {
        FixedTimestep clock(0.01, 8);
        CHECK(clock.advance(0.035) == 3);
        CHECK(clock.alpha() == doctest::Approx(0.5));
    }

    TEST_CASE("FixedTimestep drops surplus time instead of spiralling") {
        FixedTimestep clock(0.01, 4);
        CHECK(clock.advance(1.0) == 4);
        CHECK(clock.alpha() == doctest::Approx(0.0));
        // The backlog is gone: the next ordinary frame runs one step.
        CHECK(clock.advance(0.01) == 1);
    }

    TEST_CASE("FixedTimestep total step count is independent of frame pacing") {
        FixedTimestep steady(1.0 / 120.0, 64);
        FixedTimestep jittery(1.0 / 120.0, 64);

        uint32_t steadySteps = 0;
        for (int frame = 0; frame < 1000; ++frame) {
            steadySteps += steady.advance(0.01);
        }

        uint32_t jitterySteps = 0;
        for (int frame = 0; frame < 500; ++frame) {
            jitterySteps += jittery.advance(0.003);
            jitterySteps += jittery.advance(0.017);
        }

        // Both clocks consumed 10 seconds of frame time.
        CHECK(steadySteps == 1200);
        CHECK(jitterySteps == 1200);
    }

    TEST_CASE("FixedTimestep ignores invalid frame times") {
        FixedTimestep clock(0.01);
        CHECK(clock.advance(-1.0) == 0);
        CHECK(clock.advance(0.0) == 0);
        CHECK(clock.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
        CHECK(clock.advance(std::numeric_limits<double>::infinity()) == 0);
        CHECK(clock.alpha() == 0.0);
    }

    TEST_CASE("FixedTimestep alpha stays below one") {
        FixedTimestep clock(0.01);
        for (int frame = 0; frame < 1000; ++frame) {
            clock.advance(0.0037);
            CHECK(clock.alpha() >= 0.0);
            CHECK(clock.alpha() < 1.0);
        }
    }

    TEST_CASE("reset and setStepSeconds") {
        FixedTimestep clock(0.1);
        clock.advance(0.05);
        clock.reset();
        CHECK(clock.alpha() == 0.0);

        clock.setStepSeconds(0.02);
        CHECK(clock.stepSeconds() == 0.02);
        clock.setStepSeconds(-1.0);
        clock.setStepSeconds(0.0);
        CHECK(clock.stepSeconds() == 0.02);
        CHECK(clock.maxStepsPerFrame() == 8);
    }
}
