#include "cordite/math/MathJson.h"

#include "cordite/core/Json.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <limits>

using namespace cordite;
using cordite::test::sameRotation;

TEST_SUITE("MathJson") {
    TEST_CASE("vectors serialize as arrays and round-trip") {
        CHECK(dumpJson(toJson(Vec2(1.0f, 2.0f)), -1) == "[1.0,2.0]");
        CHECK(dumpJson(toJson(Vec3(1.0f, 2.0f, 3.0f)), -1) == "[1.0,2.0,3.0]");
        CHECK(dumpJson(toJson(Vec4(1.0f, 2.0f, 3.0f, 4.0f)), -1) == "[1.0,2.0,3.0,4.0]");

        Vec2 two;
        REQUIRE(fromJson(toJson(Vec2(0.5f, -1.5f)), two).has_value());
        CHECK(two == Vec2(0.5f, -1.5f));

        Vec3 three;
        REQUIRE(fromJson(toJson(Vec3(0.1f, 0.2f, 0.3f)), three).has_value());
        CHECK(three == Vec3(0.1f, 0.2f, 0.3f));

        Vec4 four;
        REQUIRE(fromJson(toJson(Vec4(9.0f, 8.0f, 7.0f, 6.0f)), four).has_value());
        CHECK(four == Vec4(9.0f, 8.0f, 7.0f, 6.0f));
    }

    TEST_CASE("integers are accepted where numbers are expected") {
        Vec3 value;
        REQUIRE(fromJson(*parseJson("[1, 2, 3]"), value).has_value());
        CHECK(value == Vec3(1.0f, 2.0f, 3.0f));
    }

    TEST_CASE("quaternions are stored as x, y, z, w") {
        const Quat rotation = glm::angleAxis(toRadians(90.0f), kWorldUp);
        const Json json = toJson(rotation);
        REQUIRE(json.size() == 4);
        CHECK(json[0].get<float>() == doctest::Approx(0.0f));
        CHECK(json[1].get<float>() == doctest::Approx(rotation.y));
        CHECK(json[3].get<float>() == doctest::Approx(rotation.w));

        Quat parsed = kIdentityQuat;
        REQUIRE(fromJson(json, parsed).has_value());
        CHECK(sameRotation(parsed, rotation, 1e-6f));
    }

    TEST_CASE("quaternions are normalized and a zero quaternion is rejected") {
        Quat parsed = kIdentityQuat;
        REQUIRE(fromJson(*parseJson("[0, 2, 0, 2]"), parsed).has_value());
        CHECK(glm::length(parsed) == doctest::Approx(1.0f));
        CHECK(sameRotation(parsed, glm::angleAxis(toRadians(90.0f), kWorldUp), 1e-5f));

        const Status zero = fromJson(*parseJson("[0, 0, 0, 0]"), parsed);
        REQUIRE_FALSE(zero.has_value());
        CHECK(zero.error().code == ErrorCode::ParseError);
    }

    TEST_CASE("unit quaternions and vectors are read back bit for bit") {
        // Saving, loading and saving again must not change a file, so nothing may be
        // "improved" on the way in.
        for (const float degrees : {1.0f, 30.0f, 77.7f, 123.4f, 179.0f}) {
            CAPTURE(degrees);
            const Quat rotation = glm::angleAxis(toRadians(degrees), glm::normalize(Vec3(0.3f, -0.8f, 0.5f)));
            Quat parsed = kIdentityQuat;
            REQUIRE(fromJson(*parseJson(dumpJson(toJson(rotation))), parsed).has_value());
            CHECK(parsed == rotation);
        }

        const Vec3 position(0.1f, -1234.5678f, 1.0f / 3.0f);
        Vec3 parsedPosition(0.0f);
        REQUIRE(fromJson(*parseJson(dumpJson(toJson(position))), parsedPosition).has_value());
        CHECK(parsedPosition == position);
    }

    TEST_CASE("malformed vectors are rejected and leave the output untouched") {
        Vec3 value(7.0f, 8.0f, 9.0f);

        CHECK_FALSE(fromJson(*parseJson("[1, 2]"), value).has_value());
        CHECK_FALSE(fromJson(*parseJson("[1, 2, 3, 4]"), value).has_value());
        CHECK_FALSE(fromJson(*parseJson(R"([1, "two", 3])"), value).has_value());
        CHECK_FALSE(fromJson(*parseJson(R"({"x": 1, "y": 2, "z": 3})"), value).has_value());
        CHECK_FALSE(fromJson(*parseJson("null"), value).has_value());
        CHECK_FALSE(fromJson(*parseJson("[1, null, 3]"), value).has_value());

        CHECK(value == Vec3(7.0f, 8.0f, 9.0f));
    }

    TEST_CASE("non-finite numbers are rejected") {
        Vec3 value(1.0f);
        const Json withNan = Json::array({1.0, std::numeric_limits<double>::quiet_NaN(), 3.0});
        CHECK_FALSE(fromJson(withNan, value).has_value());

        const Json withInfinity = Json::array({1.0, 2.0, std::numeric_limits<double>::infinity()});
        CHECK_FALSE(fromJson(withInfinity, value).has_value());

        // A double too large for float overflows to infinity when narrowed.
        CHECK_FALSE(fromJson(*parseJson("[1e300, 0, 0]"), value).has_value());
        CHECK(value == Vec3(1.0f));
    }

    TEST_CASE("transforms round-trip") {
        Transform original;
        original.position = Vec3(1.0f, 2.0f, 3.0f);
        original.rotation = glm::angleAxis(toRadians(35.0f), glm::normalize(Vec3(1.0f, 1.0f, 0.0f)));
        original.scale = Vec3(2.0f, 0.5f, 1.0f);

        const Json json = toJson(original);
        CHECK(json.contains("position"));
        CHECK(json.contains("rotation"));
        CHECK(json.contains("scale"));

        Transform parsed;
        REQUIRE(fromJson(json, parsed).has_value());
        CHECK_NEAR(parsed.position, original.position, 1e-6f);
        CHECK_NEAR(parsed.scale, original.scale, 1e-6f);
        CHECK(sameRotation(parsed.rotation, original.rotation, 1e-6f));
    }

    TEST_CASE("missing transform members keep their identity defaults") {
        Transform parsed;
        parsed.position = Vec3(99.0f);
        REQUIRE(fromJson(*parseJson(R"({"scale": [2, 2, 2]})"), parsed).has_value());
        CHECK(parsed.position == Vec3(0.0f));
        CHECK(parsed.rotation == kIdentityQuat);
        CHECK(parsed.scale == Vec3(2.0f));
    }

    TEST_CASE("an invalid transform member fails without partial updates") {
        Transform parsed;
        parsed.position = Vec3(5.0f);
        const Status status = fromJson(*parseJson(R"({"position": [1, 2, 3], "scale": "big"})"), parsed);
        REQUIRE_FALSE(status.has_value());
        CHECK(parsed.position == Vec3(5.0f));

        CHECK_FALSE(fromJson(*parseJson("[1, 2, 3]"), parsed).has_value());
    }

    TEST_CASE("bounding boxes round-trip and require both corners") {
        const Aabb original = Aabb::fromCenterExtents(Vec3(1.0f, 2.0f, 3.0f), Vec3(0.5f));
        Aabb parsed;
        REQUIRE(fromJson(toJson(original), parsed).has_value());
        CHECK(parsed == original);

        CHECK_FALSE(fromJson(*parseJson(R"({"min": [0, 0, 0]})"), parsed).has_value());
        CHECK_FALSE(fromJson(*parseJson(R"({"min": [0, 0, 0], "max": [1, 1]})"), parsed).has_value());
        CHECK(parsed == original);
    }
}
