#include "cordite/assets/Primitives.h"

#include "AssetTestSupport.h"

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::checkTangentFrames;
using cordite::test::checkTangentsFollowUvs;

namespace {

struct Primitive {
    std::string name;
    std::function<MeshData()> make;
    /// Half the size of the box the shape must fill exactly.
    Vec3 halfExtents;
};

std::vector<Primitive> allPrimitives() {
    return {
        {"Box", [] { return makeBox(Vec3(2.0f, 4.0f, 6.0f)); }, Vec3(1.0f, 2.0f, 3.0f)},
        {"Plane", [] { return makePlane(Vec2(4.0f, 2.0f), UVec2(4, 2)); }, Vec3(2.0f, 0.0f, 1.0f)},
        {"Sphere", [] { return makeSphere(1.5f, 16, 8); }, Vec3(1.5f)},
        {"Cylinder", [] { return makeCylinder(0.5f, 3.0f, 16); }, Vec3(0.5f, 1.5f, 0.5f)},
        {"Cone", [] { return makeCone(1.0f, 2.0f, 16); }, Vec3(1.0f, 1.0f, 1.0f)},
        {"Capsule", [] { return makeCapsule(0.5f, 3.0f, 16, 4); }, Vec3(0.5f, 1.5f, 0.5f)},
        {"Torus", [] { return makeTorus(1.0f, 0.25f, 16, 8); }, Vec3(1.25f, 0.25f, 1.25f)},
    };
}

} // namespace

TEST_SUITE("Primitives") {
    TEST_CASE("every primitive is a valid mesh with one submesh and its own name") {
        for (const Primitive& primitive : allPrimitives()) {
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();
            CHECK(validateMesh(mesh).has_value());
            CHECK(mesh.name == primitive.name);
            REQUIRE(mesh.submeshes.size() == 1);
            CHECK(mesh.submeshes[0].indexCount == mesh.indices.size());
            CHECK(mesh.submeshes[0].bounds == mesh.bounds);
        }
    }

    TEST_CASE("every primitive is centred and exactly as large as asked") {
        for (const Primitive& primitive : allPrimitives()) {
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();
            CHECK_NEAR(mesh.bounds.max, primitive.halfExtents, 1.0e-5f);
            CHECK_NEAR(mesh.bounds.min, -primitive.halfExtents, 1.0e-5f);
        }
    }

    TEST_CASE("every primitive faces outwards and has tangents that follow its texture coordinates") {
        for (const Primitive& primitive : allPrimitives()) {
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();
            checkTangentFrames(mesh);
            checkTangentsFollowUvs(mesh);
        }
    }

    TEST_CASE("texture coordinates stay inside the unit square, and both sets agree") {
        for (const Primitive& primitive : allPrimitives()) {
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();
            for (const MeshVertex& vertex : mesh.vertices) {
                CHECK(vertex.uv0.x >= -1.0e-6f);
                CHECK(vertex.uv0.x <= 1.0f + 1.0e-6f);
                CHECK(vertex.uv0.y >= -1.0e-6f);
                CHECK(vertex.uv0.y <= 1.0f + 1.0e-6f);
                CHECK(vertex.uv1 == vertex.uv0);
            }
        }
    }

    TEST_CASE("the convex primitives have normals that point away from the centre") {
        for (const Primitive& primitive : allPrimitives()) {
            if (primitive.name == "Torus" || primitive.name == "Plane") {
                continue;
            }
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();
            for (const MeshVertex& vertex : mesh.vertices) {
                CHECK(glm::dot(vertex.normal, vertex.position) > 0.0f);
            }
        }
    }

    TEST_CASE("a box has four vertices per face, with the image upright on its sides") {
        const MeshData box = makeBox();
        CHECK(box.vertices.size() == 24);
        CHECK(box.indices.size() == 36);

        // On the four sides the top of the image (v = 0) is at the top of the box.
        for (const MeshVertex& vertex : box.vertices) {
            if (std::abs(vertex.normal.y) < 0.5f) {
                CHECK(vertex.uv0.y == (vertex.position.y > 0.0f ? 0.0f : 1.0f));
            }
        }
        // Seen from outside, the left edge of the +X face is at +Z.
        size_t topLeftCorners = 0;
        for (const MeshVertex& vertex : box.vertices) {
            if (vertex.normal == Vec3(1.0f, 0.0f, 0.0f) && vertex.uv0 == Vec2(0.0f, 0.0f)) {
                ++topLeftCorners;
                CHECK(vertex.position == Vec3(0.5f, 0.5f, 0.5f));
            }
        }
        CHECK(topLeftCorners == 1);
    }

    TEST_CASE("a plane faces up, with u along +X and v along +Z") {
        const MeshData plane = makePlane(Vec2(2.0f, 4.0f), UVec2(2, 4));
        CHECK(plane.vertices.size() == 3 * 5);
        CHECK(plane.indices.size() == 2 * 4 * 6);
        for (const MeshVertex& vertex : plane.vertices) {
            CHECK(vertex.normal == Vec3(0.0f, 1.0f, 0.0f));
            CHECK(vertex.tangent == Vec4(1.0f, 0.0f, 0.0f, 1.0f));
            CHECK(vertex.position.y == 0.0f);
            CHECK(vertex.uv0.x == doctest::Approx(vertex.position.x / 2.0f + 0.5f));
            CHECK(vertex.uv0.y == doctest::Approx(vertex.position.z / 4.0f + 0.5f));
        }
    }

    TEST_CASE("a sphere has the expected number of vertices and triangles, and every vertex on its surface") {
        const MeshData sphere = makeSphere(2.0f, 8, 4);
        // A grid of 9 x 5, less the one vertex at each pole that no triangle uses.
        CHECK(sphere.vertices.size() == 9 * 5 - 2);
        // Two triangles per cell, less the one that vanishes in each cell at a pole.
        CHECK(sphere.triangleCount() == 8 * 4 * 2 - 2 * 8);
        for (const MeshVertex& vertex : sphere.vertices) {
            CHECK(glm::length(vertex.position) == doctest::Approx(2.0f));
            CHECK_NEAR(vertex.normal, vertex.position / 2.0f, 1.0e-6f);
        }
        // v runs from the top pole to the bottom one, and every vertex of a pole is the
        // same point exactly.
        size_t poleVertices = 0;
        for (const MeshVertex& vertex : sphere.vertices) {
            if (vertex.uv0.y == 0.0f) {
                ++poleVertices;
                CHECK(vertex.position == Vec3(0.0f, 2.0f, 0.0f));
            } else if (vertex.uv0.y == 1.0f) {
                ++poleVertices;
                CHECK(vertex.position == Vec3(0.0f, -2.0f, 0.0f));
            }
        }
        CHECK(poleVertices == 16);
    }

    TEST_CASE("no primitive contains a triangle without area, except a capsule that is a sphere") {
        const auto degenerateTriangles = [](const MeshData& mesh) {
            size_t count = 0;
            for (size_t first = 0; first < mesh.indices.size(); first += 3) {
                const Vec3& a = mesh.vertices[mesh.indices[first]].position;
                const Vec3& b = mesh.vertices[mesh.indices[first + 1]].position;
                const Vec3& c = mesh.vertices[mesh.indices[first + 2]].position;
                count += glm::length(glm::cross(b - a, c - a)) < 1.0e-9f ? 1 : 0;
            }
            return count;
        };
        for (const Primitive& primitive : allPrimitives()) {
            CAPTURE(primitive.name);
            CHECK(degenerateTriangles(primitive.make()) == 0);
        }
        // With no straight part, the band between the two halves has no height.
        CHECK(degenerateTriangles(makeCapsule(0.5f, 1.0f, 8, 2)) == 2 * 8);
    }

    TEST_CASE("the closed primitives are watertight: every edge joins exactly two triangles") {
        // Vertices are told apart by position alone, and compared exactly: a seam whose
        // two sides differ in the last bit is a crack that can show as a missing pixel.
        using Point = std::tuple<float, float, float>;
        using Edge = std::pair<Point, Point>;
        const auto pointOf = [](const Vec3& position) { return Point(position.x, position.y, position.z); };

        for (const Primitive& primitive : allPrimitives()) {
            if (primitive.name == "Plane") {
                continue;
            }
            CAPTURE(primitive.name);
            const MeshData mesh = primitive.make();

            std::map<Edge, int> edges;
            for (size_t first = 0; first < mesh.indices.size(); first += 3) {
                for (size_t corner = 0; corner < 3; ++corner) {
                    Point from = pointOf(mesh.vertices[mesh.indices[first + corner]].position);
                    Point to = pointOf(mesh.vertices[mesh.indices[first + (corner + 1) % 3]].position);
                    if (to < from) {
                        std::swap(from, to);
                    }
                    ++edges[{from, to}];
                }
            }
            for (const auto& [edge, count] : edges) {
                REQUIRE(count == 2);
            }
        }
    }

    TEST_CASE("a capsule shorter than its diameter is a sphere") {
        const MeshData capsule = makeCapsule(1.0f, 0.5f, 12, 3);
        CHECK(validateMesh(capsule).has_value());
        CHECK_NEAR(capsule.bounds.max, Vec3(1.0f), 1.0e-5f);
        for (const MeshVertex& vertex : capsule.vertices) {
            CHECK(glm::length(vertex.position) == doctest::Approx(1.0f));
        }
    }

    TEST_CASE("the texture of a capsule is not stretched over its straight part") {
        const MeshData capsule = makeCapsule(1.0f, 6.0f, 8, 4);
        // Outline: a quarter circle, the straight part, a quarter circle.
        const float outline = kPi + 4.0f;
        float rimTop = -1.0f;
        float rimBottom = -1.0f;
        for (const MeshVertex& vertex : capsule.vertices) {
            if (std::abs(vertex.normal.y) < 1.0e-5f) {
                (vertex.position.y > 0.0f ? rimTop : rimBottom) = vertex.uv0.y;
            }
        }
        CHECK(rimTop == doctest::Approx(kHalfPi / outline));
        CHECK(rimBottom == doctest::Approx((kHalfPi + 4.0f) / outline));
    }

    TEST_CASE("a cone has a tip vertex per segment, so that its side is not shaded like a ball") {
        const MeshData cone = makeCone(1.0f, 2.0f, 6);
        size_t tipVertices = 0;
        for (const MeshVertex& vertex : cone.vertices) {
            if (vertex.position == Vec3(0.0f, 1.0f, 0.0f)) {
                ++tipVertices;
                // The side leans by atan(radius / height).
                CHECK(vertex.normal.y == doctest::Approx(1.0f / std::sqrt(5.0f)));
            }
        }
        CHECK(tipVertices == 6);
        // Side: one triangle per segment. Base: one per segment.
        CHECK(cone.triangleCount() == 12);
    }

    TEST_CASE("a torus whose tube is thicker than its ring does not pass through itself") {
        const MeshData torus = makeTorus(0.1f, 0.5f, 12, 8);
        CHECK(validateMesh(torus).has_value());
        // The centre line was moved out to the tube's radius.
        CHECK_NEAR(torus.bounds.max, Vec3(1.0f, 0.5f, 1.0f), 1.0e-5f);
        // Where a surface passes through itself its triangles face against their normals.
        checkTangentsFollowUvs(torus);
    }

    TEST_CASE("sizes that are not positive and counts out of range still give valid meshes") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<MeshData> meshes = {
            makeBox(Vec3(0.0f, -1.0f, nan)), makePlane(Vec2(-2.0f, 0.0f), UVec2(0, 100000)),
            makeSphere(0.0f, 0, 0),          makeSphere(1.0f, 100000, 100000),
            makeCylinder(-1.0f, nan, 1),     makeCone(0.0f, 0.0f, 2),
            makeCapsule(-1.0f, -1.0f, 0, 0), makeTorus(0.0f, 0.0f, 0, 0),
        };
        for (const MeshData& mesh : meshes) {
            CAPTURE(mesh.name);
            CHECK(validateMesh(mesh).has_value());
            CHECK(mesh.vertices.size() <= (kMaxPrimitiveSegments + 1) * (2 * kMaxPrimitiveSegments + 2));
            CHECK_FALSE(mesh.bounds.isEmpty());
            CHECK(mesh.triangleCount() > 0);
        }

        // The smallest counts each shape can be made of.
        CHECK(makeSphere(1.0f, 0, 0).vertices.size() == 4 * 3 - 2);
        CHECK(makePlane(Vec2(1.0f), UVec2(0)).vertices.size() == 4);
        CHECK(makeSphere(1.0f, 100000, 100000).vertices.size() ==
              (kMaxPrimitiveSegments + 1) * (kMaxPrimitiveSegments + 1) - 2);
    }

    TEST_CASE("the defaults fit a unit cube, or for the capsule a person") {
        CHECK_NEAR(makeBox().bounds.max, Vec3(0.5f), 1.0e-6f);
        CHECK_NEAR(makeSphere().bounds.max, Vec3(0.5f), 1.0e-6f);
        CHECK_NEAR(makePlane().bounds.max, Vec3(0.5f, 0.0f, 0.5f), 1.0e-6f);
        CHECK_NEAR(makeCylinder().bounds.max, Vec3(0.5f), 1.0e-6f);
        CHECK_NEAR(makeCone().bounds.max, Vec3(0.5f), 1.0e-6f);
        CHECK_NEAR(makeCapsule().bounds.max, Vec3(0.5f, 1.0f, 0.5f), 1.0e-6f);
        CHECK_NEAR(makeTorus().bounds.max, Vec3(0.5f, 0.125f, 0.5f), 1.0e-6f);
    }
}
