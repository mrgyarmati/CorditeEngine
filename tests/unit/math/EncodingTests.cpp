#include "cordite/math/Encoding.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

using namespace cordite;

TEST_SUITE("Encoding") {
    TEST_CASE("octahedral encoding round-trips the axes") {
        const Vec3 axes[] = {
            {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
        };
        for (const Vec3& axis : axes) {
            const Vec2 encoded = encodeOctahedral(axis);
            CHECK(encoded.x >= 0.0f);
            CHECK(encoded.x <= 1.0f);
            CHECK(encoded.y >= 0.0f);
            CHECK(encoded.y <= 1.0f);
            CHECK_NEAR(decodeOctahedral(encoded), axis, 1e-5f);
        }
    }

    TEST_CASE("octahedral encoding round-trips arbitrary directions") {
        Random random(31337);
        for (int iteration = 0; iteration < 5000; ++iteration) {
            const Vec3 normal = random.onUnitSphere();
            const Vec2 encoded = encodeOctahedral(normal);
            REQUIRE(encoded.x >= 0.0f);
            REQUIRE(encoded.x <= 1.0f);
            REQUIRE(encoded.y >= 0.0f);
            REQUIRE(encoded.y <= 1.0f);

            const Vec3 decoded = decodeOctahedral(encoded);
            REQUIRE(glm::length(decoded) == doctest::Approx(1.0f).epsilon(1e-4));
            REQUIRE(glm::dot(decoded, normal) > 0.99999f);
        }
    }

    TEST_CASE("octahedral encoding survives 16-bit quantization") {
        // The G-buffer stores the two channels as 16-bit unsigned normalized values.
        Random random(404);
        float worstAngle = 0.0f;
        for (int iteration = 0; iteration < 5000; ++iteration) {
            const Vec3 normal = random.onUnitSphere();
            const Vec2 quantized = glm::round(encodeOctahedral(normal) * 65535.0f) / 65535.0f;
            const float cosine = glm::clamp(glm::dot(decodeOctahedral(quantized), normal), -1.0f, 1.0f);
            worstAngle = glm::max(worstAngle, toDegrees(std::acos(cosine)));
        }
        CHECK(worstAngle < 0.05f);
    }
}
