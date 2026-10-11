#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <limits>

using namespace cordite;

TEST_SUITE("Random") {
    TEST_CASE("matches the PCG32 reference implementation") {
        // Output of pcg32-demo from the reference distribution, seeded with (42, 54).
        Random random(42, 54);
        CHECK(random.nextU32() == 0xa15c02b7u);
        CHECK(random.nextU32() == 0x7b47f409u);
        CHECK(random.nextU32() == 0xba1d3330u);
        CHECK(random.nextU32() == 0x83d2f293u);
        CHECK(random.nextU32() == 0xbfa4784bu);
        CHECK(random.nextU32() == 0xcbed606eu);
    }

    TEST_CASE("the same seed gives the same sequence; different seeds differ") {
        Random first(123);
        Random second(123);
        Random other(124);

        bool anyDifferent = false;
        for (int index = 0; index < 100; ++index) {
            const uint32_t value = first.nextU32();
            CHECK(value == second.nextU32());
            anyDifferent = anyDifferent || value != other.nextU32();
        }
        CHECK(anyDifferent);
    }

    TEST_CASE("nextFloat is in [0, 1) and roughly uniform") {
        Random random(5);
        double sum = 0.0;
        std::array<int, 10> buckets{};
        constexpr int kSamples = 100000;
        for (int index = 0; index < kSamples; ++index) {
            const float value = random.nextFloat();
            REQUIRE(value >= 0.0f);
            REQUIRE(value < 1.0f);
            sum += value;
            ++buckets[static_cast<size_t>(value * 10.0f)];
        }

        CHECK(sum / kSamples == doctest::Approx(0.5).epsilon(0.01));
        for (const int count : buckets) {
            CHECK(count > kSamples / 10 * 0.95);
            CHECK(count < kSamples / 10 * 1.05);
        }
    }

    TEST_CASE("range stays inside its bounds") {
        Random random(6);
        for (int index = 0; index < 10000; ++index) {
            const float value = random.range(-3.0f, 7.0f);
            REQUIRE(value >= -3.0f);
            REQUIRE(value < 7.0f);
        }
    }

    TEST_CASE("rangeInt is inclusive and reaches every value") {
        Random random(7);
        std::array<int, 6> counts{};
        for (int index = 0; index < 60000; ++index) {
            const int32_t value = random.rangeInt(1, 6);
            REQUIRE(value >= 1);
            REQUIRE(value <= 6);
            ++counts[static_cast<size_t>(value - 1)];
        }
        for (const int count : counts) {
            CHECK(count > 9500);
            CHECK(count < 10500);
        }
    }

    TEST_CASE("rangeInt handles degenerate and extreme ranges") {
        Random random(8);
        CHECK(random.rangeInt(5, 5) == 5);
        CHECK(random.rangeInt(9, 3) == 9);

        for (int index = 0; index < 1000; ++index) {
            const int32_t negative = random.rangeInt(-10, -5);
            REQUIRE(negative >= -10);
            REQUIRE(negative <= -5);
        }

        // The full int32 range must not overflow the span computation.
        const int32_t minimum = std::numeric_limits<int32_t>::min();
        const int32_t maximum = std::numeric_limits<int32_t>::max();
        bool sawNegative = false;
        bool sawPositive = false;
        for (int index = 0; index < 1000; ++index) {
            const int32_t value = random.rangeInt(minimum, maximum);
            sawNegative = sawNegative || value < 0;
            sawPositive = sawPositive || value > 0;
        }
        CHECK(sawNegative);
        CHECK(sawPositive);
    }

    TEST_CASE("sphere and circle sampling stay within their shapes") {
        Random random(9);
        Vec3 surfaceSum(0.0f);
        constexpr int kSamples = 20000;
        for (int index = 0; index < kSamples; ++index) {
            const Vec3 onSphere = random.onUnitSphere();
            REQUIRE(glm::length(onSphere) == doctest::Approx(1.0f).epsilon(1e-4));
            surfaceSum += onSphere;

            REQUIRE(glm::length(random.insideUnitSphere()) <= 1.0001f);
            REQUIRE(glm::length(random.insideUnitCircle()) <= 1.0001f);
        }

        // A uniform distribution on the sphere averages out to the centre.
        CHECK(glm::length(surfaceSum / static_cast<float>(kSamples)) < 0.03f);
    }
}
