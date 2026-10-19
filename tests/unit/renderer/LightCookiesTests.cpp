#include "cordite/renderer/LightCookies.h"

#include "cordite/math/Transform.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <optional>

using namespace cordite;

namespace {

const Uuid kCookie = *Uuid::parse("6f1c2e4a-0b3d-4c5e-9f70-8a1b2c3d4e5f");

/// A light of `type` at `position`, shining along -Z, its right +X, with a cookie.
SceneLight lightWithCookie(GpuLightType type, const Vec3& position = Vec3(0.0f)) {
    SceneLight light;
    light.type = type;
    light.position = position;
    light.direction = Vec3(0.0f, 0.0f, -1.0f);
    light.right = Vec3(1.0f, 0.0f, 0.0f);
    // A cone whose tangent is a half: at a depth of two metres, a metre either way.
    light.outerConeAngle = std::atan(0.5f);
    light.cookie = kCookie;
    light.cookieSize = 2.0f;
    return light;
}

/// The GPU light of `light`, as far as its cookie goes.
GpuLight gpuOf(const SceneLight& light) {
    GpuLight gpu;
    gpu.typeAndChannels = static_cast<uint32_t>(light.type);
    gpu.position = light.position;
    gpu.direction = light.direction;
    gpu.cookieRight = cookieAxisOf(light);
    return gpu;
}

} // namespace

TEST_SUITE("LightCookies") {
    TEST_CASE("a spot light's cookie fills a square around its outer cone, seen from behind the light") {
        const GpuLight spot = gpuOf(lightWithCookie(GpuLightType::Spot));
        // The right, divided by the tangent of the cone: a metre either way at a depth of two
        // is the edge of the picture.
        CHECK_NEAR(spot.cookieRight, Vec3(2.0f, 0.0f, 0.0f), 1e-5f);
        CHECK_NEAR(cookieUvOf(spot, Vec3(0.0f, 0.0f, -2.0f)).value(), Vec2(0.5f), 1e-6f);
        // To the right is to the right of the picture, up is its top.
        CHECK_NEAR(cookieUvOf(spot, Vec3(0.5f, 0.0f, -2.0f)).value(), Vec2(0.75f, 0.5f), 1e-5f);
        CHECK_NEAR(cookieUvOf(spot, Vec3(0.0f, 0.5f, -2.0f)).value(), Vec2(0.5f, 0.25f), 1e-5f);
        CHECK_NEAR(cookieUvOf(spot, Vec3(-1.0f, -1.0f, -2.0f)).value(), Vec2(0.0f, 1.0f), 1e-5f);
        // The same direction further away is the same place of the picture.
        CHECK_NEAR(cookieUvOf(spot, Vec3(1.5f, 0.0f, -6.0f)).value(), Vec2(0.75f, 0.5f), 1e-5f);
        // Behind the light, and beside it, there is none.
        CHECK_FALSE(cookieUvOf(spot, Vec3(0.0f, 0.0f, 1.0f)).has_value());
        CHECK_FALSE(cookieUvOf(spot, Vec3(1.0f, 0.0f, 0.0f)).has_value());
    }

    TEST_CASE("a cookie turns with its light, about the way it shines") {
        SceneLight light = lightWithCookie(GpuLightType::Spot);
        // A quarter turn about -Z: the right of the light is the world's down.
        const Quat turn = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, -1.0f));
        light.right = turn * Vec3(1.0f, 0.0f, 0.0f);
        const GpuLight spot = gpuOf(light);
        CHECK_NEAR(spot.cookieRight, Vec3(0.0f, -2.0f, 0.0f), 1e-5f);
        CHECK_NEAR(cookieUvOf(spot, Vec3(0.0f, -0.5f, -2.0f)).value(), Vec2(0.75f, 0.5f), 1e-5f);
        CHECK_NEAR(cookieUvOf(spot, Vec3(0.5f, 0.0f, -2.0f)).value(), Vec2(0.5f, 0.25f), 1e-5f);
    }

    TEST_CASE("a directional light's cookie begins where the light is and repeats every so many metres") {
        const GpuLight sun = gpuOf(lightWithCookie(GpuLightType::Directional, Vec3(10.0f, 0.0f, 3.0f)));
        CHECK_NEAR(sun.cookieRight, Vec3(0.5f, 0.0f, 0.0f), 1e-6f);
        CHECK_NEAR(cookieUvOf(sun, Vec3(10.0f, 0.0f, -7.0f)).value(), Vec2(0.0f), 1e-6f);
        // Two metres to the right is the next cookie; a metre down, half of one down.
        CHECK_NEAR(cookieUvOf(sun, Vec3(12.0f, 0.0f, 0.0f)).value(), Vec2(1.0f, 0.0f), 1e-6f);
        CHECK_NEAR(cookieUvOf(sun, Vec3(10.0f, -1.0f, 0.0f)).value(), Vec2(0.0f, 0.5f), 1e-6f);
        // Behind the light too: a directional light is everywhere.
        CHECK(cookieUvOf(sun, Vec3(9.0f, 0.0f, 20.0f)).has_value());
    }

    TEST_CASE("the right of a light is taken at right angles to the way it shines") {
        SceneLight light = lightWithCookie(GpuLightType::Directional);
        // A right that leans along the way the light shines: only what is across it counts.
        light.right = glm::normalize(Vec3(1.0f, 0.0f, -1.0f));
        CHECK_NEAR(gpuOf(light).cookieRight, Vec3(0.5f, 0.0f, 0.0f), 1e-6f);
        // One that runs along it says nothing of which way round: some right it is.
        light.right = Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 right = gpuOf(light).cookieRight;
        CHECK(std::abs(glm::dot(right, light.direction)) < 1e-6f);
        CHECK(glm::length(right) == doctest::Approx(0.5f));
        // That is the right a rect light is wide along, too.
        CHECK_NEAR(rightAcrossOf(light) * 0.5f, right, 1e-6f);
    }

    TEST_CASE("cone angles and sizes that cannot lay a cookie out are held to what can") {
        SceneLight light = lightWithCookie(GpuLightType::Spot);
        light.outerConeAngle = kHalfPi;
        CHECK(glm::length(cookieAxisOf(light)) == doctest::Approx(1.0f / std::tan(kMaxCookieConeAngle)));
        light.outerConeAngle = 0.0f;
        CHECK(glm::length(cookieAxisOf(light)) == doctest::Approx(1.0f / std::tan(kMinCookieConeAngle)));
        light.outerConeAngle = std::numeric_limits<float>::quiet_NaN();
        CHECK(std::isfinite(glm::length(cookieAxisOf(light))));

        light.type = GpuLightType::Directional;
        light.cookieSize = 0.0f;
        CHECK(glm::length(cookieAxisOf(light)) == doctest::Approx(1.0f / kMinCookieSize));
        light.cookieSize = std::numeric_limits<float>::infinity();
        CHECK(std::isfinite(glm::length(cookieAxisOf(light))));
    }

    TEST_CASE("a light without a cookie, and a point or a rect light, has nothing to lay one out with") {
        SceneLight light = lightWithCookie(GpuLightType::Point);
        CHECK(cookieAxisOf(light) == Vec3(0.0f));
        CHECK_FALSE(cookieUvOf(gpuOf(light), Vec3(0.0f, 0.0f, -1.0f)).has_value());
        light.type = GpuLightType::Rect;
        CHECK(cookieAxisOf(light) == Vec3(0.0f));
        CHECK_FALSE(cookieUvOf(gpuOf(light), Vec3(0.0f, 0.0f, -1.0f)).has_value());
        light.type = GpuLightType::Spot;
        light.cookie = Uuid{};
        CHECK(cookieAxisOf(light) == Vec3(0.0f));
    }
}
