#include "cordite/math/Frustum.h"

#include "cordite/math/Projection.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

using namespace cordite;

namespace {

constexpr float kNear = 1.0f;
constexpr float kFar = 100.0f;

/// A camera at the origin looking down -Z with a 90 degree field of view, so the
/// frustum edges lie on |x| = |y| = -z.
Mat4 testViewProjection() {
    return perspectiveReverseZ(toRadians(90.0f), 1.0f, kNear, kFar) * lookAt(Vec3(0.0f), kWorldForward);
}

} // namespace

TEST_SUITE("Frustum") {
    TEST_CASE("contains classifies points against all six planes") {
        const Frustum frustum = Frustum::fromViewProjection(testViewProjection());

        CHECK(frustum.contains(Vec3(0.0f, 0.0f, -10.0f)));
        CHECK(frustum.contains(Vec3(9.9f, 0.0f, -10.0f)));
        CHECK(frustum.contains(Vec3(0.0f, -9.9f, -10.0f)));

        CHECK_FALSE(frustum.contains(Vec3(10.1f, 0.0f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(-10.1f, 0.0f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 10.1f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, -10.1f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, -0.5f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, -100.5f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, 10.0f)));
    }

    TEST_CASE("planes are normalized and face inward") {
        const Frustum frustum = Frustum::fromViewProjection(testViewProjection());
        const Vec3 inside(0.0f, 0.0f, -50.0f);
        for (const Plane& plane : frustum.planes()) {
            CHECK(glm::length(plane.normal) == doctest::Approx(1.0f));
            CHECK(plane.signedDistance(inside) > 0.0f);
        }
    }

    TEST_CASE("boxes are culled only when fully outside") {
        const Frustum frustum = Frustum::fromViewProjection(testViewProjection());

        CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -10.0f), Vec3(1.0f))));
        // Straddles the right plane.
        CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(10.5f, 0.0f, -10.0f), Vec3(1.0f))));
        // Encloses the whole frustum.
        CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1000.0f))));

        CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, 10.0f), Vec3(1.0f))));
        CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(15.0f, 0.0f, -10.0f), Vec3(1.0f))));
        CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f, -15.0f, -10.0f), Vec3(1.0f))));
        CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -150.0f), Vec3(1.0f))));
        CHECK_FALSE(frustum.intersects(Aabb()));
    }

    TEST_CASE("spheres are culled only when fully outside") {
        const Frustum frustum = Frustum::fromViewProjection(testViewProjection());

        CHECK(frustum.intersects(Sphere{Vec3(0.0f, 0.0f, -10.0f), 1.0f}));
        CHECK(frustum.intersects(Sphere{Vec3(11.0f, 0.0f, -10.0f), 1.0f}));
        CHECK(frustum.intersects(Sphere{Vec3(0.0f, 0.0f, 0.5f), 2.0f}));

        CHECK_FALSE(frustum.intersects(Sphere{Vec3(14.0f, 0.0f, -10.0f), 1.0f}));
        CHECK_FALSE(frustum.intersects(Sphere{Vec3(0.0f, 0.0f, 5.0f), 1.0f}));
        CHECK_FALSE(frustum.intersects(Sphere{Vec3(0.0f, 0.0f, -110.0f), 1.0f}));
    }

    TEST_CASE("culling follows the camera transform") {
        const Vec3 eye(50.0f, 10.0f, 20.0f);
        const Vec3 target(50.0f, 10.0f, 120.0f);
        const Mat4 viewProjection =
            perspectiveReverseZ(toRadians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f) * lookAt(eye, target);
        const Frustum frustum = Frustum::fromViewProjection(viewProjection);

        CHECK(frustum.contains(Vec3(50.0f, 10.0f, 70.0f)));
        CHECK_FALSE(frustum.contains(Vec3(50.0f, 10.0f, -30.0f)));
        CHECK(frustum.intersects(Aabb::fromCenterExtents(target, Vec3(2.0f))));
        CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(2.0f))));
    }

    TEST_CASE("an infinite far plane never culls by distance") {
        const Mat4 viewProjection =
            perspectiveInfiniteReverseZ(toRadians(90.0f), 1.0f, kNear) * lookAt(Vec3(0.0f), kWorldForward);
        const Frustum frustum = Frustum::fromViewProjection(viewProjection);

        CHECK(frustum.contains(Vec3(0.0f, 0.0f, -1.0e6f)));
        CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -1.0e5f), Vec3(1.0f))));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, -0.5f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, 5.0f)));
        CHECK_FALSE(frustum.intersects(Sphere{Vec3(100.0f, 0.0f, -10.0f), 1.0f}));
    }

    TEST_CASE("conventional depth projections give the same frustum") {
        const Mat4 view = lookAt(Vec3(0.0f), kWorldForward);
        const Frustum reversed =
            Frustum::fromViewProjection(perspectiveReverseZ(toRadians(90.0f), 1.0f, kNear, kFar) * view);
        const Frustum standard = Frustum::fromViewProjection(perspective(toRadians(90.0f), 1.0f, kNear, kFar) * view);

        const Vec3 probes[] = {
            {0.0f, 0.0f, -10.0f}, {9.0f, 0.0f, -10.0f},  {11.0f, 0.0f, -10.0f},
            {0.0f, 0.0f, -0.5f},  {0.0f, 0.0f, -100.5f}, {0.0f, 0.0f, 3.0f},
        };
        for (const Vec3& probe : probes) {
            CHECK(reversed.contains(probe) == standard.contains(probe));
        }
    }

    TEST_CASE("orthographic frustums are boxes") {
        const Mat4 viewProjection =
            orthographicReverseZ(-5.0f, 5.0f, -2.0f, 2.0f, 1.0f, 20.0f) * lookAt(Vec3(0.0f), kWorldForward);
        const Frustum frustum = Frustum::fromViewProjection(viewProjection);

        CHECK(frustum.contains(Vec3(4.9f, 1.9f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(5.1f, 0.0f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 2.1f, -10.0f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, -20.5f)));
        CHECK_FALSE(frustum.contains(Vec3(0.0f, 0.0f, -0.5f)));
    }

    TEST_CASE("frustumCorners returns the corners of the view volume") {
        const Mat4 viewProjection = testViewProjection();
        const std::array<Vec3, 8> corners = frustumCorners(glm::inverse(viewProjection));

        // Reversed depth: clip depth 0 is the far plane, depth 1 the near plane.
        for (size_t index = 0; index < 4; ++index) {
            CHECK(corners[index].z == doctest::Approx(-kFar).epsilon(1e-3));
            CHECK(std::abs(corners[index].x) == doctest::Approx(kFar).epsilon(1e-3));
            CHECK(std::abs(corners[index].y) == doctest::Approx(kFar).epsilon(1e-3));
        }
        for (size_t index = 4; index < 8; ++index) {
            CHECK(corners[index].z == doctest::Approx(-kNear).epsilon(1e-3));
            CHECK(std::abs(corners[index].x) == doctest::Approx(kNear).epsilon(1e-3));
            CHECK(std::abs(corners[index].y) == doctest::Approx(kNear).epsilon(1e-3));
        }

        // Order within a group: (-x,-y), (+x,-y), (-x,+y), (+x,+y).
        CHECK(corners[4].x < 0.0f);
        CHECK(corners[4].y < 0.0f);
        CHECK(corners[5].x > 0.0f);
        CHECK(corners[5].y < 0.0f);
        CHECK(corners[6].x < 0.0f);
        CHECK(corners[6].y > 0.0f);
        CHECK(corners[7].x > 0.0f);
        CHECK(corners[7].y > 0.0f);
    }
}
