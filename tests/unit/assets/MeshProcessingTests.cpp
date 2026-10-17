#include "cordite/assets/MeshProcessing.h"

#include "cordite/assets/Primitives.h"

#include "AssetTestSupport.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <span>
#include <vector>

using namespace cordite;
using cordite::test::checkTangentFrames;
using cordite::test::checkTangentsFollowUvs;
using cordite::test::sameSurface;

namespace {

/// A unit square in the XY plane facing +Z, with the image mapped onto it the usual way:
/// `u` to the right, `v` downwards.
MeshData makeQuad() {
    MeshData mesh;
    mesh.name = "Quad";
    const auto corner = [](float x, float y) {
        MeshVertex vertex;
        vertex.position = Vec3(x, y, 0.0f);
        vertex.normal = Vec3(0.0f, 0.0f, 1.0f);
        vertex.tangent = Vec4(0.0f);
        vertex.uv0 = Vec2(x, 1.0f - y);
        return vertex;
    };
    const std::vector<MeshVertex> vertices = {corner(0.0f, 0.0f), corner(1.0f, 0.0f), corner(1.0f, 1.0f),
                                              corner(0.0f, 1.0f)};
    const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
    appendSubmesh(mesh, vertices, indices);
    return mesh;
}

/// A cube of eight shared vertices, as a file without normals would describe it.
MeshData makeSharedVertexCube() {
    MeshData mesh;
    mesh.name = "Shared cube";
    std::vector<MeshVertex> vertices(8);
    for (uint32_t index = 0; index < 8; ++index) {
        vertices[index].position =
            Vec3((index & 1u) != 0 ? 1.0f : -1.0f, (index & 2u) != 0 ? 1.0f : -1.0f, (index & 4u) != 0 ? 1.0f : -1.0f);
        vertices[index].normal = Vec3(0.0f);
        vertices[index].tangent = Vec4(0.0f);
    }
    // Counter-clockwise as seen from outside.
    const std::vector<uint32_t> indices = {
        4, 5, 7, 4, 7, 6, // +Z
        1, 0, 2, 1, 2, 3, // -Z
        5, 1, 3, 5, 3, 7, // +X
        0, 4, 6, 0, 6, 2, // -X
        6, 7, 3, 6, 3, 2, // +Y
        0, 1, 5, 0, 5, 4, // -Y
    };
    appendSubmesh(mesh, vertices, indices);
    return mesh;
}

} // namespace

TEST_SUITE("MeshProcessing") {
    TEST_CASE("perpendicularTo returns a unit vector at a right angle, for every direction") {
        const std::vector<Vec3> normals = {
            {1.0f, 0.0f, 0.0f},
            {-1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, -1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
            {0.0f, 0.0f, -1.0f},
            glm::normalize(Vec3(1.0f, 2.0f, 3.0f)),
            glm::normalize(Vec3(-0.3f, 0.2f, -0.9f)),
            glm::normalize(Vec3(1.0e-4f, 1.0e-4f, -1.0f)),
        };
        for (const Vec3& normal : normals) {
            const Vec3 perpendicular = perpendicularTo(normal);
            CAPTURE(test::describe(normal));
            CHECK(std::abs(glm::length(perpendicular) - 1.0f) < 1.0e-5f);
            CHECK(std::abs(glm::dot(perpendicular, normal)) < 1.0e-5f);
        }
    }

    TEST_CASE("repairTangentFrames fixes what a file can get wrong") {
        MeshData mesh = makeQuad();
        // Too long, not perpendicular, and a handedness that is not a sign.
        mesh.vertices[0].normal = Vec3(0.0f, 0.0f, 5.0f);
        mesh.vertices[0].tangent = Vec4(2.0f, 0.0f, 2.0f, -0.25f);
        // No direction at all.
        mesh.vertices[1].normal = Vec3(0.0f);
        mesh.vertices[1].tangent = Vec4(0.0f);
        // Tangent parallel to the normal.
        mesh.vertices[2].tangent = Vec4(0.0f, 0.0f, 3.0f, 1.0f);
        // Not a number.
        mesh.vertices[3].normal = Vec3(std::numeric_limits<float>::quiet_NaN());
        mesh.vertices[3].tangent = Vec4(std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 0.0f);

        repairTangentFrames(mesh);
        checkTangentFrames(mesh);

        CHECK_NEAR(mesh.vertices[0].normal, Vec3(0.0f, 0.0f, 1.0f), 1.0e-6f);
        CHECK_NEAR(Vec3(mesh.vertices[0].tangent), Vec3(1.0f, 0.0f, 0.0f), 1.0e-6f);
        CHECK(mesh.vertices[0].tangent.w == -1.0f);
        CHECK(mesh.vertices[1].normal == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(mesh.vertices[3].normal == Vec3(0.0f, 1.0f, 0.0f));
        // Zero counts as positive.
        CHECK(mesh.vertices[3].tangent.w == 1.0f);
    }

    TEST_CASE("repairTangentFrames leaves a correct frame untouched") {
        MeshData mesh = makeBox();
        const MeshData before = mesh;
        repairTangentFrames(mesh);
        for (size_t index = 0; index < mesh.vertices.size(); ++index) {
            CHECK_NEAR(mesh.vertices[index].normal, before.vertices[index].normal, 1.0e-6f);
            CHECK_NEAR(mesh.vertices[index].tangent, before.vertices[index].tangent, 1.0e-6f);
        }
    }

    TEST_CASE("flat normals give every triangle its own vertices and the normal of its plane") {
        MeshData mesh = makeSharedVertexCube();
        const MeshData before = mesh;
        generateFlatNormals(mesh);

        CHECK(mesh.vertices.size() == 36);
        CHECK(mesh.indices.size() == 36);
        CHECK(sameSurface(mesh, before));
        for (size_t first = 0; first < mesh.indices.size(); first += 3) {
            const MeshVertex& a = mesh.vertices[mesh.indices[first]];
            const MeshVertex& b = mesh.vertices[mesh.indices[first + 1]];
            const MeshVertex& c = mesh.vertices[mesh.indices[first + 2]];
            const Vec3 expected = glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
            CHECK_NEAR(a.normal, expected, 1.0e-6f);
            CHECK(a.normal == b.normal);
            CHECK(a.normal == c.normal);
            // A cube's faces point along the axes, away from the centre.
            CHECK(glm::dot(a.normal, a.position) > 0.0f);
        }
    }

    TEST_CASE("flat normals of a triangle without area fall back to +Y") {
        MeshData mesh;
        std::vector<MeshVertex> vertices(3);
        vertices[1].position = Vec3(1.0f, 0.0f, 0.0f);
        vertices[2].position = Vec3(2.0f, 0.0f, 0.0f);
        const std::vector<uint32_t> indices = {0, 1, 2};
        appendSubmesh(mesh, vertices, indices);

        generateFlatNormals(mesh);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK(vertex.normal == Vec3(0.0f, 1.0f, 0.0f));
        }
    }

    TEST_CASE("tangents of the usual mapping point along u, with positive handedness") {
        MeshData mesh = makeQuad();
        generateTangents(mesh);

        checkTangentFrames(mesh);
        checkTangentsFollowUvs(mesh);
        CHECK(mesh.vertices.size() == 4);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.tangent, Vec4(1.0f, 0.0f, 0.0f, 1.0f), 1.0e-5f);
        }
    }

    TEST_CASE("a mirrored mapping gives negative handedness") {
        MeshData mesh = makeQuad();
        for (MeshVertex& vertex : mesh.vertices) {
            vertex.uv0.x = 1.0f - vertex.uv0.x;
        }
        generateTangents(mesh);

        checkTangentFrames(mesh);
        checkTangentsFollowUvs(mesh);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.tangent, Vec4(-1.0f, 0.0f, 0.0f, -1.0f), 1.0e-5f);
        }
    }

    TEST_CASE("tangents follow a rotated mapping") {
        MeshData mesh = makeQuad();
        // u now runs up the quad and v to the right.
        for (MeshVertex& vertex : mesh.vertices) {
            vertex.uv0 = Vec2(vertex.position.y, vertex.position.x);
        }
        generateTangents(mesh);

        checkTangentFrames(mesh);
        checkTangentsFollowUvs(mesh);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(Vec3(vertex.tangent), Vec3(0.0f, 1.0f, 0.0f), 1.0e-5f);
        }
    }

    TEST_CASE("generated tangents agree with the analytic ones of the primitives") {
        for (MeshData mesh : {makeBox(), makePlane(Vec2(2.0f), UVec2(3)), makeCylinder(), makeTorus()}) {
            CAPTURE(mesh.name);
            const MeshData analytic = mesh;
            for (MeshVertex& vertex : mesh.vertices) {
                vertex.tangent = Vec4(0.0f);
            }
            generateTangents(mesh);

            checkTangentFrames(mesh);
            checkTangentsFollowUvs(mesh);
            CHECK(sameSurface(mesh, analytic));
        }
    }

    TEST_CASE("a mesh without texture coordinates still gets valid tangents") {
        MeshData mesh = makeSphere(1.0f, 12, 6);
        for (MeshVertex& vertex : mesh.vertices) {
            vertex.uv0 = Vec2(0.0f);
            vertex.tangent = Vec4(0.0f);
        }
        generateTangents(mesh);

        checkTangentFrames(mesh);
        CHECK(validateMesh(mesh).has_value());
    }

    TEST_CASE("a vertex is split where its triangles need different tangents") {
        // The two triangles of the quad share an edge and both of its vertices. Moving
        // the texture coordinate of the far corner of the second triangle turns its
        // mapping inside out: it now has the opposite handedness of the first.
        MeshData mesh = makeQuad();
        REQUIRE(mesh.vertices.size() == 4);
        mesh.vertices[3].uv0 = Vec2(2.0f, 1.0f);

        generateTangents(mesh);

        checkTangentFrames(mesh);
        checkTangentsFollowUvs(mesh);
        // The two shared vertices exist once per handedness now.
        CHECK(mesh.vertices.size() == 6);
        CHECK(validateMesh(mesh).has_value());

        int positive = 0;
        int negative = 0;
        for (const MeshVertex& vertex : mesh.vertices) {
            (vertex.tangent.w > 0.0f ? positive : negative) += 1;
        }
        CHECK(positive == 3);
        CHECK(negative == 3);
    }

    TEST_CASE("generating tangents for an empty mesh does nothing") {
        MeshData mesh;
        generateTangents(mesh);
        generateFlatNormals(mesh);
        optimizeMesh(mesh);
        CHECK(mesh.vertices.empty());
        CHECK(mesh.indices.empty());
    }

    TEST_CASE("optimizing merges equal vertices and drops unused ones") {
        MeshData mesh = makeSharedVertexCube();
        generateFlatNormals(mesh);
        REQUIRE(mesh.vertices.size() == 36);
        // Not referred to by any triangle.
        mesh.vertices.push_back({.position = Vec3(9.0f)});
        const MeshData before = mesh;

        optimizeMesh(mesh);

        // Four distinct corners per face.
        CHECK(mesh.vertices.size() == 24);
        CHECK(mesh.indices.size() == 36);
        CHECK(sameSurface(mesh, before));
        CHECK(validateMesh(mesh).has_value());
        CHECK(mesh.bounds.min == Vec3(-1.0f));
        CHECK(mesh.bounds.max == Vec3(1.0f));
    }

    TEST_CASE("optimizing treats positive and negative zero as the same number") {
        MeshData mesh = makeQuad();
        mesh.vertices.push_back(mesh.vertices[0]);
        mesh.vertices.back().position.z = -0.0f;
        mesh.indices = {4, 1, 2, 0, 2, 3};

        optimizeMesh(mesh);
        CHECK(mesh.vertices.size() == 4);
    }

    TEST_CASE("optimizing keeps every triangle in its own submesh") {
        MeshData mesh;
        mesh.name = "Two parts";
        const MeshData box = makeBox();
        const MeshData sphere = makeSphere(0.5f, 8, 4);
        appendSubmesh(mesh, box.vertices, box.indices);
        appendSubmesh(mesh, sphere.vertices, sphere.indices);

        optimizeMesh(mesh);

        REQUIRE(mesh.submeshes.size() == 2);
        CHECK(mesh.submeshes[0].firstIndex == 0);
        CHECK(mesh.submeshes[0].indexCount == box.indices.size());
        CHECK(mesh.submeshes[1].firstIndex == box.indices.size());
        CHECK(mesh.submeshes[1].indexCount == sphere.indices.size());
        CHECK(validateMesh(mesh).has_value());

        // Each range still draws the part it drew before.
        MeshData first = mesh;
        first.indices.resize(box.indices.size());
        MeshData second = mesh;
        second.indices.erase(second.indices.begin(),
                             second.indices.begin() + static_cast<ptrdiff_t>(box.indices.size()));
        CHECK(sameSurface(first, box));
        CHECK(sameSurface(second, sphere));
    }

    TEST_CASE("optimizing is safe for a mesh with an empty submesh") {
        MeshData mesh = makeBox();
        appendSubmesh(mesh, {}, {});
        optimizeMesh(mesh);
        CHECK(mesh.submeshes.size() == 2);
        CHECK(mesh.submeshes[1].bounds.isEmpty());
        CHECK(validateMesh(mesh).has_value());
    }

    TEST_CASE("a fine mesh gets levels of detail, each with fewer triangles and an error no smaller") {
        MeshData sphere = makeSphere(0.5f, 64, 32);
        generateLods(sphere);
        REQUIRE(validateMesh(sphere).has_value());
        REQUIRE(sphere.lods.size() >= 3);
        size_t previousTriangles = sphere.triangleCount();
        float previousError = 0.0f;
        std::vector<size_t> triangles;
        for (const MeshLod& lod : sphere.lods) {
            const size_t count = lod.indices.size() / 3;
            CHECK(static_cast<float>(count) <= 0.85f * static_cast<float>(previousTriangles));
            CHECK(count >= 64);
            CHECK(lod.error >= previousError);
            // A sphere half a metre across that loses half its triangles is not a millimetre off;
            // one of a few dozen is off by centimetres, but never by its radius.
            CHECK(lod.error > 0.0f);
            CHECK(lod.error < 0.5f);
            triangles.push_back(count);
            previousTriangles = count;
            previousError = lod.error;
        }
        CHECK(sphere.lods.front().error < 0.005f);
        // The levels of a mesh are part of what it is: pinned, so that a change of the
        // simplifier, or of how it is built, is noticed.
        CHECK(triangles == std::vector<size_t>{1984, 992, 496, 248});

        // The same on every run.
        MeshData again = makeSphere(0.5f, 64, 32);
        generateLods(again);
        CHECK(again.lods == sphere.lods);
        // And generating them again replaces them.
        generateLods(again);
        CHECK(again.lods == sphere.lods);
    }

    TEST_CASE("a mesh the simplifier cannot reduce gets no levels of detail") {
        // Two hundred squares side by side, none touching another: every edge is open, and
        // stays where it is, so not a triangle can go. A level would keep all of them.
        MeshData squares;
        squares.name = "Squares";
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        for (uint32_t square = 0; square < 200; ++square) {
            const auto first = static_cast<uint32_t>(vertices.size());
            const float left = static_cast<float>(square) * 2.0f;
            for (const Vec2 corner : {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)}) {
                MeshVertex vertex;
                vertex.position = Vec3(left + corner.x, 0.0f, -corner.y);
                vertex.uv0 = corner;
                vertices.push_back(vertex);
            }
            indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
        }
        appendSubmesh(squares, vertices, indices);
        REQUIRE(squares.triangleCount() >= kMinLodTriangles);
        REQUIRE(validateMesh(squares).has_value());
        generateLods(squares);
        CHECK(squares.lods.empty());
    }

    TEST_CASE("a mesh of few triangles gets no levels of detail") {
        MeshData box = makeBox();
        generateLods(box);
        CHECK(box.lods.empty());
        MeshData sphere = makeSphere(0.5f, 16, 8);
        REQUIRE(sphere.triangleCount() < kMinLodTriangles);
        generateLods(sphere);
        CHECK(sphere.lods.empty());
    }

    TEST_CASE("the edges between submeshes and around open surfaces stay where they are at every level") {
        // A flat grid in two submeshes, its left half and its right half, which share the
        // vertices of the line between them. Inside each half, the simplifier may take away
        // every vertex at no cost; at the line and at the rim it may take away none.
        const MeshData plane = makePlane(Vec2(1.0f), UVec2(32, 32));
        MeshData grid;
        grid.name = "Grid";
        grid.vertices = plane.vertices;
        std::array<std::vector<uint32_t>, 2> halves;
        for (size_t first = 0; first + 2 < plane.indices.size(); first += 3) {
            const float centerX =
                (plane.vertices[plane.indices[first]].position.x + plane.vertices[plane.indices[first + 1]].position.x +
                 plane.vertices[plane.indices[first + 2]].position.x) /
                3.0f;
            std::vector<uint32_t>& half = halves[centerX < 0.0f ? 0 : 1];
            half.insert(half.end(), plane.indices.begin() + static_cast<std::ptrdiff_t>(first),
                        plane.indices.begin() + static_cast<std::ptrdiff_t>(first + 3));
        }
        for (const std::vector<uint32_t>& half : halves) {
            grid.submeshes.push_back({.firstIndex = static_cast<uint32_t>(grid.indices.size()),
                                      .indexCount = static_cast<uint32_t>(half.size())});
            grid.indices.insert(grid.indices.end(), half.begin(), half.end());
        }
        updateBounds(grid);
        REQUIRE(validateMesh(grid).has_value());

        generateLods(grid);
        REQUIRE(validateMesh(grid).has_value());
        REQUIRE_FALSE(grid.lods.empty());
        const auto isOnLine = [&](uint32_t vertex) { return std::abs(grid.vertices[vertex].position.x) < 1.0e-5f; };
        const auto isOnRim = [&](uint32_t vertex) {
            const Vec3& position = grid.vertices[vertex].position;
            return std::abs(std::abs(position.x) - 0.5f) < 1.0e-5f || std::abs(std::abs(position.z) - 0.5f) < 1.0e-5f;
        };
        const auto used = [&](std::span<const uint32_t> indices, const auto& keep) {
            std::set<uint32_t> found;
            for (const uint32_t index : indices) {
                if (keep(index)) {
                    found.insert(index);
                }
            }
            return found;
        };
        const std::set<uint32_t> line = used(grid.indices, isOnLine);
        const std::set<uint32_t> rim = used(grid.indices, isOnRim);
        REQUIRE(line.size() == 33);
        for (size_t level = 0; level < grid.lods.size(); ++level) {
            CAPTURE(level);
            const MeshLod& lod = grid.lods[level];
            // Fewer triangles than the level before.
            CHECK(lod.indices.size() < (level == 0 ? grid.indices.size() : grid.lods[level - 1].indices.size()));
            // Flat stays flat, within a fraction of a millimetre: the simplifier counts a
            // little for the edges it keeps where they are.
            CHECK(lod.error < 1.0e-3f);
            for (size_t half = 0; half < 2; ++half) {
                const IndexRange& range = lod.submeshes[half];
                const std::span<const uint32_t> indices(lod.indices.data() + range.firstIndex, range.indexCount);
                CHECK(used(indices, isOnLine) == line);
            }
            CHECK(used(lod.indices, isOnRim) == rim);
        }
    }

    TEST_CASE("a skinned mesh keeps how its vertices follow their joints at every level") {
        MeshData sphere = makeSphere(0.5f, 32, 16);
        sphere.skin.assign(sphere.vertices.size(), MeshSkinVertex{.joints = {1, 0, 0, 0}});
        REQUIRE(validateMesh(sphere).has_value());
        generateLods(sphere);
        CHECK_FALSE(sphere.lods.empty());
        CHECK(sphere.skin.size() == sphere.vertices.size());
        CHECK(validateMesh(sphere).has_value());
    }

    TEST_CASE("whatever changes the vertices or the submeshes drops the levels of detail") {
        const auto withLevels = [] {
            MeshData sphere = makeSphere(0.5f, 32, 16);
            generateLods(sphere);
            REQUIRE_FALSE(sphere.lods.empty());
            return sphere;
        };
        MeshData optimized = withLevels();
        optimizeMesh(optimized);
        CHECK(optimized.lods.empty());
        MeshData tangents = withLevels();
        generateTangents(tangents);
        CHECK(tangents.lods.empty());
        MeshData flat = withLevels();
        generateFlatNormals(flat);
        CHECK(flat.lods.empty());
        MeshData appended = withLevels();
        const std::vector<MeshVertex> three(appended.vertices.begin(), appended.vertices.begin() + 3);
        appendSubmesh(appended, three, std::vector<uint32_t>{0, 1, 2});
        CHECK(appended.lods.empty());
        for (const MeshData* mesh : {&optimized, &tangents, &flat, &appended}) {
            CHECK(validateMesh(*mesh).has_value());
        }
    }
}
