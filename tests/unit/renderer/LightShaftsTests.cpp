#include "cordite/renderer/LightShafts.h"

#include "cordite/renderer/CameraView.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A camera at the origin that looks along -Z, ninety degrees from the top of a square
/// picture to its bottom - or without perspective.
CameraView viewAlongMinusZ(bool orthographic = false) {
    CameraComponent camera;
    camera.verticalFovDegrees = 90.0f;
    if (orthographic) {
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = 10.0f;
    }
    return makeCameraView(Mat4(1.0f), camera, 1.0f);
}

} // namespace

TEST_SUITE("LightShafts") {
    TEST_CASE("a pixel of the sky gives the sun's light and what of its own is beyond the threshold") {
        // Looking straight at the sun: all of both.
        CHECK_NEAR(lightShaftSource(Vec3(3.0f, 1.0f, 0.5f), Vec3(1.0f, 0.5f, 0.25f), 1.0f, 1.0f),
                   Vec3(3.0f, 0.5f, 0.25f), 1.0e-6f);
        // A threshold of nothing passes all of the sky.
        CHECK_NEAR(lightShaftSource(Vec3(0.5f), Vec3(0.0f), 1.0f, 0.0f), Vec3(0.5f), 1.0e-6f);
        // A sun ten thousand times as bright as white gives the most there is, no more.
        CHECK(lightShaftSource(Vec3(10000.0f), Vec3(0.0f), 1.0f, 1.0f) == Vec3(kMaxLightShaftSource));
        CHECK(lightShaftSource(Vec3(0.0f), Vec3(10000.0f, 1.0f, 0.0f), 1.0f, 1.0f) ==
              Vec3(kMaxLightShaftSource, 1.0f, 0.0f));
    }

    TEST_CASE("a pixel of the sky gives the shafts as much as it looks towards the sun") {
        // About twelve degrees off, half.
        const float cos12 = std::cos(glm::radians(12.0f));
        CHECK(lightShaftSource(Vec3(0.0f), Vec3(1.0f), cos12, 1.0f).x ==
              doctest::Approx(std::pow(cos12, kLightShaftConeExponent)));
        CHECK(std::pow(cos12, kLightShaftConeExponent) == doctest::Approx(0.5f).epsilon(0.02));
        // Looking away from it, or level with it: nothing.
        CHECK(lightShaftSource(Vec3(5.0f), Vec3(1.0f), -0.5f, 1.0f) == Vec3(0.0f));
        CHECK(lightShaftSource(Vec3(5.0f), Vec3(1.0f), 0.0f, 1.0f) == Vec3(0.0f));
        // Numbers that are none give nothing.
        CHECK(lightShaftSource(Vec3(1.0f), Vec3(1.0f), kNotANumber, 1.0f) == Vec3(0.0f));
        CHECK(lightShaftSource(Vec3(kNotANumber), Vec3(1.0f), 1.0f, 1.0f) == Vec3(0.0f));
    }

    TEST_CASE("a view has all of its shafts while its sun is ahead, fewer towards its side, none behind") {
        const CameraView view = viewAlongMinusZ();
        CHECK(lightShaftFade(view, Vec3(0.0f, 0.0f, -1.0f)) == 1.0f);
        CHECK(lightShaftFade(view, glm::normalize(Vec3(1.0f, 0.0f, -1.0f))) == 1.0f);
        const float angle = glm::radians(80.0f);
        CHECK(lightShaftFade(view, Vec3(std::sin(angle), 0.0f, -std::cos(angle))) ==
              doctest::Approx(4.0f * std::cos(angle)));
        CHECK(lightShaftFade(view, Vec3(1.0f, 0.0f, 0.0f)) == 0.0f);
        CHECK(lightShaftFade(view, Vec3(0.0f, 0.0f, 1.0f)) == 0.0f);
        CHECK(lightShaftFade(view, Vec3(kNotANumber)) == 0.0f);
        // Without perspective every ray is parallel to every other: no shafts.
        CHECK(lightShaftFade(viewAlongMinusZ(true), Vec3(0.0f, 0.0f, -1.0f)) == 0.0f);
    }

    TEST_CASE("the sun has a point in the picture while it is ahead, also outside the picture") {
        const CameraView view = viewAlongMinusZ();
        const std::optional<Vec2> ahead = sunScreenPoint(view, Vec3(0.0f, 0.0f, -1.0f));
        REQUIRE(ahead.has_value());
        CHECK_NEAR(*ahead, Vec2(0.5f), 1.0e-6f);
        // Forty-five degrees to the right is the right edge of a picture ninety degrees
        // wide; up is its top, which is row nought.
        const std::optional<Vec2> right = sunScreenPoint(view, glm::normalize(Vec3(1.0f, 0.0f, -1.0f)));
        REQUIRE(right.has_value());
        CHECK_NEAR(*right, Vec2(1.0f, 0.5f), 1.0e-5f);
        const std::optional<Vec2> up = sunScreenPoint(view, glm::normalize(Vec3(0.0f, 1.0f, -1.0f)));
        REQUIRE(up.has_value());
        CHECK_NEAR(*up, Vec2(0.5f, 0.0f), 1.0e-5f);
        // Further out, outside the picture.
        const std::optional<Vec2> outside = sunScreenPoint(view, glm::normalize(Vec3(3.0f, 0.0f, -1.0f)));
        REQUIRE(outside.has_value());
        CHECK(outside->x > 1.0f);
        // Behind, level, without perspective, or no direction at all: none.
        CHECK_FALSE(sunScreenPoint(view, Vec3(0.0f, 0.0f, 1.0f)).has_value());
        CHECK_FALSE(sunScreenPoint(view, Vec3(1.0f, 0.0f, 0.0f)).has_value());
        CHECK_FALSE(sunScreenPoint(viewAlongMinusZ(true), Vec3(0.0f, 0.0f, -1.0f)).has_value());
        CHECK_FALSE(sunScreenPoint(view, Vec3(kNotANumber)).has_value());
    }
}
