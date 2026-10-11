#include "cordite/math/Geometry.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <vector>

using namespace cordite;

TEST_SUITE("Aabb") {
    TEST_CASE("a default box is empty and grows from the first point") {
        Aabb box;
        CHECK(box.isEmpty());
        CHECK_FALSE(box.contains(Vec3(0.0f)));

        box.expand(Vec3(1.0f, 2.0f, 3.0f));
        CHECK_FALSE(box.isEmpty());
        CHECK(box.min == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(box.max == Vec3(1.0f, 2.0f, 3.0f));

        box.expand(Vec3(-1.0f, 5.0f, 0.0f));
        CHECK(box.min == Vec3(-1.0f, 2.0f, 0.0f));
        CHECK(box.max == Vec3(1.0f, 5.0f, 3.0f));
    }

    TEST_CASE("center, extents and size") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(1.0f, 2.0f, 3.0f), Vec3(0.5f, 1.0f, 2.0f));
        CHECK_NEAR(box.center(), Vec3(1.0f, 2.0f, 3.0f), 1e-6f);
        CHECK_NEAR(box.extents(), Vec3(0.5f, 1.0f, 2.0f), 1e-6f);
        CHECK_NEAR(box.size(), Vec3(1.0f, 2.0f, 4.0f), 1e-6f);
    }

    TEST_CASE("fromPoints bounds every point; no points gives an empty box") {
        const std::vector<Vec3> points = {{0.0f, 0.0f, 0.0f}, {2.0f, -1.0f, 4.0f}, {-3.0f, 6.0f, 1.0f}};
        const Aabb box = Aabb::fromPoints(points);
        CHECK(box.min == Vec3(-3.0f, -1.0f, 0.0f));
        CHECK(box.max == Vec3(2.0f, 6.0f, 4.0f));
        for (const Vec3& point : points) {
            CHECK(box.contains(point));
        }

        CHECK(Aabb::fromPoints({}).isEmpty());
    }

    TEST_CASE("expanding by another box ignores empty boxes") {
        Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f));
        box.expand(Aabb());
        CHECK(box == Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f)));

        box.expand(Aabb::fromCenterExtents(Vec3(5.0f, 0.0f, 0.0f), Vec3(1.0f)));
        CHECK(box.max.x == 6.0f);
        CHECK(box.min.x == -1.0f);
    }

    TEST_CASE("contains and intersects treat the boundary as inside") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f));
        CHECK(box.contains(Vec3(1.0f, 1.0f, 1.0f)));
        CHECK_FALSE(box.contains(Vec3(1.001f, 0.0f, 0.0f)));

        CHECK(box.contains(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f))));
        CHECK_FALSE(box.contains(Aabb::fromCenterExtents(Vec3(0.8f, 0.0f, 0.0f), Vec3(0.5f))));
        CHECK_FALSE(box.contains(Aabb()));

        CHECK(box.intersects(Aabb::fromCenterExtents(Vec3(2.0f, 0.0f, 0.0f), Vec3(1.0f))));
        CHECK_FALSE(box.intersects(Aabb::fromCenterExtents(Vec3(2.01f, 0.0f, 0.0f), Vec3(1.0f))));
        CHECK_FALSE(box.intersects(Aabb()));
        CHECK_FALSE(Aabb().intersects(box));
    }

    TEST_CASE("transformed encloses every transformed corner") {
        Random random(7);
        for (int iteration = 0; iteration < 200; ++iteration) {
            const Aabb box = Aabb::fromCenterExtents(
                random.insideUnitSphere() * 10.0f,
                Vec3(random.range(0.1f, 5.0f), random.range(0.1f, 5.0f), random.range(0.1f, 5.0f)));

            Mat4 matrix = glm::translate(Mat4(1.0f), random.insideUnitSphere() * 20.0f);
            matrix = glm::rotate(matrix, random.range(0.0f, kTwoPi), random.onUnitSphere());
            matrix =
                glm::scale(matrix, Vec3(random.range(0.2f, 3.0f), random.range(0.2f, 3.0f), random.range(0.2f, 3.0f)));

            const Aabb transformed = box.transformed(matrix);
            const float slack = 1e-3f;
            const Aabb padded = Aabb::fromCenterExtents(transformed.center(), transformed.extents() + Vec3(slack));

            Aabb exact;
            for (const Vec3& corner : box.corners()) {
                const Vec3 moved = Vec3(matrix * Vec4(corner, 1.0f));
                CHECK(padded.contains(moved));
                exact.expand(moved);
            }
            // Arvo's method is exact for boxes: it must match the brute-force bound.
            CHECK_NEAR(transformed.min, exact.min, 1e-2f);
            CHECK_NEAR(transformed.max, exact.max, 1e-2f);
        }
    }

    TEST_CASE("transforming an empty box keeps it empty") {
        CHECK(Aabb().transformed(glm::translate(Mat4(1.0f), Vec3(5.0f))).isEmpty());
    }

    TEST_CASE("corners enumerates all eight distinct corners") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f, 2.0f, 3.0f));
        const std::array<Vec3, 8> corners = box.corners();
        for (size_t a = 0; a < corners.size(); ++a) {
            CHECK(std::abs(corners[a].x) == 1.0f);
            CHECK(std::abs(corners[a].y) == 2.0f);
            CHECK(std::abs(corners[a].z) == 3.0f);
            for (size_t b = a + 1; b < corners.size(); ++b) {
                CHECK(corners[a] != corners[b]);
            }
        }
    }
}

TEST_SUITE("Sphere") {
    TEST_CASE("fromAabb contains every corner of the box") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(1.0f, 2.0f, 3.0f), Vec3(2.0f, 1.0f, 0.5f));
        const Sphere sphere = Sphere::fromAabb(box);
        CHECK_NEAR(sphere.center, box.center(), 1e-6f);
        for (const Vec3& corner : box.corners()) {
            CHECK(glm::distance(corner, sphere.center) <= sphere.radius + 1e-5f);
        }
        CHECK(Sphere::fromAabb(Aabb()).radius == 0.0f);
    }

    TEST_CASE("contains and intersects") {
        const Sphere sphere{Vec3(0.0f), 2.0f};
        CHECK(sphere.contains(Vec3(2.0f, 0.0f, 0.0f)));
        CHECK_FALSE(sphere.contains(Vec3(2.01f, 0.0f, 0.0f)));
        CHECK(sphere.intersects(Sphere{Vec3(3.0f, 0.0f, 0.0f), 1.0f}));
        CHECK_FALSE(sphere.intersects(Sphere{Vec3(3.1f, 0.0f, 0.0f), 1.0f}));
    }
}

TEST_SUITE("Plane") {
    TEST_CASE("fromPointNormal normalizes and measures signed distance") {
        const Plane plane = Plane::fromPointNormal(Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 10.0f, 0.0f));
        CHECK_NEAR(plane.normal, kWorldUp, 1e-6f);
        CHECK(plane.signedDistance(Vec3(5.0f, 2.0f, -3.0f)) == doctest::Approx(0.0f));
        CHECK(plane.signedDistance(Vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(3.0f));
        CHECK(plane.signedDistance(Vec3(0.0f, -1.0f, 0.0f)) == doctest::Approx(-3.0f));
    }

    TEST_CASE("fromPoints follows counter-clockwise winding") {
        const Plane plane = Plane::fromPoints(Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));
        CHECK_NEAR(plane.normal, kWorldUp, 1e-6f);
    }
}

TEST_SUITE("Ray") {
    TEST_CASE("ray against a box") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -10.0f), Vec3(1.0f));

        const std::optional<float> hit = intersect(Ray{Vec3(0.0f), kWorldForward}, box);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(9.0f));

        CHECK_FALSE(intersect(Ray{Vec3(5.0f, 0.0f, 0.0f), kWorldForward}, box).has_value());
        CHECK_FALSE(intersect(Ray{Vec3(0.0f), -kWorldForward}, box).has_value());

        const std::optional<float> inside = intersect(Ray{Vec3(0.0f, 0.0f, -10.0f), kWorldRight}, box);
        REQUIRE(inside.has_value());
        CHECK(*inside == 0.0f);

        const Ray diagonal{Vec3(-5.0f, 0.0f, -5.0f), glm::normalize(Vec3(1.0f, 0.0f, -1.0f))};
        const std::optional<float> diagonalHit = intersect(diagonal, box);
        REQUIRE(diagonalHit.has_value());
        CHECK(box.contains(diagonal.at(*diagonalHit + 1e-3f)));

        CHECK_FALSE(intersect(Ray{Vec3(0.0f), kWorldForward}, Aabb()).has_value());
    }

    TEST_CASE("an axis-parallel ray uses the slab it travels in") {
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f));
        CHECK(intersect(Ray{Vec3(0.5f, 0.5f, 10.0f), kWorldForward}, box).has_value());
        CHECK_FALSE(intersect(Ray{Vec3(1.5f, 0.5f, 10.0f), kWorldForward}, box).has_value());
    }

    TEST_CASE("ray against a sphere") {
        const Sphere sphere{Vec3(0.0f, 0.0f, -10.0f), 2.0f};

        const std::optional<float> hit = intersect(Ray{Vec3(0.0f), kWorldForward}, sphere);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(8.0f));

        CHECK_FALSE(intersect(Ray{Vec3(3.0f, 0.0f, 0.0f), kWorldForward}, sphere).has_value());
        CHECK_FALSE(intersect(Ray{Vec3(0.0f), -kWorldForward}, sphere).has_value());

        const std::optional<float> inside = intersect(Ray{Vec3(0.0f, 0.0f, -9.0f), kWorldUp}, sphere);
        REQUIRE(inside.has_value());
        CHECK(*inside == 0.0f);

        const std::optional<float> grazing = intersect(Ray{Vec3(2.0f, 0.0f, 0.0f), kWorldForward}, sphere);
        REQUIRE(grazing.has_value());
        CHECK(*grazing == doctest::Approx(10.0f).epsilon(1e-3));
    }

    TEST_CASE("ray against a plane") {
        const Plane ground = Plane::fromPointNormal(Vec3(0.0f), kWorldUp);

        const Ray down{Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f)};
        const std::optional<float> hit = intersect(down, ground);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(5.0f));

        CHECK_FALSE(intersect(Ray{Vec3(0.0f, 5.0f, 0.0f), kWorldUp}, ground).has_value());
        CHECK_FALSE(intersect(Ray{Vec3(0.0f, 5.0f, 0.0f), kWorldRight}, ground).has_value());

        const std::optional<float> fromBelow = intersect(Ray{Vec3(0.0f, -2.0f, 0.0f), kWorldUp}, ground);
        REQUIRE(fromBelow.has_value());
        CHECK(*fromBelow == doctest::Approx(2.0f));
    }

    TEST_CASE("ray against a triangle") {
        // Counter-clockwise when seen from +Z.
        const Vec3 a(-1.0f, -1.0f, 0.0f);
        const Vec3 b(1.0f, -1.0f, 0.0f);
        const Vec3 c(0.0f, 1.0f, 0.0f);

        const Ray front{Vec3(0.0f, 0.0f, 5.0f), kWorldForward};
        const std::optional<float> hit = intersectTriangle(front, a, b, c);
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(5.0f));
        CHECK(intersectTriangle(front, a, b, c, true).has_value());

        const Ray back{Vec3(0.0f, 0.0f, -5.0f), -kWorldForward};
        CHECK(intersectTriangle(back, a, b, c).has_value());
        CHECK_FALSE(intersectTriangle(back, a, b, c, true).has_value());

        CHECK_FALSE(intersectTriangle(Ray{Vec3(2.0f, 0.0f, 5.0f), kWorldForward}, a, b, c).has_value());
        CHECK_FALSE(intersectTriangle(Ray{Vec3(0.0f, 0.0f, 5.0f), -kWorldForward}, a, b, c).has_value());
        CHECK_FALSE(intersectTriangle(Ray{Vec3(0.0f, 0.0f, 5.0f), kWorldRight}, a, b, c).has_value());
    }

    TEST_CASE("a ray through an edge or a corner that triangles share hits one of them") {
        // A square of two triangles, and a fan of four around its centre.
        const Vec3 a(-1.0f, -1.0f, 0.0f);
        const Vec3 b(1.0f, -1.0f, 0.0f);
        const Vec3 c(1.0f, 1.0f, 0.0f);
        const Vec3 d(-1.0f, 1.0f, 0.0f);
        const Vec3 centre(0.0f);

        // Along the diagonal the two triangles share, from several directions.
        for (const float along : {-0.9f, -0.3f, 0.0f, 0.123456f, 0.7f}) {
            for (const Vec3& from : {Vec3(0.0f, 0.0f, 5.0f), Vec3(3.0f, -2.0f, 4.0f), Vec3(-0.3f, 7.0f, 1.5f)}) {
                const Vec3 target(along, along, 0.0f);
                const Ray ray{from, glm::normalize(target - from)};
                CAPTURE(along);
                CHECK((intersectTriangle(ray, a, b, c).has_value() || intersectTriangle(ray, a, c, d).has_value()));
            }
        }

        // Straight at the corner four triangles meet in.
        for (const Vec3& from : {Vec3(0.0f, 0.0f, 5.0f), Vec3(2.0f, 1.0f, 3.0f), Vec3(-4.0f, 0.5f, 0.25f)}) {
            const Ray ray{from, glm::normalize(centre - from)};
            const bool hit =
                intersectTriangle(ray, centre, a, b).has_value() || intersectTriangle(ray, centre, b, c).has_value() ||
                intersectTriangle(ray, centre, c, d).has_value() || intersectTriangle(ray, centre, d, a).has_value();
            CHECK(hit);
        }

        // Clearly beside the square is still a miss.
        CHECK_FALSE(intersectTriangle(Ray{Vec3(1.01f, 0.0f, 5.0f), kWorldForward}, a, b, c).has_value());
    }

    TEST_CASE("small triangles are hit, large ones seen edge-on are not, and ones without area never") {
        // A triangle a millimetre across.
        const float tiny = 0.001f;
        const Ray down{Vec3(0.0002f, 0.0002f, 1.0f), kWorldForward};
        const std::optional<float> hit =
            intersectTriangle(down, Vec3(0.0f), Vec3(tiny, 0.0f, 0.0f), Vec3(0.0f, tiny, 0.0f));
        REQUIRE(hit.has_value());
        CHECK(*hit == doctest::Approx(1.0f));

        // A kilometre across, and the ray runs within its plane.
        const float huge = 1000.0f;
        const Ray along{Vec3(-5.0f, 1.0f, 0.0f), kWorldRight};
        CHECK_FALSE(intersectTriangle(along, Vec3(0.0f), Vec3(huge, 0.0f, 0.0f), Vec3(0.0f, huge, 0.0f)).has_value());

        // Three points on a line, and three points in one place.
        const Ray front{Vec3(0.5f, 0.0f, 5.0f), kWorldForward};
        CHECK_FALSE(intersectTriangle(front, Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f, 0.0f, 0.0f)).has_value());
        CHECK_FALSE(intersectTriangle(front, Vec3(0.5f, 0.0f, 0.0f), Vec3(0.5f, 0.0f, 0.0f), Vec3(0.5f, 0.0f, 0.0f))
                        .has_value());
    }

    TEST_CASE("Ray::at walks along the direction") {
        const Ray ray{Vec3(1.0f, 2.0f, 3.0f), kWorldRight};
        CHECK_NEAR(ray.at(4.0f), Vec3(5.0f, 2.0f, 3.0f), 1e-6f);
    }
}
