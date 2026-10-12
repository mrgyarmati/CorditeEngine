#include "cordite/renderer/DebugDraw.h"

#include "cordite/math/Color.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

using namespace cordite;

namespace {

const Vec4 kWhite(1.0f);

float lineLength(std::span<const DebugVertex> vertices, size_t line) {
    return glm::distance(vertices[line * 2].position, vertices[line * 2 + 1].position);
}

} // namespace

TEST_SUITE("DebugDraw") {
    TEST_CASE("a line is two vertices sharing a color") {
        DebugDraw lines;
        CHECK(lines.isEmpty());

        const Vec4 color(1.0f, 0.5f, 0.0f, 0.25f);
        lines.line(Vec3(1.0f, 2.0f, 3.0f), Vec3(4.0f, 5.0f, 6.0f), color);

        REQUIRE(lines.vertices().size() == 2);
        CHECK(lines.lineCount() == 1);
        CHECK_FALSE(lines.isEmpty());
        CHECK(lines.vertices()[0].position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(lines.vertices()[1].position == Vec3(4.0f, 5.0f, 6.0f));
        CHECK(lines.vertices()[0].color == packRgba8(color));
        CHECK(lines.vertices()[1].color == packRgba8(color));
    }

    TEST_CASE("clear removes every line") {
        DebugDraw lines;
        lines.grid();
        REQUIRE_FALSE(lines.isEmpty());
        lines.clear();
        CHECK(lines.isEmpty());
        CHECK(lines.lineCount() == 0);
    }

    TEST_CASE("a box is twelve edges between its transformed corners") {
        DebugDraw lines;
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f, 2.0f, 3.0f));
        const Mat4 transform = glm::translate(Mat4(1.0f), Vec3(10.0f, 0.0f, 0.0f));
        lines.box(box, transform, kWhite);

        REQUIRE(lines.lineCount() == 12);
        float totalLength = 0.0f;
        for (size_t line = 0; line < lines.lineCount(); ++line) {
            totalLength += lineLength(lines.vertices(), line);
        }
        // Four edges along each axis: 4 * (2 + 4 + 6).
        CHECK(totalLength == doctest::Approx(48.0f));

        for (const DebugVertex& vertex : lines.vertices()) {
            CHECK(std::abs(vertex.position.x - 10.0f) == doctest::Approx(1.0f));
            CHECK(std::abs(vertex.position.y) == doctest::Approx(2.0f));
            CHECK(std::abs(vertex.position.z) == doctest::Approx(3.0f));
        }
    }

    TEST_CASE("an empty box draws nothing") {
        DebugDraw lines;
        lines.box(Aabb(), Mat4(1.0f), kWhite);
        CHECK(lines.isEmpty());
    }

    TEST_CASE("axes are red, green and blue lines of the requested length") {
        DebugDraw lines;
        // The scale must not stretch the lines.
        const Mat4 transform =
            Transform{Vec3(1.0f, 2.0f, 3.0f), glm::angleAxis(toRadians(90.0f), kWorldUp), Vec3(5.0f, 0.1f, 9.0f)}
                .toMatrix();
        lines.axes(transform, 0.5f);

        REQUIRE(lines.lineCount() == 3);
        const std::span<const DebugVertex> vertices = lines.vertices();
        for (size_t line = 0; line < 3; ++line) {
            CHECK_NEAR(vertices[line * 2].position, Vec3(1.0f, 2.0f, 3.0f), 1e-6f);
            CHECK(lineLength(vertices, line) == doctest::Approx(0.5f));
        }
        CHECK(vertices[0].color == packRgba8(DebugDraw::kRed));
        CHECK(vertices[2].color == packRgba8(DebugDraw::kGreen));
        CHECK(vertices[4].color == packRgba8(DebugDraw::kBlue));

        // Turned 90 degrees about +Y, the local X axis points along world -Z.
        CHECK_NEAR(vertices[1].position - vertices[0].position, Vec3(0.0f, 0.0f, -0.5f), 1e-5f);
        CHECK_NEAR(vertices[3].position - vertices[2].position, Vec3(0.0f, 0.5f, 0.0f), 1e-5f);
    }

    TEST_CASE("a cross is three lines through a point") {
        DebugDraw lines;
        lines.cross(Vec3(1.0f, 1.0f, 1.0f), 2.0f, kWhite);
        REQUIRE(lines.lineCount() == 3);
        for (size_t line = 0; line < 3; ++line) {
            CHECK(lineLength(lines.vertices(), line) == doctest::Approx(2.0f));
            const Vec3 middle = 0.5f * (lines.vertices()[line * 2].position + lines.vertices()[line * 2 + 1].position);
            CHECK_NEAR(middle, Vec3(1.0f), 1e-6f);
        }
    }

    TEST_CASE("a circle is a closed loop in the plane perpendicular to its normal") {
        const Vec3 center(1.0f, 2.0f, 3.0f);
        for (const Vec3 normal :
             {kWorldUp, kWorldRight, kWorldForward, glm::normalize(Vec3(1.0f, 1.0f, 1.0f)), Vec3(0.0f, -3.0f, 0.0f)}) {
            CAPTURE(test::describe(normal));
            DebugDraw lines;
            lines.circle(center, normal, 2.0f, kWhite, 16);

            REQUIRE(lines.lineCount() == 16);
            const std::span<const DebugVertex> vertices = lines.vertices();
            for (size_t line = 0; line < 16; ++line) {
                const Vec3 offset = vertices[line * 2].position - center;
                CHECK(glm::length(offset) == doctest::Approx(2.0f));
                CHECK(glm::dot(offset, glm::normalize(normal)) == doctest::Approx(0.0f));
                // Each segment starts where the previous one ended.
                const Vec3& previousEnd = vertices[((line + 15) % 16) * 2 + 1].position;
                CHECK_NEAR(vertices[line * 2].position, previousEnd, 1e-5f);
            }
        }
    }

    TEST_CASE("degenerate circle parameters still produce a shape") {
        DebugDraw lines;
        lines.circle(Vec3(0.0f), Vec3(0.0f), 1.0f, kWhite, 0);
        // At least a triangle, around a fallback axis.
        CHECK(lines.lineCount() == 3);
    }

    TEST_CASE("a sphere is three perpendicular circles") {
        DebugDraw lines;
        lines.sphere(Vec3(0.0f, 5.0f, 0.0f), 1.5f, kWhite, 12);
        REQUIRE(lines.lineCount() == 36);
        for (const DebugVertex& vertex : lines.vertices()) {
            CHECK(glm::distance(vertex.position, Vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(1.5f));
        }
    }

    TEST_CASE("a capsule is two circles, four lines between them and half circles over its ends") {
        DebugDraw draw;
        const Vec3 from(1.0f, 0.0f, 2.0f);
        const Vec3 to(1.0f, 3.0f, 2.0f);
        draw.capsule(from, to, 0.5f, DebugDraw::kGreen, 16);
        // Two circles of sixteen, four lines, four half circles of eight.
        CHECK(draw.lineCount() == 16 * 2 + 4 + 8 * 4);
        // Every point is half a metre from the line between the two ends - on the
        // cylinder, or on one of the round ends - and nothing reaches further.
        float lowest = 100.0f;
        float highest = -100.0f;
        for (const DebugVertex& vertex : draw.vertices()) {
            const float along = std::clamp(vertex.position.y, from.y, to.y);
            CHECK(glm::distance(vertex.position, Vec3(from.x, along, from.z)) == doctest::Approx(0.5f).epsilon(1.0e-4));
            CHECK(vertex.color == packRgba8(DebugDraw::kGreen));
            lowest = std::min(lowest, vertex.position.y);
            highest = std::max(highest, vertex.position.y);
        }
        // From end to end it is as long as the cylinder and both round ends.
        CHECK(lowest == doctest::Approx(-0.5f).epsilon(1.0e-4));
        CHECK(highest == doctest::Approx(3.5f).epsilon(1.0e-4));

        // With both ends in one place it is the outline of a ball, and still a shape.
        draw.clear();
        draw.capsule(from, from, 2.0f, DebugDraw::kRed, 1);
        CHECK(draw.lineCount() > 0);
        for (const DebugVertex& vertex : draw.vertices()) {
            CHECK(glm::distance(vertex.position, from) == doctest::Approx(2.0f).epsilon(1.0e-4));
        }
    }

    TEST_CASE("an arrow is a line with four short lines back from its tip") {
        const Vec3 from(1.0f, 2.0f, 3.0f);
        for (const Vec3 direction : {kWorldUp, kWorldRight, kWorldForward, glm::normalize(Vec3(1.0f, -2.0f, 0.5f))}) {
            CAPTURE(test::describe(direction));
            const Vec3 to = from + direction * 5.0f;
            DebugDraw lines;
            lines.arrow(from, to, kWhite);

            REQUIRE(lines.lineCount() == 5);
            const std::span<const DebugVertex> vertices = lines.vertices();
            CHECK(vertices[0].position == from);
            CHECK(vertices[1].position == to);
            Vec3 middle(0.0f);
            for (size_t line = 1; line < 5; ++line) {
                // From the tip back to a point a fifth of the arrow behind it and a
                // little to the side.
                CHECK(vertices[line * 2].position == to);
                const Vec3 back = vertices[line * 2 + 1].position - to;
                CHECK(glm::dot(back, direction) == doctest::Approx(-1.0f));
                CHECK(glm::length(back + direction) == doctest::Approx(0.4f));
                middle += vertices[line * 2 + 1].position * 0.25f;
            }
            // The four are spread evenly around the shaft.
            CHECK_NEAR(middle, to - direction, 1e-5f);
        }
    }

    TEST_CASE("an arrow without length draws nothing") {
        DebugDraw lines;
        lines.arrow(Vec3(1.0f, 2.0f, 3.0f), Vec3(1.0f, 2.0f, 3.0f), kWhite);
        CHECK(lines.isEmpty());
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        lines.arrow(Vec3(0.0f), Vec3(notANumber, 0.0f, 0.0f), kWhite);
        CHECK(lines.isEmpty());
    }

    TEST_CASE("a cone is a circle joined to the tip by four lines") {
        const Vec3 apex(1.0f, 2.0f, 3.0f);
        for (const Vec3 direction : {kWorldUp, -kWorldUp, kWorldForward, glm::normalize(Vec3(1.0f, 1.0f, 1.0f))}) {
            CAPTURE(test::describe(direction));
            DebugDraw lines;
            // Twice as long as its direction: the direction is an axis, not a length.
            lines.cone(apex, direction * 2.0f, 4.0f, 3.0f, kWhite, 16);

            REQUIRE(lines.lineCount() == 20);
            const std::span<const DebugVertex> vertices = lines.vertices();
            const Vec3 center = apex + direction * 4.0f;
            // The rim: around the point four along the axis, three away from it, flat.
            for (size_t line = 0; line < 16; ++line) {
                const Vec3 offset = vertices[line * 2].position - center;
                CHECK(glm::length(offset) == doctest::Approx(3.0f));
                CHECK(glm::dot(offset, direction) == doctest::Approx(0.0f));
            }
            // The sides: from the tip to the rim, five long (3-4-5), a quarter turn apart.
            Vec3 middle(0.0f);
            for (size_t line = 16; line < 20; ++line) {
                CHECK(vertices[line * 2].position == apex);
                CHECK(lineLength(vertices, line) == doctest::Approx(5.0f));
                CHECK(glm::distance(vertices[line * 2 + 1].position, center) == doctest::Approx(3.0f));
                middle += vertices[line * 2 + 1].position * 0.25f;
            }
            CHECK_NEAR(middle, center, 1e-5f);
        }
    }

    TEST_CASE("a cone without a direction or a size still draws without trouble") {
        DebugDraw lines;
        // No direction: a fallback axis.
        lines.cone(Vec3(0.0f), Vec3(0.0f), 1.0f, 1.0f, kWhite, 8);
        CHECK(lines.lineCount() == 12);
        for (const DebugVertex& vertex : lines.vertices()) {
            CHECK(std::isfinite(vertex.position.x + vertex.position.y + vertex.position.z));
        }
        // No size: every line ends at the tip.
        lines.clear();
        lines.cone(Vec3(1.0f), kWorldUp, 0.0f, 0.0f, kWhite, 8);
        for (const DebugVertex& vertex : lines.vertices()) {
            CHECK_NEAR(vertex.position, Vec3(1.0f), 1e-6f);
        }
    }

    TEST_CASE("a frustum outlines the volume a camera sees") {
        const float nearPlane = 1.0f;
        const float farPlane = 10.0f;
        const Mat4 projection = perspectiveReverseZ(toRadians(90.0f), 1.0f, nearPlane, farPlane);

        DebugDraw lines;
        lines.frustum(glm::inverse(projection), kWhite);
        REQUIRE(lines.lineCount() == 12);

        // Every corner lies on the near or the far plane, on the edge of the view.
        int nearCorners = 0;
        int farCorners = 0;
        for (const DebugVertex& vertex : lines.vertices()) {
            const float depth = -vertex.position.z;
            const bool isNear = std::abs(depth - nearPlane) < 1e-3f;
            const bool isFar = std::abs(depth - farPlane) < 1e-2f;
            REQUIRE((isNear || isFar));
            nearCorners += isNear ? 1 : 0;
            farCorners += isFar ? 1 : 0;
            CHECK(std::abs(vertex.position.x) == doctest::Approx(depth).epsilon(1e-3));
            CHECK(std::abs(vertex.position.y) == doctest::Approx(depth).epsilon(1e-3));
        }
        // Each of the eight corners is the end of three edges.
        CHECK(nearCorners == 12);
        CHECK(farCorners == 12);
    }

    TEST_CASE("a grid has lines in both directions with every n-th one emphasized") {
        DebugDraw lines;
        const DebugGridDesc desc{.center = Vec3(0.0f, 1.0f, 0.0f),
                                 .cellSize = 0.5f,
                                 .halfCellCount = 4,
                                 .color = Vec4(0.2f, 0.2f, 0.2f, 1.0f),
                                 .majorLineEvery = 2,
                                 .majorColor = Vec4(0.8f, 0.8f, 0.8f, 1.0f)};
        lines.grid(desc);

        // Nine lines along each axis.
        REQUIRE(lines.lineCount() == 18);
        int majorLines = 0;
        for (size_t line = 0; line < lines.lineCount(); ++line) {
            const DebugVertex& from = lines.vertices()[line * 2];
            const DebugVertex& to = lines.vertices()[line * 2 + 1];
            CHECK(from.position.y == 1.0f);
            CHECK(to.position.y == 1.0f);
            CHECK(lineLength(lines.vertices(), line) == doctest::Approx(4.0f));
            CHECK(std::abs(from.position.x) <= 2.0f);
            CHECK(std::abs(from.position.z) <= 2.0f);
            majorLines += from.color == packRgba8(desc.majorColor) ? 1 : 0;
        }
        // Offsets -4, -2, 0, 2 and 4 in each direction.
        CHECK(majorLines == 10);
    }

    TEST_CASE("a grid without cells or with a non-positive cell size draws nothing") {
        DebugDraw lines;
        lines.grid({.halfCellCount = 0});
        lines.grid({.cellSize = 0.0f});
        lines.grid({.cellSize = -1.0f});
        CHECK(lines.isEmpty());

        lines.grid({.halfCellCount = 1, .majorLineEvery = 0});
        CHECK(lines.lineCount() == 6);
    }
}
