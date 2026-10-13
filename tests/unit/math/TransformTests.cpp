#include "cordite/math/Transform.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <limits>

using namespace cordite;
using cordite::test::sameRotation;

TEST_SUITE("Transform") {
    TEST_CASE("the default transform is the identity") {
        const Transform transform;
        CHECK(test::nearlyEqual(transform.toMatrix(), Mat4(1.0f), 0.0f));
        CHECK_NEAR(transform.forward(), kWorldForward, 1e-6f);
        CHECK_NEAR(transform.right(), kWorldRight, 1e-6f);
        CHECK_NEAR(transform.up(), kWorldUp, 1e-6f);
    }

    TEST_CASE("scale is applied first, then rotation, then translation") {
        Transform transform;
        transform.scale = Vec3(2.0f);
        transform.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
        transform.position = Vec3(10.0f, 0.0f, 0.0f);

        // (1,0,0) scales to (2,0,0); a +90 degree turn about +Y takes +X to -Z.
        const Vec3 expected(10.0f, 0.0f, -2.0f);
        CHECK_NEAR(transform.transformPoint(Vec3(1.0f, 0.0f, 0.0f)), expected, 1e-5f);
        CHECK_NEAR(Vec3(transform.toMatrix() * Vec4(1.0f, 0.0f, 0.0f, 1.0f)), expected, 1e-5f);
    }

    TEST_CASE("transformDirection ignores scale and translation") {
        Transform transform;
        transform.scale = Vec3(5.0f);
        transform.position = Vec3(1.0f, 2.0f, 3.0f);
        transform.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
        CHECK_NEAR(transform.transformDirection(kWorldForward), Vec3(-1.0f, 0.0f, 0.0f), 1e-5f);
    }

    TEST_CASE("a positive yaw turns the forward axis to the left") {
        Transform transform;
        transform.rotation = quatFromEulerDegrees(Vec3(0.0f, 90.0f, 0.0f));
        CHECK_NEAR(transform.forward(), Vec3(-1.0f, 0.0f, 0.0f), 1e-5f);
    }

    TEST_CASE("a positive pitch tilts the forward axis up") {
        Transform transform;
        transform.rotation = quatFromEulerDegrees(Vec3(45.0f, 0.0f, 0.0f));
        CHECK(transform.forward().y > 0.7f);
        CHECK(transform.forward().z < -0.7f);
    }

    TEST_CASE("fromMatrix inverts toMatrix") {
        Random random(1234);
        for (int iteration = 0; iteration < 200; ++iteration) {
            Transform original;
            original.position =
                Vec3(random.range(-50.0f, 50.0f), random.range(-50.0f, 50.0f), random.range(-50.0f, 50.0f));
            original.rotation = glm::angleAxis(random.range(0.0f, kTwoPi), random.onUnitSphere());
            original.scale = Vec3(random.range(0.1f, 4.0f), random.range(0.1f, 4.0f), random.range(0.1f, 4.0f));

            const Transform decomposed = Transform::fromMatrix(original.toMatrix());
            CHECK_NEAR(decomposed.position, original.position, 1e-4f);
            CHECK_NEAR(decomposed.scale, original.scale, 1e-4f);
            CHECK(sameRotation(decomposed.rotation, original.rotation, 1e-4f));
        }
    }

    TEST_CASE("fromMatrix folds a mirror into the X scale") {
        Transform mirrored;
        mirrored.scale = Vec3(-1.5f, 2.0f, 0.5f);
        mirrored.rotation = glm::angleAxis(toRadians(30.0f), glm::normalize(Vec3(1.0f, 2.0f, 3.0f)));
        mirrored.position = Vec3(3.0f, -2.0f, 1.0f);
        const Mat4 matrix = mirrored.toMatrix();

        const Transform decomposed = Transform::fromMatrix(matrix);
        CHECK(decomposed.scale.x < 0.0f);
        CHECK(decomposed.scale.y > 0.0f);
        CHECK(decomposed.scale.z > 0.0f);
        CHECK(test::nearlyEqual(decomposed.toMatrix(), matrix, 1e-4f));
    }

    TEST_CASE("fromMatrix survives a zero scale axis") {
        Transform flattened;
        flattened.scale = Vec3(1.0f, 0.0f, 1.0f);
        const Transform decomposed = Transform::fromMatrix(flattened.toMatrix());

        CHECK(isFinite(decomposed.scale));
        CHECK(std::isfinite(decomposed.rotation.w));
        CHECK(decomposed.scale.y == doctest::Approx(0.0f));
        CHECK(glm::length(decomposed.rotation) == doctest::Approx(1.0f).epsilon(1e-4));
    }

    TEST_CASE("four numbers are made a rotation, and what is a rotation already is left bit for bit") {
        // Glm's quaternions are written (w, x, y, z).
        const Quat made = normalizeRotation(Quat(2.0f, 0.0f, 2.0f, 0.0f));
        CHECK(glm::length(made) == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(sameRotation(made, glm::angleAxis(toRadians(90.0f), kWorldUp), 1e-6f));

        // Of length one as far as single floats go: what comes back is what went in, to
        // the last bit - although it is not exactly of length one, and could be "improved".
        const Quat nearly(1.000001f, 0.0f, 0.0f, 0.0f);
        REQUIRE(nearly.w != 1.0f);
        CHECK(normalizeRotation(nearly) == nearly);
        for (const float degrees : {1.0f, 30.0f, 45.0f, 77.7f, 123.4f, 179.0f}) {
            CAPTURE(degrees);
            const Quat rotation = glm::angleAxis(toRadians(degrees), glm::normalize(Vec3(0.3f, -0.8f, 0.5f)));
            CHECK(normalizeRotation(rotation) == rotation);
        }
        // A little further off, it is made one.
        const Quat off(1.00001f, 0.0f, 0.0f, 0.0f);
        CHECK(normalizeRotation(off) != off);
        CHECK(normalizeRotation(off).w == doctest::Approx(1.0f).epsilon(1e-6));
        const Quat under(0.99999f, 0.0f, 0.0f, 0.0f);
        CHECK(normalizeRotation(under) != under);

        // What has no length, or none that is a number, is no rotation: it turns nothing.
        const float infinite = std::numeric_limits<float>::infinity();
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        for (const Quat& none :
             {Quat(0.0f, 0.0f, 0.0f, 0.0f), Quat(1.0e-7f, 0.0f, 0.0f, 0.0f), Quat(1.0f, infinite, 0.0f, 0.0f),
              Quat(notANumber, 0.0f, 0.0f, 1.0f), Quat(0.0f, 1.0e30f, 0.0f, 0.0f)}) {
            CHECK_FALSE(isRotation(none));
            CHECK(normalizeRotation(none) == kIdentityQuat);
        }
        // Small is not none, and neither is large - as long as its square is a number.
        CHECK(isRotation(Quat(0.0f, 0.0f, 1.0e-5f, 0.0f)));
        CHECK(sameRotation(normalizeRotation(Quat(0.0f, 0.0f, 1.0e-5f, 0.0f)), Quat(0.0f, 0.0f, 1.0f, 0.0f), 1e-6f));
        CHECK(isRotation(Quat(0.0f, 1.0e18f, 0.0f, 0.0f)));
        CHECK(normalizeRotation(Quat(0.0f, 1.0e18f, 0.0f, 0.0f)) == Quat(0.0f, 1.0f, 0.0f, 0.0f));
        CHECK(isRotation(kIdentityQuat));
    }

    TEST_CASE("interpolate blends position and scale and slerps rotation") {
        Transform from;
        Transform to;
        to.position = Vec3(10.0f, 0.0f, 0.0f);
        to.scale = Vec3(3.0f);
        to.rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);

        const Transform halfway = Transform::interpolate(from, to, 0.5f);
        CHECK_NEAR(halfway.position, Vec3(5.0f, 0.0f, 0.0f), 1e-5f);
        CHECK_NEAR(halfway.scale, Vec3(2.0f), 1e-5f);
        CHECK(sameRotation(halfway.rotation, glm::angleAxis(toRadians(45.0f), kWorldUp), 1e-5f));

        CHECK(Transform::interpolate(from, to, 0.0f) == from);
        CHECK_NEAR(Transform::interpolate(from, to, 1.0f).position, to.position, 1e-5f);
    }

    TEST_CASE("Euler angles round-trip away from the poles") {
        const Vec3 samples[] = {
            {0.0f, 0.0f, 0.0f},      {30.0f, 45.0f, 60.0f},  {-60.0f, 170.0f, -20.0f},
            {85.0f, -120.0f, 10.0f}, {-89.0f, 5.0f, 179.0f}, {10.0f, -179.0f, -179.0f},
        };
        for (const Vec3& degrees : samples) {
            const Quat rotation = quatFromEulerDegrees(degrees);
            const Vec3 recovered = eulerDegreesFromQuat(rotation);
            CHECK_NEAR(recovered, degrees, 1e-2f);
        }
    }

    TEST_CASE("Euler angles at the poles reproduce the same rotation") {
        for (const float pitch : {90.0f, -90.0f}) {
            const Quat rotation = quatFromEulerDegrees(Vec3(pitch, 40.0f, 25.0f));
            const Vec3 recovered = eulerDegreesFromQuat(rotation);
            CHECK(recovered.x == doctest::Approx(pitch).epsilon(1e-3));
            CHECK(recovered.z == doctest::Approx(0.0f));
            CHECK(sameRotation(quatFromEulerDegrees(recovered), rotation, 1e-3f));
        }
    }

    TEST_CASE("lookRotation aims the forward axis and keeps up upright") {
        Random random(99);
        for (int iteration = 0; iteration < 100; ++iteration) {
            const Vec3 direction = random.onUnitSphere();
            if (std::abs(direction.y) > 0.99f) {
                continue;
            }
            const Quat rotation = lookRotation(direction);
            CHECK_NEAR(rotation * kWorldForward, direction, 1e-4f);
            CHECK((rotation * kWorldUp).y > 0.0f);
            // No roll: the right axis stays horizontal.
            CHECK(std::abs((rotation * kWorldRight).y) < 1e-4f);
        }
    }

    TEST_CASE("lookRotation handles directions parallel to up and zero vectors") {
        for (const Vec3& direction : {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)}) {
            const Quat rotation = lookRotation(direction);
            CHECK(std::isfinite(rotation.w));
            CHECK_NEAR(rotation * kWorldForward, direction, 1e-4f);
        }

        const Quat fallback = lookRotation(Vec3(0.0f));
        CHECK_NEAR(fallback * kWorldForward, kWorldForward, 1e-5f);
    }
}
