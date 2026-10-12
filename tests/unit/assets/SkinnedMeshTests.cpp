#include "cordite/assets/Mesh.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/assets/Skeleton.h"

#include "AssetTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::makeBarMesh;
using cordite::test::skinnedCorners;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A triangle whose corners follow joints 0, 1 and 2, one each.
MeshData skinnedTriangle() {
    const std::array<MeshVertex, 3> vertices = {MeshVertex{.position = Vec3(0.0f)},
                                                MeshVertex{.position = Vec3(1.0f, 0.0f, 0.0f)},
                                                MeshVertex{.position = Vec3(0.0f, 1.0f, 0.0f)}};
    const std::array<MeshSkinVertex, 3> skin = {MeshSkinVertex{.joints = {0, 0, 0, 0}},
                                                MeshSkinVertex{.joints = {1, 0, 0, 0}},
                                                MeshSkinVertex{.joints = {2, 0, 0, 0}}};
    const std::array<uint32_t, 3> indices = {0, 1, 2};
    MeshData mesh;
    mesh.name = "Triangle";
    appendSubmesh(mesh, vertices, skin, indices);
    return mesh;
}

} // namespace

TEST_SUITE("SkinnedMesh") {
    TEST_CASE("a mesh is skinned when it says how each of its vertices follows joints") {
        MeshData mesh = skinnedTriangle();
        CHECK(mesh.isSkinned());
        REQUIRE(mesh.skin.size() == 3);
        CHECK(mesh.skin[1].joints == std::array<uint16_t, 4>{1, 0, 0, 0});
        CHECK(mesh.skin[1].weights == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
        CHECK(validateMesh(mesh).has_value());
        CHECK(jointsUsed(mesh) == 3);

        // A second part, skinned too, goes behind the first.
        const std::array<MeshVertex, 3> more = {MeshVertex{.position = Vec3(0.0f, 0.0f, 1.0f)},
                                                MeshVertex{.position = Vec3(1.0f, 0.0f, 1.0f)},
                                                MeshVertex{.position = Vec3(0.0f, 1.0f, 1.0f)}};
        const std::array<MeshSkinVertex, 3> moreSkin = {
            MeshSkinVertex{.joints = {7, 2, 0, 0}, .weights = {0.25f, 0.75f, 0.0f, 0.0f}}, MeshSkinVertex{},
            MeshSkinVertex{}};
        const std::array<uint32_t, 3> indices = {0, 1, 2};
        CHECK(appendSubmesh(mesh, more, moreSkin, indices) == 1);
        REQUIRE(mesh.skin.size() == 6);
        CHECK(mesh.skin[3] == moreSkin[0]);
        CHECK(validateMesh(mesh).has_value());
        CHECK(jointsUsed(mesh) == 8);

        // A mesh without that is rigid, and needs no joints.
        MeshData rigid;
        appendSubmesh(rigid, more, indices);
        CHECK_FALSE(rigid.isSkinned());
        CHECK(rigid.skin.empty());
        CHECK(jointsUsed(rigid) == 0);
        CHECK(normalizeSkinWeights(rigid) == 0);
    }

    TEST_CASE("a joint that has no weight is not one the mesh uses") {
        MeshData mesh = skinnedTriangle();
        mesh.skin[0] = {.joints = {1, 900, 0, 0}, .weights = {1.0f, 0.0f, 0.0f, 0.0f}};
        CHECK(jointsUsed(mesh) == 3);
        mesh.skin[0].weights = {0.5f, 0.5f, 0.0f, 0.0f};
        CHECK(jointsUsed(mesh) == 901);
    }

    TEST_CASE("every joint has a box around what follows it") {
        // The bar: its lowest rings follow the root, the middle ones the middle joint,
        // the top ones the tip - and a ring between two joints follows both.
        const std::vector<Aabb> bounds = jointBounds(makeBarMesh());
        REQUIRE(bounds.size() == 3);
        CHECK(bounds[0].min == Vec3(-0.1f, 0.0f, -0.1f));
        CHECK(bounds[0].max == Vec3(0.1f, 0.5f, 0.1f));
        CHECK(bounds[1].min == Vec3(-0.1f, 0.5f, -0.1f));
        CHECK(bounds[1].max == Vec3(0.1f, 1.5f, 0.1f));
        CHECK(bounds[2].min == Vec3(-0.1f, 1.5f, -0.1f));
        CHECK(bounds[2].max == Vec3(0.1f, 2.0f, 0.1f));

        // A joint nothing follows has an empty box; one that is named without a weight
        // is not followed.
        MeshData mesh = skinnedTriangle();
        mesh.skin[1] = {.joints = {4, 1, 0, 0}, .weights = {1.0f, 0.0f, 0.0f, 0.0f}};
        const std::vector<Aabb> sparse = jointBounds(mesh);
        REQUIRE(sparse.size() == 5);
        CHECK(sparse[0].min == Vec3(0.0f));
        CHECK(sparse[0].max == Vec3(0.0f));
        CHECK(sparse[1].isEmpty());
        CHECK(sparse[2].min == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(sparse[3].isEmpty());
        CHECK(sparse[4].max == Vec3(1.0f, 0.0f, 0.0f));

        // A vertex that follows several joints is in the box of each.
        mesh.skin[2] = {.joints = {0, 4, 2, 0}, .weights = {0.5f, 0.25f, 0.25f, 0.0f}};
        const std::vector<Aabb> shared = jointBounds(mesh);
        REQUIRE(shared.size() == 5);
        CHECK(shared[0].max == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(shared[4].min == Vec3(0.0f));
        CHECK(shared[4].max == Vec3(1.0f, 1.0f, 0.0f));

        // A mesh that is not skinned has no joints, and no boxes.
        mesh.skin.clear();
        CHECK(jointBounds(mesh).empty());
        CHECK(jointBounds(MeshData()).empty());
    }

    TEST_CASE("a skinned mesh whose vertices do not follow joints as they must is refused") {
        const auto refused = [](const MeshData& mesh, std::string_view because) {
            const Status status = validateMesh(mesh);
            REQUIRE_FALSE(status.has_value());
            const std::string& message = status.error().message;
            CHECK_MESSAGE(message.find(because) != std::string::npos, message);
            CHECK(message.find("'Triangle'") != std::string::npos);
        };

        MeshData mesh = skinnedTriangle();
        mesh.skin.pop_back();
        refused(mesh, "how 2 vertices follow their joints, but has 3");

        mesh = skinnedTriangle();
        mesh.skin[2].weights = {0.5f, 0.25f, 0.0f, 0.0f};
        refused(mesh, "vertex 2 has weights that are not one in all");
        mesh.skin[2].weights = {0.0f, 0.0f, 0.0f, 0.0f};
        refused(mesh, "not one in all");
        mesh.skin[2].weights = {1.5f, -0.5f, 0.0f, 0.0f};
        refused(mesh, "negative or no number");
        mesh.skin[2].weights = {kNotANumber, 1.0f, 0.0f, 0.0f};
        refused(mesh, "negative or no number");

        mesh = skinnedTriangle();
        mesh.skin[1].joints[3] = static_cast<uint16_t>(kMaxSkeletonJoints);
        refused(mesh, "vertex 1 follows a joint beyond the most a skeleton can have");
        mesh.skin[1].joints[3] = static_cast<uint16_t>(kMaxSkeletonJoints - 1);
        CHECK(validateMesh(mesh).has_value());

        // Within a thousandth of one in all is one in all: what rounding leaves.
        mesh = skinnedTriangle();
        mesh.skin[0].weights = {0.3334f, 0.3333f, 0.3333f, 0.0005f};
        CHECK(validateMesh(mesh).has_value());
    }

    TEST_CASE("weights are brought to one in all, and what has none follows its first joint") {
        MeshData mesh = skinnedTriangle();
        mesh.skin[0] = {.joints = {4, 5, 6, 7}, .weights = {2.0f, 1.0f, 1.0f, 0.0f}};
        mesh.skin[1] = {.joints = {4, 5, 6, 7}, .weights = {-1.0f, kNotANumber, 0.25f, 0.25f}};
        mesh.skin[2] = {.joints = {9, 5, 6, 7}, .weights = {0.0f, 0.0f, -3.0f, kNotANumber}};
        CHECK(normalizeSkinWeights(mesh) == 1);

        CHECK(mesh.skin[0].weights == std::array<float, 4>{0.5f, 0.25f, 0.25f, 0.0f});
        // A joint without weight is joint zero: there is one way to say nothing.
        CHECK(mesh.skin[0].joints == std::array<uint16_t, 4>{4, 5, 6, 0});
        CHECK(mesh.skin[1].weights == std::array<float, 4>{0.0f, 0.0f, 0.5f, 0.5f});
        CHECK(mesh.skin[1].joints == std::array<uint16_t, 4>{0, 0, 6, 7});
        CHECK(mesh.skin[2].weights == std::array<float, 4>{1.0f, 0.0f, 0.0f, 0.0f});
        CHECK(mesh.skin[2].joints == std::array<uint16_t, 4>{9, 0, 0, 0});
        CHECK(validateMesh(mesh).has_value());

        // What is as it should be already is left alone.
        const MeshData before = mesh;
        CHECK(normalizeSkinWeights(mesh) == 0);
        CHECK(mesh == before);
    }

    TEST_CASE("preparing a mesh for rendering keeps how each corner of each triangle follows its joints") {
        const MeshData bar = makeBarMesh();
        REQUIRE(validateMesh(bar).has_value());
        REQUIRE(bar.isSkinned());
        CHECK(bar.skin.size() == bar.vertices.size());
        CHECK(jointsUsed(bar) == 3);
        const auto corners = skinnedCorners(bar);
        // Four sides of four quads.
        CHECK(corners.size() == 4 * 4 * 6);

        // Optimizing what is optimized leaves the same surface, of the same size.
        MeshData again = bar;
        optimizeMesh(again);
        CHECK(again.vertices.size() == bar.vertices.size());
        CHECK(again.skin.size() == bar.skin.size());
        CHECK(skinnedCorners(again) == corners);

        // Flat normals give every corner a vertex of its own, and its joints with it.
        MeshData flat = bar;
        generateFlatNormals(flat);
        CHECK(flat.vertices.size() == flat.indices.size());
        CHECK(flat.skin.size() == flat.vertices.size());
        CHECK(skinnedCorners(flat) == corners);
        CHECK(validateMesh(flat).has_value());

        // Tangents split and weld vertices; the joints go where their vertices go.
        MeshData tangents = flat;
        generateTangents(tangents);
        CHECK(tangents.skin.size() == tangents.vertices.size());
        CHECK(tangents.vertices.size() < flat.vertices.size());
        CHECK(skinnedCorners(tangents) == corners);
        CHECK(validateMesh(tangents).has_value());

        optimizeMesh(flat);
        CHECK(flat.skin.size() == flat.vertices.size());
        CHECK(skinnedCorners(flat) == corners);
        CHECK(validateMesh(flat).has_value());
    }

    TEST_CASE("vertices that are alike but follow other joints are not made one") {
        // Two triangles that share an edge, as six vertices: the two pairs on the edge
        // are in the same place, and one of the pairs follows different joints.
        const std::array<MeshVertex, 6> vertices = {MeshVertex{.position = Vec3(0.0f)},
                                                    MeshVertex{.position = Vec3(1.0f, 0.0f, 0.0f)},
                                                    MeshVertex{.position = Vec3(0.0f, 1.0f, 0.0f)},
                                                    MeshVertex{.position = Vec3(1.0f, 0.0f, 0.0f)},
                                                    MeshVertex{.position = Vec3(1.0f, 1.0f, 0.0f)},
                                                    MeshVertex{.position = Vec3(0.0f, 1.0f, 0.0f)}};
        std::array<MeshSkinVertex, 6> skin{};
        skin[1].joints[0] = 3;
        skin[3].joints[0] = 3;
        skin[2].joints[0] = 1;
        skin[5].joints[0] = 2;
        const std::array<uint32_t, 6> indices = {0, 1, 2, 3, 4, 5};

        MeshData mesh;
        appendSubmesh(mesh, vertices, skin, indices);
        const auto corners = skinnedCorners(mesh);
        optimizeMesh(mesh);
        // One pair became one vertex; the other is two as before.
        CHECK(mesh.vertices.size() == 5);
        CHECK(mesh.skin.size() == 5);
        CHECK(skinnedCorners(mesh) == corners);

        // Without what tells them apart, both pairs are one.
        MeshData rigid;
        appendSubmesh(rigid, vertices, indices);
        optimizeMesh(rigid);
        CHECK(rigid.vertices.size() == 4);
        CHECK(rigid.skin.empty());

        // A weight of nothing is nothing, whichever zero it is written as.
        MeshData zeros;
        std::array<MeshSkinVertex, 6> signedSkin{};
        signedSkin[1].weights[3] = -0.0f;
        appendSubmesh(zeros, vertices, signedSkin, indices);
        optimizeMesh(zeros);
        CHECK(zeros.vertices.size() == 4);
    }

    TEST_CASE("a vertex nothing draws is dropped, and what it followed with it") {
        MeshData mesh = skinnedTriangle();
        // A fourth vertex that no triangle uses.
        mesh.vertices.push_back({.position = Vec3(5.0f)});
        mesh.skin.push_back({.joints = {40, 0, 0, 0}});
        REQUIRE(validateMesh(mesh).has_value());
        CHECK(jointsUsed(mesh) == 41);
        optimizeMesh(mesh);
        CHECK(mesh.vertices.size() == 3);
        CHECK(mesh.skin.size() == 3);
        CHECK(jointsUsed(mesh) == 3);
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("a mesh is skinned as a whole or not at all") {
        const std::array<MeshVertex, 3> vertices = {MeshVertex{.position = Vec3(0.0f)},
                                                    MeshVertex{.position = Vec3(1.0f, 0.0f, 0.0f)},
                                                    MeshVertex{.position = Vec3(0.0f, 1.0f, 0.0f)}};
        const std::array<MeshSkinVertex, 2> tooFew{};
        const std::array<uint32_t, 3> indices = {0, 1, 2};

        MeshData skinned = skinnedTriangle();
        CHECK_THROWS_AS(appendSubmesh(skinned, vertices, indices), test::AssertionFailure);
        MeshData rigid;
        appendSubmesh(rigid, vertices, indices);
        const std::array<MeshSkinVertex, 3> skin{};
        CHECK_THROWS_AS(appendSubmesh(rigid, vertices, skin, indices), test::AssertionFailure);
        MeshData empty;
        CHECK_THROWS_AS(appendSubmesh(empty, vertices, tooFew, indices), test::AssertionFailure);
    }
#endif
}
