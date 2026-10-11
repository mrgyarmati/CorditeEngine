#include "cordite/math/Projection.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

using namespace cordite;

namespace {

/// Projects a view-space point and returns normalized device coordinates.
Vec3 project(const Mat4& projection, const Vec3& viewPoint) {
    const Vec4 clip = projection * Vec4(viewPoint, 1.0f);
    return Vec3(clip) / clip.w;
}

} // namespace

TEST_SUITE("Projection") {
    TEST_CASE("perspectiveReverseZ maps near to depth 1 and far to depth 0") {
        const Mat4 projection = perspectiveReverseZ(toRadians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

        CHECK(project(projection, Vec3(0.0f, 0.0f, -0.1f)).z == doctest::Approx(1.0f));
        CHECK(project(projection, Vec3(0.0f, 0.0f, -100.0f)).z == doctest::Approx(0.0f).epsilon(1e-5));

        const float middle = project(projection, Vec3(0.0f, 0.0f, -10.0f)).z;
        CHECK(middle > 0.0f);
        CHECK(middle < 1.0f);
        // Depth decreases with distance.
        CHECK(project(projection, Vec3(0.0f, 0.0f, -20.0f)).z < middle);
    }

    TEST_CASE("perspectiveReverseZ puts +Y up and +X right on screen") {
        const float fov = toRadians(90.0f);
        const float aspect = 2.0f;
        const Mat4 projection = perspectiveReverseZ(fov, aspect, 0.5f, 50.0f);

        // With a 90 degree vertical field of view the top edge is at y = distance.
        const float distance = 10.0f;
        CHECK_NEAR(Vec2(project(projection, Vec3(0.0f, distance, -distance))), Vec2(0.0f, 1.0f), 1e-5f);
        CHECK_NEAR(Vec2(project(projection, Vec3(0.0f, -distance, -distance))), Vec2(0.0f, -1.0f), 1e-5f);
        CHECK_NEAR(Vec2(project(projection, Vec3(distance * aspect, 0.0f, -distance))), Vec2(1.0f, 0.0f), 1e-5f);
        CHECK_NEAR(Vec2(project(projection, Vec3(0.0f, 0.0f, -distance))), Vec2(0.0f, 0.0f), 1e-6f);
    }

    TEST_CASE("perspectiveInfiniteReverseZ has no far limit") {
        const Mat4 projection = perspectiveInfiniteReverseZ(toRadians(60.0f), 1.0f, 0.25f);

        CHECK(project(projection, Vec3(0.0f, 0.0f, -0.25f)).z == doctest::Approx(1.0f));
        const float distant = project(projection, Vec3(0.0f, 0.0f, -1.0e6f)).z;
        CHECK(distant > 0.0f);
        CHECK(distant < 1e-5f);
    }

    TEST_CASE("orthographicReverseZ maps the volume to the clip cube") {
        const Mat4 projection = orthographicReverseZ(-4.0f, 4.0f, -2.0f, 2.0f, 1.0f, 21.0f);

        CHECK_NEAR(project(projection, Vec3(-4.0f, -2.0f, -1.0f)), Vec3(-1.0f, -1.0f, 1.0f), 1e-5f);
        CHECK_NEAR(project(projection, Vec3(4.0f, 2.0f, -21.0f)), Vec3(1.0f, 1.0f, 0.0f), 1e-5f);
        CHECK_NEAR(project(projection, Vec3(0.0f, 0.0f, -11.0f)), Vec3(0.0f, 0.0f, 0.5f), 1e-5f);
    }

    TEST_CASE("orthographicReverseZ handles an off-centre volume") {
        const Mat4 projection = orthographicReverseZ(0.0f, 10.0f, 0.0f, 5.0f, 0.0f, 10.0f);
        CHECK_NEAR(project(projection, Vec3(0.0f, 0.0f, 0.0f)), Vec3(-1.0f, -1.0f, 1.0f), 1e-5f);
        CHECK_NEAR(project(projection, Vec3(10.0f, 5.0f, -10.0f)), Vec3(1.0f, 1.0f, 0.0f), 1e-5f);
    }

    TEST_CASE("conventional projections map near to 0 and far to 1") {
        const Mat4 ortho = orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 12.0f);
        CHECK(project(ortho, Vec3(0.0f, 0.0f, -2.0f)).z == doctest::Approx(0.0f));
        CHECK(project(ortho, Vec3(0.0f, 0.0f, -12.0f)).z == doctest::Approx(1.0f));
        CHECK_NEAR(Vec2(project(ortho, Vec3(1.0f, 1.0f, -5.0f))), Vec2(1.0f, 1.0f), 1e-6f);

        const Mat4 persp = perspective(toRadians(90.0f), 1.0f, 1.0f, 100.0f);
        CHECK(project(persp, Vec3(0.0f, 0.0f, -1.0f)).z == doctest::Approx(0.0f));
        CHECK(project(persp, Vec3(0.0f, 0.0f, -100.0f)).z == doctest::Approx(1.0f));
        CHECK_NEAR(Vec2(project(persp, Vec3(0.0f, 5.0f, -5.0f))), Vec2(0.0f, 1.0f), 1e-5f);
    }

    TEST_CASE("linearizeReverseZ inverts the depth mapping") {
        const float nearPlane = 0.1f;
        const float farPlane = 500.0f;
        const Mat4 projection = perspectiveReverseZ(toRadians(70.0f), 1.5f, nearPlane, farPlane);

        for (const float distance : {0.1f, 0.5f, 3.0f, 42.0f, 250.0f, 500.0f}) {
            const float depth = project(projection, Vec3(0.0f, 0.0f, -distance)).z;
            CHECK(linearizeReverseZ(depth, nearPlane, farPlane) == doctest::Approx(distance).epsilon(1e-3));
        }
    }

    TEST_CASE("lookAt places the target on the -Z axis of view space") {
        const Mat4 view = lookAt(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f));
        CHECK_NEAR(Vec3(view * Vec4(0.0f, 0.0f, 0.0f, 1.0f)), Vec3(0.0f, 0.0f, -5.0f), 1e-5f);
        CHECK_NEAR(Vec3(view * Vec4(1.0f, 0.0f, 0.0f, 1.0f)), Vec3(1.0f, 0.0f, -5.0f), 1e-5f);

        const Mat4 fromSide = lookAt(Vec3(10.0f, 0.0f, 0.0f), Vec3(0.0f));
        CHECK_NEAR(Vec3(fromSide * Vec4(0.0f, 0.0f, 0.0f, 1.0f)), Vec3(0.0f, 0.0f, -10.0f), 1e-5f);
        CHECK_NEAR(Vec3(fromSide * Vec4(0.0f, 1.0f, 0.0f, 1.0f)), Vec3(0.0f, 1.0f, -10.0f), 1e-5f);
    }

    TEST_CASE("halton produces the van der Corput sequence") {
        CHECK(halton(1, 2) == doctest::Approx(0.5f));
        CHECK(halton(2, 2) == doctest::Approx(0.25f));
        CHECK(halton(3, 2) == doctest::Approx(0.75f));
        CHECK(halton(4, 2) == doctest::Approx(0.125f));
        CHECK(halton(5, 2) == doctest::Approx(0.625f));

        CHECK(halton(1, 3) == doctest::Approx(1.0f / 3.0f));
        CHECK(halton(2, 3) == doctest::Approx(2.0f / 3.0f));
        CHECK(halton(3, 3) == doctest::Approx(1.0f / 9.0f));
        CHECK(halton(0, 2) == 0.0f);
    }

    TEST_CASE("temporalJitter stays within half a pixel and repeats") {
        for (uint32_t frame = 0; frame < 64; ++frame) {
            const Vec2 jitter = temporalJitter(frame, 8);
            CHECK(jitter.x >= -0.5f);
            CHECK(jitter.x < 0.5f);
            CHECK(jitter.y >= -0.5f);
            CHECK(jitter.y < 0.5f);
            CHECK(jitter == temporalJitter(frame + 8, 8));
        }
        CHECK(temporalJitter(0, 8) != temporalJitter(1, 8));
        // A sample count of zero must not divide by zero.
        CHECK_NOTHROW((void)temporalJitter(5, 0));
    }
}
