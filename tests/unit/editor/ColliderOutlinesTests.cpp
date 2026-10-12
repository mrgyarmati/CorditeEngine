#include "cordite/editor/ColliderOutlines.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Primitives.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

/// A world with the engine's own meshes, and the physics that makes colliders of them.
struct Stage {
    Stage() : assets(unwrap(AssetManager::create())), physics(unwrap(PhysicsSystem::create())) {}

    /// An entity whose collider is made of a mesh the engine brings along.
    Entity addCollider(const char* name, ShapeType shape, BuiltinMesh mesh) {
        const Entity entity = world.createEntity(name);
        world.add<ColliderComponent>(entity) = {.shape = shape, .mesh = builtinMeshId(mesh)};
        return entity;
    }

    void synchronize() { physics->synchronize(world, *assets); }

    World world;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<PhysicsSystem> physics;
};

} // namespace

TEST_SUITE("ColliderOutlines") {
    TEST_CASE("the outline of a collider is what the physics has made of its mesh") {
        Stage stage;
        stage.addCollider("Crate", ShapeType::ConvexHull, BuiltinMesh::Cube);
        stage.addCollider("Ring", ShapeType::Mesh, BuiltinMesh::Torus);
        ColliderOutlines outlines;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Uuid torus = builtinMeshId(BuiltinMesh::Torus);

        // Before the physics has seen the colliders there is nothing to show.
        CHECK(outlines.find(*stage.physics, cube, ShapeType::ConvexHull) == nullptr);
        CHECK(outlines.size() == 0);
        stage.synchronize();

        // The hull around a cube: twelve edges, each a metre long - not the eighteen of
        // the triangles the cube is drawn with.
        const ColliderOutline* crate = outlines.find(*stage.physics, cube, ShapeType::ConvexHull);
        REQUIRE(crate != nullptr);
        REQUIRE(crate->edges.size() == 2 * 12);
        for (size_t edge = 0; edge < crate->edges.size(); edge += 2) {
            CHECK(glm::distance(crate->edges[edge], crate->edges[edge + 1]) == doctest::Approx(1.0f).epsilon(1.0e-4));
        }
        CHECK_NEAR(crate->bounds.min, Vec3(-0.5f), 1.0e-4f);
        CHECK_NEAR(crate->bounds.max, Vec3(0.5f), 1.0e-4f);

        // The triangles of a ring: every side of every triangle once, and so three for
        // every two triangles of a surface that is closed.
        const std::shared_ptr<const MeshData> mesh = unwrap(stage.assets->loadMesh(torus));
        const ColliderOutline* ring = outlines.find(*stage.physics, torus, ShapeType::Mesh);
        REQUIRE(ring != nullptr);
        CHECK(ring->edges.size() == 2 * (mesh->triangleCount() * 3 / 2));
        CHECK_NEAR(ring->bounds.min, mesh->bounds.min, 1.0e-3f);
        CHECK_NEAR(ring->bounds.max, mesh->bounds.max, 1.0e-3f);
        CHECK(outlines.size() == 2);

        // Asked for again, it is the one that was kept.
        CHECK(outlines.find(*stage.physics, torus, ShapeType::Mesh) == ring);
        CHECK(outlines.size() == 2);
        // What no collider is made of has none: the other shape of a mesh, another mesh, no mesh.
        CHECK(outlines.find(*stage.physics, torus, ShapeType::ConvexHull) == nullptr);
        CHECK(outlines.find(*stage.physics, builtinMeshId(BuiltinMesh::Cone), ShapeType::Mesh) == nullptr);
        CHECK(outlines.find(*stage.physics, Uuid(), ShapeType::Mesh) == nullptr);
        CHECK(outlines.find(*stage.physics, cube, ShapeType::Box) == nullptr);
        CHECK(outlines.size() == 2);
    }

    TEST_CASE("a shape of more edges than are drawn has its box, and no edges") {
        // A project with a ball of more triangles than an outline has edges.
        const test::TempDirectory directory;
        GltfDocument document;
        document.meshes.push_back({.mesh = makeSphere(0.5f, 256, 128), .materials = {0}});
        document.materials.push_back({.name = "Paint"});
        document.nodes.push_back({.name = "Ball", .mesh = 0});
        document.roots = {0};
        REQUIRE(document.meshes[0].mesh.triangleCount() * 3 / 2 > kMaxColliderOutlineEdges);
        REQUIRE(exportGltf(document, directory.path() / "ball.glb").has_value());

        Stage stage;
        stage.assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        const AssetInfo* asset = stage.assets->database().findByReference("ball.glb#mesh/0");
        REQUIRE(asset != nullptr);
        const Entity ball = stage.world.createEntity("Ball");
        stage.world.add<ColliderComponent>(ball) = {.shape = ShapeType::Mesh, .mesh = asset->id};
        stage.synchronize();

        ColliderOutlines outlines;
        const ColliderOutline* outline = outlines.find(*stage.physics, asset->id, ShapeType::Mesh);
        REQUIRE(outline != nullptr);
        CHECK(outline->edges.empty());
        CHECK_NEAR(outline->bounds.min, Vec3(-0.5f), 1.0e-3f);
        CHECK_NEAR(outline->bounds.max, Vec3(0.5f), 1.0e-3f);
        // The hull around the same ball is of few corners, and has its edges.
        stage.world.tryGet<ColliderComponent>(ball)->shape = ShapeType::ConvexHull;
        stage.synchronize();
        const ColliderOutline* hull = outlines.find(*stage.physics, asset->id, ShapeType::ConvexHull);
        REQUIRE(hull != nullptr);
        CHECK_FALSE(hull->edges.empty());
        CHECK(hull->edges.size() < 2 * 3 * 256);
    }

    TEST_CASE("outlines of what the physics has forgotten are forgotten too") {
        Stage stage;
        stage.addCollider("Crate", ShapeType::ConvexHull, BuiltinMesh::Cube);
        stage.addCollider("Drum", ShapeType::ConvexHull, BuiltinMesh::Cylinder);
        stage.synchronize();
        ColliderOutlines outlines;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Uuid cylinder = builtinMeshId(BuiltinMesh::Cylinder);
        REQUIRE(outlines.find(*stage.physics, cube, ShapeType::ConvexHull) != nullptr);
        REQUIRE(outlines.find(*stage.physics, cylinder, ShapeType::ConvexHull) != nullptr);
        CHECK(outlines.size() == 2);

        // The assets are refreshed: the physics makes its geometry anew, under new ids.
        // The outlines of the old are of no use to anybody, and go.
        REQUIRE(stage.assets->refresh().has_value());
        stage.synchronize();
        const ColliderOutline* crate = outlines.find(*stage.physics, cube, ShapeType::ConvexHull);
        REQUIRE(crate != nullptr);
        CHECK(crate->edges.size() == 2 * 12);
        REQUIRE(outlines.find(*stage.physics, cylinder, ShapeType::ConvexHull) != nullptr);
        CHECK(outlines.size() == 2);

        // With everything forgotten there is nothing to show until it is made again.
        stage.physics->clear();
        CHECK(outlines.find(*stage.physics, cube, ShapeType::ConvexHull) == nullptr);
        stage.synchronize();
        CHECK(outlines.find(*stage.physics, cube, ShapeType::ConvexHull) != nullptr);
        CHECK(outlines.size() == 1);
    }
}
