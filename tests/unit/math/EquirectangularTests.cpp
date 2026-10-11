#include "cordite/math/Equirectangular.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>

using namespace cordite;

namespace {

const Vec3 kAhead(0.0f, 0.0f, -1.0f);
const Vec3 kRight(1.0f, 0.0f, 0.0f);
const Vec3 kLeft(-1.0f, 0.0f, 0.0f);
const Vec3 kBehind(0.0f, 0.0f, 1.0f);

} // namespace

TEST_SUITE("Equirectangular") {
    TEST_CASE("the middle of the image is straight ahead, its right half to the right, its top row straight up") {
        CHECK_NEAR(equirectUv(kAhead), Vec2(0.5f, 0.5f), 1.0e-6f);
        CHECK_NEAR(equirectUv(kRight), Vec2(0.75f, 0.5f), 1.0e-6f);
        CHECK_NEAR(equirectUv(kLeft), Vec2(0.25f, 0.5f), 1.0e-6f);
        CHECK(equirectUv(kWorldUp).y == doctest::Approx(0.0f));
        CHECK(equirectUv(-kWorldUp).y == doctest::Approx(1.0f));
        // Halfway up to the right of ahead: an eighth of the width on, a quarter of the
        // height up.
        CHECK_NEAR(equirectUv(Vec3(1.0f, std::sqrt(2.0f), -1.0f)), Vec2(0.625f, 0.25f), 1.0e-6f);
        // Straight behind is where the left and the right edge meet.
        const float behind = equirectUv(kBehind).x;
        CHECK((behind == doctest::Approx(0.0f) || behind == doctest::Approx(1.0f)));
        CHECK(equirectUv(Vec3(1.0e-4f, 0.0f, 1.0f)).x == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(equirectUv(Vec3(-1.0e-4f, 0.0f, 1.0f)).x == doctest::Approx(0.0f).epsilon(0.001));
    }

    TEST_CASE("how long a direction is does not matter, and what is no direction is taken for ahead") {
        CHECK_NEAR(equirectUv(Vec3(40.0f, 0.0f, 0.0f)), Vec2(0.75f, 0.5f), 1.0e-6f);
        CHECK_NEAR(equirectUv(Vec3(0.0f, 1.0e-3f, 0.0f)), Vec2(0.5f, 0.0f), 1.0e-6f);
        CHECK_NEAR(equirectUv(Vec3(0.0f)), Vec2(0.5f, 0.5f), 1.0e-6f);
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        const float infinite = std::numeric_limits<float>::infinity();
        CHECK_NEAR(equirectUv(Vec3(notANumber)), Vec2(0.5f, 0.5f), 1.0e-6f);
        CHECK_NEAR(equirectUv(Vec3(1.0f, notANumber, 0.0f)), Vec2(0.5f, 0.5f), 1.0e-6f);
        CHECK_NEAR(equirectUv(Vec3(infinite, 0.0f, 0.0f)), Vec2(0.5f, 0.5f), 1.0e-6f);
        // So long that its length cannot be told: still a place in the image.
        const Vec2 ofTheLongest = equirectUv(Vec3(std::numeric_limits<float>::max()));
        CHECK(std::isfinite(ofTheLongest.x));
        CHECK(std::isfinite(ofTheLongest.y));
    }

    TEST_CASE("a place in the image shows the direction that is found there") {
        CHECK_NEAR(equirectDirection(Vec2(0.5f, 0.5f)), kAhead, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(0.75f, 0.5f)), kRight, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(0.25f, 0.5f)), kLeft, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(0.0f, 0.5f)), kBehind, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(1.0f, 0.5f)), kBehind, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(0.3f, 0.0f)), kWorldUp, 1.0e-6f);
        CHECK_NEAR(equirectDirection(Vec2(0.9f, 1.0f)), -kWorldUp, 1.0e-6f);

        // There and back, all over the image.
        Random random(3);
        for (int index = 0; index < 200; ++index) {
            const Vec2 uv(random.range(0.01f, 0.99f), random.range(0.01f, 0.99f));
            CAPTURE(uv);
            const Vec3 direction = equirectDirection(uv);
            CHECK(glm::length(direction) == doctest::Approx(1.0f).epsilon(1.0e-5));
            CHECK_NEAR(equirectUv(direction), uv, 1.0e-4f);
        }
    }

    TEST_CASE("turning about the vertical axis goes counter-clockwise seen from above") {
        // A quarter turn brings what is ahead to the left, and what is on the right ahead.
        CHECK_NEAR(turnedAboutVertical(kAhead, kHalfPi), kLeft, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(kRight, kHalfPi), kAhead, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(kLeft, kHalfPi), kBehind, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(kAhead, -kHalfPi), kRight, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(kAhead, kPi), kBehind, 1.0e-6f);
        // Up stays up; nothing is turned by no angle, or by a whole turn.
        CHECK_NEAR(turnedAboutVertical(kWorldUp, 1.234f), kWorldUp, 1.0e-6f);
        const Vec3 some = glm::normalize(Vec3(0.3f, -0.5f, 0.8f));
        CHECK_NEAR(turnedAboutVertical(some, 0.0f), some, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(some, kTwoPi), some, 1.0e-5f);
        // It is the turn a transform makes about +Y, and the same angle back undoes it.
        const Quat turn = glm::angleAxis(0.7f, kWorldUp);
        CHECK_NEAR(turnedAboutVertical(some, 0.7f), turn * some, 1.0e-6f);
        CHECK_NEAR(turnedAboutVertical(turnedAboutVertical(some, 0.7f), -0.7f), some, 1.0e-6f);
        CHECK(turnedAboutVertical(some, 0.7f).y == some.y);
        // A length is kept as it is.
        CHECK(glm::length(turnedAboutVertical(Vec3(3.0f, 4.0f, 12.0f), 2.1f)) == doctest::Approx(13.0f));
    }
}
