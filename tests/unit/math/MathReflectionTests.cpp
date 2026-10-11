#include "cordite/math/MathReflection.h"

#include "cordite/math/Transform.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::sameRotation;
using cordite::test::unwrap;

namespace {

struct Shape {
    Vec2 size{1.0f, 2.0f};
    Vec3 position{0.0f};
    Vec4 tint{1.0f};
    Quat orientation = kIdentityQuat;
    std::vector<Vec3> points;
};

TypeInfo makeShapeType() {
    return TypeBuilder<Shape>("Shape")
        .property("size", &Shape::size, {.minimum = 0.0})
        .property("position", &Shape::position, {.unit = "m"})
        .property("tint", &Shape::tint, {.semantic = PropertySemantic::Color, .minimum = 0.0, .maximum = 1.0})
        .property("orientation", &Shape::orientation)
        .property("points", &Shape::points, {.minimum = -100.0, .maximum = 100.0})
        .build();
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

} // namespace

TEST_SUITE("MathReflection") {
    TEST_CASE("math members have vector property types") {
        const TypeInfo type = makeShapeType();
        CHECK(type.findProperty("size")->type() == PropertyType::Vec2);
        CHECK(type.findProperty("position")->type() == PropertyType::Vec3);
        CHECK(type.findProperty("tint")->type() == PropertyType::Vec4);
        CHECK(type.findProperty("orientation")->type() == PropertyType::Quat);
        CHECK(type.findProperty("points")->type() == PropertyType::List);
        CHECK(type.findProperty("points")->elementType() == PropertyType::Vec3);
        CHECK(type.describe()["properties"][2]["semantic"] == "color");
    }

    TEST_CASE("math members round-trip through JSON") {
        const TypeInfo type = makeShapeType();
        Shape original;
        original.size = Vec2(3.0f, 4.0f);
        original.position = Vec3(1.0f, -2.0f, 3.5f);
        original.tint = Vec4(0.1f, 0.2f, 0.3f, 0.4f);
        original.orientation = glm::angleAxis(toRadians(40.0f), glm::normalize(Vec3(1.0f, 2.0f, 3.0f)));
        original.points = {Vec3(1.0f), Vec3(2.0f, 3.0f, 4.0f)};

        Shape restored;
        REQUIRE(type.fromJson(&restored, type.toJson(&original)).has_value());
        CHECK(restored.size == original.size);
        CHECK(restored.position == original.position);
        CHECK(restored.tint == original.tint);
        CHECK(sameRotation(restored.orientation, original.orientation, 1e-6f));
        CHECK(restored.points == original.points);
    }

    TEST_CASE("vector components are written without float noise") {
        const TypeInfo type = makeShapeType();
        Shape shape;
        shape.position = Vec3(0.1f, -2.5f, 1.0f / 3.0f);
        shape.tint = Vec4(0.2f, 0.4f, 0.6f, 0.8f);

        const Json saved = type.toJson(&shape);
        CHECK(dumpJson(saved["position"], -1) == "[0.1,-2.5,0.33333334]");
        CHECK(dumpJson(saved["tint"], -1) == "[0.2,0.4,0.6,0.8]");
    }

    TEST_CASE("each component of a vector is checked against the range") {
        const TypeInfo type = makeShapeType();
        Shape shape;

        const Status negative = type.fromJson(&shape, json(R"({"size": [1, -1]})"));
        REQUIRE_FALSE(negative.has_value());
        CHECK(negative.error().code == ErrorCode::OutOfRange);
        CHECK(negative.error().message.find("size") != std::string::npos);

        CHECK_FALSE(type.fromJson(&shape, json(R"({"tint": [0.5, 0.5, 1.5, 1]})")).has_value());
        CHECK(type.fromJson(&shape, json(R"({"tint": [0, 0.5, 1, 1]})")).has_value());
        // Position has no range.
        CHECK(type.fromJson(&shape, json(R"({"position": [-1e6, 0, 1e6]})")).has_value());
    }

    TEST_CASE("the range reaches the components of every vector in a list") {
        const TypeInfo type = makeShapeType();
        Shape shape;

        const Status status = type.fromJson(&shape, json(R"({"points": [[1, 2, 3], [4, 500, 6]]})"));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
        CHECK(shape.points.empty());

        REQUIRE(type.fromJson(&shape, json(R"({"points": [[1, 2, 3], [-100, 100, 0]]})")).has_value());
        CHECK(shape.points == std::vector<Vec3>{Vec3(1.0f, 2.0f, 3.0f), Vec3(-100.0f, 100.0f, 0.0f)});
    }

    TEST_CASE("malformed vectors are rejected as invalid arguments naming the property") {
        const TypeInfo type = makeShapeType();
        Shape shape;
        const Vec3 before = shape.position;

        for (const char* text : {R"({"position": [1, 2]})", R"({"position": "origin"})", R"({"position": [1, 2, "z"]})",
                                 R"({"position": [1, 2, 3, 4]})", R"({"orientation": [0, 0, 0, 0]})",
                                 R"({"points": [[1, 2, 3], [4, 5]]})"}) {
            CAPTURE(text);
            const Status status = type.fromJson(&shape, json(text));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
            CHECK(status.error().message.find("Shape: property '") == 0);
        }
        CHECK(shape.position == before);
        CHECK(shape.points.empty());
    }

    TEST_CASE("quaternions are normalized when read") {
        const TypeInfo type = makeShapeType();
        Shape shape;
        REQUIRE(type.fromJson(&shape, json(R"({"orientation": [0, 2, 0, 0]})")).has_value());
        CHECK(sameRotation(shape.orientation, Quat(0.0f, 0.0f, 1.0f, 0.0f), 1e-6f));
        CHECK(glm::length(shape.orientation) == doctest::Approx(1.0f));
    }

    TEST_CASE("Transform can be described as a reflected type") {
        const TypeInfo type = TypeBuilder<Transform>("Transform")
                                  .property("position", &Transform::position)
                                  .property("rotation", &Transform::rotation)
                                  .property("scale", &Transform::scale)
                                  .build();

        Transform transform;
        REQUIRE(type.fromJson(&transform, json(R"({"position": [1, 2, 3], "scale": [2, 2, 2]})")).has_value());
        CHECK(transform.position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(transform.scale == Vec3(2.0f));
        CHECK(transform.rotation == kIdentityQuat);
    }
}
