#include "cordite/renderer/MotionBlur.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <set>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

} // namespace

TEST_SUITE("MotionBlur") {
    TEST_CASE("the longest blur is a fortieth of the height of the picture, rounded up") {
        CHECK(motionBlurRadius(1) == 1);
        CHECK(motionBlurRadius(40) == 1);
        CHECK(motionBlurRadius(41) == 2);
        CHECK(motionBlurRadius(144) == 4);
        CHECK(motionBlurRadius(540) == 14);
        CHECK(motionBlurRadius(720) == 18);
        CHECK(motionBlurRadius(1080) == 27);
        CHECK(motionBlurRadius(2160) == 54);
        // Nothing high still blurs a pixel at the most.
        CHECK(motionBlurRadius(0) == 1);
    }

    TEST_CASE("a pixel is blurred half the way it went while the shutter was open, either way, and no further") {
        const Vec2 size(200.0f, 100.0f);
        // A tenth of the width is twenty pixels; with the shutter open all the time, ten
        // either way; half the time, five.
        CHECK_NEAR(blurOf(Vec2(0.1f, 0.0f), size, 1.0f, 50.0f), Vec2(10.0f, 0.0f), 1.0e-5f);
        CHECK_NEAR(blurOf(Vec2(0.1f, 0.0f), size, 0.5f, 50.0f), Vec2(5.0f, 0.0f), 1.0e-5f);
        // Down a fifth of the height, and left.
        CHECK_NEAR(blurOf(Vec2(-0.05f, 0.2f), size, 1.0f, 50.0f), Vec2(-5.0f, 10.0f), 1.0e-5f);
        // No longer than the radius, the way it went.
        CHECK_NEAR(blurOf(Vec2(0.3f, 0.4f), Vec2(100.0f), 1.0f, 5.0f), Vec2(3.0f, 4.0f), 1.0e-5f);
        // Nothing moved, the shutter was shut, or it is no number: no blur.
        CHECK(blurOf(Vec2(0.0f), size, 1.0f, 50.0f) == Vec2(0.0f));
        CHECK(blurOf(Vec2(0.1f), size, 0.0f, 50.0f) == Vec2(0.0f));
        CHECK(blurOf(Vec2(kNotANumber, 0.0f), size, 1.0f, 50.0f) == Vec2(0.0f));
        CHECK(blurOf(Vec2(std::numeric_limits<float>::infinity(), 0.0f), size, 1.0f, 50.0f) == Vec2(0.0f));
    }

    TEST_CASE("what is nearer is in front, softly over a fiftieth of the depth") {
        CHECK(inFrontOf(1.0f, 2.0f) == 1.0f);
        // As near: both are in front of each other, the same surface.
        CHECK(inFrontOf(5.0f, 5.0f) == 1.0f);
        // Two percent of the nearer behind is wholly behind; half of that, half.
        CHECK(inFrontOf(10.2f, 10.0f) == doctest::Approx(0.0f).epsilon(1.0e-4));
        CHECK(inFrontOf(10.1f, 10.0f) == doctest::Approx(0.5f).epsilon(1.0e-4));
        CHECK(inFrontOf(20.0f, 10.0f) == 0.0f);
        // The sky - as far as anything - is behind everything else.
        CHECK(inFrontOf(1.0e9f, 100.0f) == 0.0f);
        CHECK(inFrontOf(100.0f, 1.0e9f) == 1.0f);
    }

    TEST_CASE("a blur reaches a point by a cone, or by a cylinder") {
        CHECK(motionCone(0.0f, 4.0f) == 1.0f);
        CHECK(motionCone(1.0f, 4.0f) == 0.75f);
        CHECK(motionCone(4.0f, 4.0f) == 0.0f);
        CHECK(motionCone(6.0f, 4.0f) == 0.0f);
        CHECK(motionCylinder(0.0f, 4.0f) == 1.0f);
        CHECK(motionCylinder(3.7f, 4.0f) == 1.0f);
        CHECK(motionCylinder(4.0f, 4.0f) == doctest::Approx(0.5f));
        CHECK(motionCylinder(4.3f, 4.0f) == 0.0f);
    }

    TEST_CASE("a point is blended in as much as what is in front moves over what is behind") {
        const MotionBlurPoint still{.speed = 0.5f, .depth = 10.0f};
        // Something in front that moves four pixels either way reaches three pixels away
        // by a quarter.
        CHECK(motionBlurWeight({.distance = 3.0f, .speed = 4.0f, .depth = 5.0f}, still) == doctest::Approx(0.25f));
        // Something behind that moves does not reach over what stands in front of it.
        CHECK(motionBlurWeight({.distance = 3.0f, .speed = 4.0f, .depth = 20.0f}, still) == 0.0f);
        // What stands still behind a pixel that moves shows through its blur.
        CHECK(motionBlurWeight({.distance = 1.0f, .speed = 0.5f, .depth = 20.0f}, {.speed = 4.0f, .depth = 10.0f}) ==
              doctest::Approx(0.75f));
        // Two that move alike, on the same surface, reach each other: both cones, and
        // twice both cylinders.
        CHECK(motionBlurWeight({.distance = 2.0f, .speed = 4.0f, .depth = 10.0f}, {.speed = 4.0f, .depth = 10.0f}) ==
              doctest::Approx(0.5f + 0.5f + 2.0f));
        // Nothing reaches past the longer of two blurs.
        CHECK(motionBlurWeight({.distance = 5.0f, .speed = 4.0f, .depth = 10.0f}, {.speed = 4.0f, .depth = 10.0f}) ==
              0.0f);
    }

    TEST_CASE("the points of a blur are spread evenly along it, moved by the noise of the pixel") {
        for (const UVec2 pixel : {UVec2(0, 0), UVec2(17, 3), UVec2(640, 360), UVec2(1919, 1079)}) {
            CAPTURE(pixel);
            const float noise = motionBlurNoise(pixel);
            CHECK(noise >= 0.0f);
            CHECK(noise < 1.0f);
            float previous = -1.0f;
            for (uint32_t index = 0; index < 12; ++index) {
                const float along = motionBlurSampleAt(index, 12, pixel);
                CHECK(along > -1.0f);
                CHECK(along < 1.0f);
                // A step of two thirteenths from one to the next.
                if (index > 0) {
                    CHECK(along - previous == doctest::Approx(2.0f / 13.0f).epsilon(1.0e-4));
                }
                previous = along;
            }
            // Spread about the pixel: the middle of the points is the noise's own offset.
            CHECK(motionBlurSampleAt(0, 12, pixel) + motionBlurSampleAt(11, 12, pixel) ==
                  doctest::Approx((noise - 0.5f) * 4.0f / 13.0f).epsilon(1.0e-4));
        }
    }

    TEST_CASE("the noise of neighbouring pixels differs, and is the same every time") {
        // The numbers are pinned: the shader works them out the same way
        // (common/MotionBlur.slang).
        CHECK(motionBlurNoise(UVec2(0, 0)) == 0.0f);
        CHECK(motionBlurNoise(UVec2(1, 0)) == doctest::Approx(0.5557f).epsilon(1.0e-3));
        CHECK(motionBlurNoise(UVec2(0, 1)) == doctest::Approx(0.3093f).epsilon(1.0e-3));
        std::set<int> buckets;
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 0; x < 8; ++x) {
                buckets.insert(static_cast<int>(motionBlurNoise(UVec2(x, y)) * 16.0f));
                CHECK(motionBlurNoise(UVec2(x, y)) == motionBlurNoise(UVec2(x, y)));
            }
        }
        // Sixty-four pixels fall into nearly all sixteen sixteenths.
        CHECK(buckets.size() >= 14);
    }
}
