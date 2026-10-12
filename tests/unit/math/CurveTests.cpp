#include "cordite/math/Curve.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>

using namespace cordite;

TEST_SUITE("Curve") {
    TEST_CASE("an empty curve evaluates to zero and a single key is constant") {
        FloatCurve curve;
        CHECK(curve.empty());
        CHECK(curve.evaluate(1.0f) == 0.0f);
        CHECK(curve.duration() == 0.0f);

        curve.addKey(2.0f, 5.0f);
        CHECK_FALSE(curve.empty());
        CHECK(curve.evaluate(-10.0f) == 5.0f);
        CHECK(curve.evaluate(2.0f) == 5.0f);
        CHECK(curve.evaluate(10.0f) == 5.0f);
    }

    TEST_CASE("keys stay sorted and a key at an existing time replaces it") {
        FloatCurve curve;
        curve.addKey(2.0f, 20.0f);
        curve.addKey(0.0f, 0.0f);
        curve.addKey(1.0f, 10.0f);
        curve.addKey(1.0f, 15.0f);

        REQUIRE(curve.keys().size() == 3);
        CHECK(curve.keys()[0].time == 0.0f);
        CHECK(curve.keys()[1].time == 1.0f);
        CHECK(curve.keys()[1].value == 15.0f);
        CHECK(curve.keys()[2].time == 2.0f);
        CHECK(curve.startTime() == 0.0f);
        CHECK(curve.endTime() == 2.0f);
        CHECK(curve.duration() == 2.0f);
    }

    TEST_CASE("removeKey and clear") {
        FloatCurve curve;
        curve.addKey(0.0f, 1.0f);
        curve.addKey(1.0f, 2.0f);

        CHECK_FALSE(curve.removeKey(5));
        CHECK(curve.removeKey(0));
        REQUIRE(curve.keys().size() == 1);
        CHECK(curve.keys()[0].value == 2.0f);

        curve.clear();
        CHECK(curve.empty());
    }

    TEST_CASE("linear segments interpolate and clamp outside the keys") {
        FloatCurve curve;
        curve.addKey(0.0f, 0.0f);
        curve.addKey(2.0f, 10.0f);
        curve.addKey(4.0f, -10.0f);

        CHECK(curve.evaluate(0.0f) == 0.0f);
        CHECK(curve.evaluate(1.0f) == doctest::Approx(5.0f));
        CHECK(curve.evaluate(2.0f) == doctest::Approx(10.0f));
        CHECK(curve.evaluate(3.0f) == doctest::Approx(0.0f));
        CHECK(curve.evaluate(4.0f) == -10.0f);
        CHECK(curve.evaluate(-5.0f) == 0.0f);
        CHECK(curve.evaluate(99.0f) == -10.0f);
    }

    TEST_CASE("step segments hold the value until the next key") {
        FloatCurve curve;
        curve.addKey(0.0f, 1.0f, Interpolation::Step);
        curve.addKey(1.0f, 2.0f, Interpolation::Step);
        curve.addKey(2.0f, 3.0f);

        CHECK(curve.evaluate(0.0f) == 1.0f);
        CHECK(curve.evaluate(0.999f) == 1.0f);
        CHECK(curve.evaluate(1.0f) == 2.0f);
        CHECK(curve.evaluate(1.5f) == 2.0f);
        CHECK(curve.evaluate(2.0f) == 3.0f);
    }

    TEST_CASE("cubic segments with flat tangents ease in and out") {
        FloatCurve curve;
        curve.addKey(0.0f, 0.0f, Interpolation::Cubic);
        curve.addKey(1.0f, 1.0f, Interpolation::Cubic);

        CHECK(curve.evaluate(0.5f) == doctest::Approx(0.5f));
        // Flat tangents: slower than linear near the ends.
        CHECK(curve.evaluate(0.1f) < 0.1f);
        CHECK(curve.evaluate(0.9f) > 0.9f);
        CHECK(curve.evaluate(0.25f) == doctest::Approx(0.15625f));
    }

    TEST_CASE("cubic segments honour explicit tangents") {
        // Tangents equal to the chord slope turn the Hermite segment into a line.
        FloatCurve curve;
        Keyframe<float> start;
        start.time = 0.0f;
        start.value = 0.0f;
        start.outTangent = 2.0f;
        start.interpolation = Interpolation::Cubic;
        Keyframe<float> end;
        end.time = 3.0f;
        end.value = 6.0f;
        end.inTangent = 2.0f;
        curve.addKey(start);
        curve.addKey(end);

        for (const float time : {0.5f, 1.0f, 1.7f, 2.9f}) {
            CHECK(curve.evaluate(time) == doctest::Approx(2.0f * time));
        }
    }

    TEST_CASE("computeSmoothTangents gives a curve through every key") {
        FloatCurve curve;
        curve.addKey(0.0f, 0.0f, Interpolation::Cubic);
        curve.addKey(1.0f, 4.0f, Interpolation::Cubic);
        curve.addKey(2.0f, 2.0f, Interpolation::Cubic);
        curve.addKey(3.0f, 6.0f, Interpolation::Cubic);
        curve.computeSmoothTangents();

        CHECK(curve.evaluate(1.0f) == doctest::Approx(4.0f));
        CHECK(curve.evaluate(2.0f) == doctest::Approx(2.0f));
        // Interior tangent is the slope between the neighbours: (2 - 0) / (2 - 0).
        CHECK(curve.keys()[1].outTangent == doctest::Approx(1.0f));
        CHECK(curve.keys()[1].inTangent == doctest::Approx(1.0f));
        // End tangents use the one available neighbour.
        CHECK(curve.keys()[0].outTangent == doctest::Approx(4.0f));

        // Continuity: values just either side of a key are close.
        CHECK(curve.evaluate(1.0f - 1e-3f) == doctest::Approx(curve.evaluate(1.0f + 1e-3f)).epsilon(1e-2));
    }

    TEST_CASE("Loop repeats the curve") {
        FloatCurve curve;
        curve.addKey(1.0f, 0.0f);
        curve.addKey(3.0f, 10.0f);
        curve.setWrapMode(WrapMode::Loop);
        CHECK(curve.wrapMode() == WrapMode::Loop);

        CHECK(curve.evaluate(2.0f) == doctest::Approx(5.0f));
        CHECK(curve.evaluate(4.0f) == doctest::Approx(5.0f));
        CHECK(curve.evaluate(8.5f) == doctest::Approx(7.5f));
        CHECK(curve.evaluate(0.0f) == doctest::Approx(5.0f));
        CHECK(curve.evaluate(-1.5f) == doctest::Approx(7.5f));
    }

    TEST_CASE("PingPong plays forward then backward") {
        FloatCurve curve;
        curve.addKey(0.0f, 0.0f);
        curve.addKey(2.0f, 10.0f);
        curve.setWrapMode(WrapMode::PingPong);

        CHECK(curve.evaluate(1.0f) == doctest::Approx(5.0f));
        CHECK(curve.evaluate(2.0f) == doctest::Approx(10.0f));
        CHECK(curve.evaluate(2.5f) == doctest::Approx(7.5f));
        CHECK(curve.evaluate(4.0f) == doctest::Approx(0.0f));
        CHECK(curve.evaluate(4.5f) == doctest::Approx(2.5f));
        CHECK(curve.evaluate(-0.5f) == doctest::Approx(2.5f));
    }

    TEST_CASE("wrapping a curve without duration does not divide by zero") {
        FloatCurve curve;
        curve.addKey(1.0f, 3.0f);
        curve.setWrapMode(WrapMode::Loop);
        CHECK(curve.evaluate(100.0f) == 3.0f);
    }

    TEST_CASE("non-finite times never read outside the keys") {
        FloatCurve curve;
        curve.addKey(0.0f, 1.0f);
        curve.addKey(1.0f, 2.0f);
        curve.addKey(2.0f, 3.0f);

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        for (const WrapMode mode : {WrapMode::Clamp, WrapMode::Loop, WrapMode::PingPong}) {
            curve.setWrapMode(mode);
            CHECK(curve.evaluate(nan) == 1.0f);
            CHECK(std::isfinite(curve.evaluate(infinity)));
            CHECK(std::isfinite(curve.evaluate(-infinity)));
        }

        curve.setWrapMode(WrapMode::Clamp);
        CHECK(curve.evaluate(infinity) == 3.0f);
        CHECK(curve.evaluate(-infinity) == 1.0f);
    }

    TEST_CASE("vector curves interpolate per component") {
        Vec3Curve path;
        path.addKey(0.0f, Vec3(0.0f));
        path.addKey(2.0f, Vec3(2.0f, 4.0f, -6.0f));
        CHECK_NEAR(path.evaluate(1.0f), Vec3(1.0f, 2.0f, -3.0f), 1e-6f);

        ColorCurve gradient;
        gradient.addKey(0.0f, Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        gradient.addKey(1.0f, Vec4(0.0f, 0.0f, 1.0f, 0.0f));
        CHECK_NEAR(gradient.evaluate(0.25f), Vec4(0.75f, 0.0f, 0.25f, 0.75f), 1e-6f);
    }

    TEST_CASE("curves are equal when their keys are, and they wrap the same way") {
        Vec3Curve first;
        first.addKey(0.0f, Vec3(0.0f));
        first.addKey(2.0f, Vec3(2.0f, 4.0f, -6.0f), Interpolation::Step);
        Vec3Curve second;
        // Whatever order the keys were added in.
        second.addKey(2.0f, Vec3(2.0f, 4.0f, -6.0f), Interpolation::Step);
        second.addKey(0.0f, Vec3(0.0f));
        CHECK(first == second);
        CHECK(Vec3Curve() == Vec3Curve());
        CHECK_FALSE(first == Vec3Curve());

        // A value, a time, a slope, the way to the next key, the wrapping: each tells
        // two curves apart.
        Vec3Curve other = first;
        other.addKey(2.0f, Vec3(2.0f, 4.0f, -6.5f), Interpolation::Step);
        CHECK_FALSE(first == other);
        other = first;
        other.addKey(1.0f, Vec3(1.0f));
        CHECK_FALSE(first == other);
        other = first;
        other.addKey(2.0f, Vec3(2.0f, 4.0f, -6.0f), Interpolation::Linear);
        CHECK_FALSE(first == other);
        other = first;
        other.computeSmoothTangents();
        CHECK_FALSE(first == other);
        other = first;
        other.setWrapMode(WrapMode::Loop);
        CHECK_FALSE(first == other);
        other.setWrapMode(WrapMode::Clamp);
        CHECK(first == other);
    }
}
