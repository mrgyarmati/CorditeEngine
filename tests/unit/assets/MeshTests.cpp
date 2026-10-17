#include "cordite/assets/Mesh.h"

#include "cordite/assets/Primitives.h"

#include "AssetTestSupport.h"

#include <limits>
#include <string>

using namespace cordite;
using cordite::test::AssertionFailure;

namespace {

/// Two triangles forming the unit square in the XY plane, as two submeshes.
MeshData makeTwoTriangles() {
    MeshData mesh;
    mesh.name = "Square";
    mesh.vertices.resize(4);
    mesh.vertices[0].position = Vec3(0.0f, 0.0f, 0.0f);
    mesh.vertices[1].position = Vec3(1.0f, 0.0f, 0.0f);
    mesh.vertices[2].position = Vec3(1.0f, 1.0f, 0.0f);
    mesh.vertices[3].position = Vec3(0.0f, 1.0f, 0.0f);
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.submeshes = {{.firstIndex = 0, .indexCount = 3}, {.firstIndex = 3, .indexCount = 3}};
    updateBounds(mesh);
    return mesh;
}

/// Runs validateMesh and returns the message of the error it must produce.
std::string validationError(const MeshData& mesh) {
    const Status status = validateMesh(mesh);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == ErrorCode::InvalidArgument);
    return status.error().message;
}

} // namespace

TEST_SUITE("Mesh") {
    TEST_CASE("a well-formed mesh is valid") {
        CHECK(validateMesh(makeTwoTriangles()).has_value());
        CHECK(makeTwoTriangles().triangleCount() == 2);
    }

    TEST_CASE("the vertex layout is the one the GPU and files are given") {
        CHECK(sizeof(MeshVertex) == 56);
        CHECK(offsetof(MeshVertex, position) == 0);
        CHECK(offsetof(MeshVertex, normal) == 12);
        CHECK(offsetof(MeshVertex, tangent) == 24);
        CHECK(offsetof(MeshVertex, uv0) == 40);
        CHECK(offsetof(MeshVertex, uv1) == 48);
    }

    TEST_CASE("a default vertex has a usable tangent frame") {
        const MeshVertex vertex;
        CHECK(vertex.normal == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(vertex.tangent == Vec4(1.0f, 0.0f, 0.0f, 1.0f));
    }

    TEST_CASE("validation names the mesh and what is wrong with it") {
        SUBCASE("no vertices") {
            MeshData mesh = makeTwoTriangles();
            mesh.vertices.clear();
            CHECK(validationError(mesh) == "mesh 'Square': it has no vertices");
        }
        SUBCASE("indices that do not make whole triangles") {
            MeshData mesh = makeTwoTriangles();
            mesh.indices.pop_back();
            CHECK(validationError(mesh).find("5 indices do not make whole triangles") != std::string::npos);
        }
        SUBCASE("no submeshes") {
            MeshData mesh = makeTwoTriangles();
            mesh.submeshes.clear();
            CHECK(validationError(mesh).find("it has no submeshes") != std::string::npos);
        }
        SUBCASE("a gap between submeshes") {
            MeshData mesh = makeTwoTriangles();
            mesh.indices.insert(mesh.indices.end(), {0, 1, 2});
            mesh.submeshes[1].firstIndex = 6;
            CHECK(validationError(mesh).find("submesh 1 starts at index 6, expected 3") != std::string::npos);
        }
        SUBCASE("submeshes that overlap") {
            MeshData mesh = makeTwoTriangles();
            mesh.submeshes[1].firstIndex = 0;
            CHECK(validationError(mesh).find("submesh 1 starts at index 0, expected 3") != std::string::npos);
        }
        SUBCASE("a submesh that is not made of whole triangles") {
            MeshData mesh = makeTwoTriangles();
            mesh.submeshes = {{.firstIndex = 0, .indexCount = 4}, {.firstIndex = 4, .indexCount = 2}};
            CHECK(validationError(mesh).find("submesh 0 has 4 indices") != std::string::npos);
        }
        SUBCASE("submeshes that do not cover every index") {
            MeshData mesh = makeTwoTriangles();
            mesh.submeshes.pop_back();
            CHECK(validationError(mesh).find("its submeshes cover 3 indices, but it has 6") != std::string::npos);
        }
        SUBCASE("submeshes that reach beyond the indices") {
            MeshData mesh = makeTwoTriangles();
            mesh.submeshes[1].indexCount = 6;
            CHECK(validationError(mesh).find("its submeshes cover 9 indices, but it has 6") != std::string::npos);
        }
        SUBCASE("an index without a vertex") {
            MeshData mesh = makeTwoTriangles();
            mesh.indices[4] = 4;
            CHECK(validationError(mesh).find("index 4 refers to vertex 4, but there are only 4") != std::string::npos);
        }
        SUBCASE("a number that is not finite") {
            for (const int member : {0, 1, 2, 3, 4}) {
                MeshData mesh = makeTwoTriangles();
                const float bad =
                    member % 2 == 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
                MeshVertex& vertex = mesh.vertices[2];
                switch (member) {
                    case 0:
                        vertex.position.y = bad;
                        break;
                    case 1:
                        vertex.normal.z = bad;
                        break;
                    case 2:
                        vertex.tangent.w = bad;
                        break;
                    case 3:
                        vertex.uv0.x = bad;
                        break;
                    default:
                        vertex.uv1.y = bad;
                        break;
                }
                CAPTURE(member);
                CHECK(validationError(mesh).find("vertex 2 contains a number that is not finite") != std::string::npos);
            }
        }
    }

    TEST_CASE("a submesh may be empty, as long as the ranges still follow each other") {
        MeshData mesh = makeTwoTriangles();
        mesh.submeshes = {{.firstIndex = 0, .indexCount = 0}, {.firstIndex = 0, .indexCount = 6}};
        CHECK(validateMesh(mesh).has_value());
    }

    TEST_CASE("the limits on size are enforced without allocating") {
        // A mesh this large is never built in a test; the constants are what matters.
        CHECK(kMaxMeshVertices == 16777216u);
        CHECK(kMaxMeshIndices == 3u * 16777216u);
    }

    TEST_CASE("bounds enclose the positions in use, per submesh and overall") {
        MeshData mesh = makeTwoTriangles();
        // Not referred to by any triangle, so it must not widen the bounds.
        mesh.vertices.push_back({.position = Vec3(50.0f, 50.0f, 50.0f)});
        mesh.vertices[2].position.z = 3.0f;
        updateBounds(mesh);

        CHECK(mesh.bounds.min == Vec3(0.0f, 0.0f, 0.0f));
        CHECK(mesh.bounds.max == Vec3(1.0f, 1.0f, 3.0f));
        // First triangle: vertices 0, 1, 2.
        CHECK(mesh.submeshes[0].bounds.min == Vec3(0.0f, 0.0f, 0.0f));
        CHECK(mesh.submeshes[0].bounds.max == Vec3(1.0f, 1.0f, 3.0f));
        // Second triangle: vertices 0, 2, 3.
        CHECK(mesh.submeshes[1].bounds.min == Vec3(0.0f, 0.0f, 0.0f));
        CHECK(mesh.submeshes[1].bounds.max == Vec3(1.0f, 1.0f, 3.0f));
    }

    TEST_CASE("the bounds of an empty submesh are empty and do not affect the mesh") {
        MeshData mesh = makeTwoTriangles();
        mesh.submeshes = {{.firstIndex = 0, .indexCount = 0}, {.firstIndex = 0, .indexCount = 6}};
        updateBounds(mesh);
        CHECK(mesh.submeshes[0].bounds.isEmpty());
        CHECK(mesh.bounds.min == Vec3(0.0f));
        CHECK(mesh.bounds.max == Vec3(1.0f, 1.0f, 0.0f));
    }

    TEST_CASE("appendSubmesh adds triangles behind what is there and keeps the bounds current") {
        MeshData mesh;
        mesh.name = "Assembled";

        const MeshData box = makeBox(Vec3(2.0f));
        const uint32_t first = appendSubmesh(mesh, box.vertices, box.indices);
        CHECK(first == 0);
        CHECK(mesh.bounds.min == Vec3(-1.0f));
        CHECK(mesh.bounds.max == Vec3(1.0f));

        std::vector<MeshVertex> raised = box.vertices;
        for (MeshVertex& vertex : raised) {
            vertex.position.y += 10.0f;
        }
        const uint32_t second = appendSubmesh(mesh, raised, box.indices);
        CHECK(second == 1);

        REQUIRE(mesh.submeshes.size() == 2);
        CHECK(mesh.submeshes[1].firstIndex == box.indices.size());
        CHECK(mesh.submeshes[1].indexCount == box.indices.size());
        CHECK(mesh.submeshes[1].bounds.min == Vec3(-1.0f, 9.0f, -1.0f));
        CHECK(mesh.vertices.size() == 2 * box.vertices.size());
        // The indices of the second part refer to its own vertices.
        CHECK(mesh.indices[box.indices.size()] == box.indices[0] + box.vertices.size());
        CHECK(mesh.bounds.max == Vec3(1.0f, 11.0f, 1.0f));
        CHECK(validateMesh(mesh).has_value());
    }

    TEST_CASE("appendSubmesh with nothing adds an empty submesh") {
        MeshData mesh = makeBox();
        const size_t indexCount = mesh.indices.size();
        const uint32_t added = appendSubmesh(mesh, {}, {});
        CHECK(added == 1);
        CHECK(mesh.submeshes[1].firstIndex == indexCount);
        CHECK(mesh.submeshes[1].indexCount == 0);
        CHECK(validateMesh(mesh).has_value());
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("appendSubmesh rejects indices that are not triangles or lead outside the vertices") {
        MeshData mesh;
        const std::vector<MeshVertex> vertices(3);
        const std::vector<uint32_t> incomplete = {0, 1};
        const std::vector<uint32_t> outside = {0, 1, 3};
        CHECK_THROWS_AS(appendSubmesh(mesh, vertices, incomplete), AssertionFailure);
        CHECK_THROWS_AS(appendSubmesh(mesh, vertices, outside), AssertionFailure);
    }
#endif

    TEST_CASE("validation holds the levels of detail to the same rules as the mesh") {
        // A level of the square that is the square again: valid, if pointless.
        const auto withLevel = [] {
            MeshData mesh = makeTwoTriangles();
            mesh.lods.push_back({.indices = {0, 1, 2, 0, 2, 3},
                                 .submeshes = {{.firstIndex = 0, .indexCount = 3}, {.firstIndex = 3, .indexCount = 3}},
                                 .error = 0.25f});
            return mesh;
        };
        CHECK(validateMesh(withLevel()).has_value());

        SUBCASE("a level with another number of submeshes") {
            MeshData mesh = withLevel();
            mesh.lods[0].submeshes.pop_back();
            CHECK(validationError(mesh).find("level of detail 1 has 1 submeshes, the mesh 2") != std::string::npos);
        }
        SUBCASE("ranges of a level that do not follow each other") {
            MeshData mesh = withLevel();
            mesh.lods[0].submeshes[1].firstIndex = 0;
            CHECK(validationError(mesh).find("submesh 1 of level of detail 1 has indices 0 to 3, expected whole "
                                             "triangles from index 3") != std::string::npos);
        }
        SUBCASE("ranges of a level that are not made of whole triangles") {
            MeshData mesh = withLevel();
            mesh.lods[0].submeshes = {{.firstIndex = 0, .indexCount = 4}, {.firstIndex = 4, .indexCount = 2}};
            CHECK(validationError(mesh).find("submesh 0 of level of detail 1 has indices 0 to 4") != std::string::npos);
        }
        SUBCASE("ranges of a level that do not cover its indices") {
            MeshData mesh = withLevel();
            mesh.lods[0].indices.insert(mesh.lods[0].indices.end(), {0, 1, 2});
            CHECK(validationError(mesh).find("the submeshes of level of detail 1 cover 6 indices, but it has 9") !=
                  std::string::npos);
        }
        SUBCASE("an index of a level without a vertex") {
            MeshData mesh = withLevel();
            mesh.lods[0].indices[4] = 7;
            CHECK(validationError(mesh).find("index 4 of level of detail 1 refers to vertex 7, but there are only 4") !=
                  std::string::npos);
        }
        SUBCASE("an error that is no number") {
            MeshData mesh = withLevel();
            mesh.lods[0].error = std::numeric_limits<float>::quiet_NaN();
            CHECK(validationError(mesh).find("level of detail 1 has an error of") != std::string::npos);
        }
        SUBCASE("an error smaller than that of the level before") {
            MeshData mesh = withLevel();
            mesh.lods.push_back(mesh.lods[0]);
            mesh.lods[1].error = 0.125f;
            CHECK(validationError(mesh).find("level of detail 2 has an error of 0.125") != std::string::npos);
        }
        SUBCASE("more levels than a mesh may have") {
            MeshData mesh = withLevel();
            while (mesh.lods.size() <= kMaxMeshLods) {
                mesh.lods.push_back(mesh.lods[0]);
            }
            CHECK(validationError(mesh).find("5 levels of detail exceed the limit of 4") != std::string::npos);
        }
    }
}
