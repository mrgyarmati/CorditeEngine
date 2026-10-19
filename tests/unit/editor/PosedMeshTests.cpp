#include "cordite/editor/PosedMesh.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A project with the skinned bar in it, and a world with an entity that draws the bar:
/// two metres tall, a fifth of a metre thick, standing on the origin. Its clip "Bend"
/// tips its upper half over to -X, about the joint one metre up.
struct Stage {
    Stage() {
        REQUIRE(exportGltf(test::makeSkinnedBar(), directory.path() / "bar.gltf").has_value());
        assets = unwrap(AssetManager::create({.rootDirectory = directory.path()}));
        bar = world.createEntity("Bar");
        world.add<MeshRendererComponent>(bar).mesh = idOf("bar.gltf#mesh/0");
    }

    [[nodiscard]] Uuid idOf(std::string_view reference) const {
        const AssetInfo* asset = assets->database().findByReference(reference);
        REQUIRE(asset != nullptr);
        return asset->id;
    }

    /// Gives an entity an animator that holds "Bend" at `time` - 0 upright, 1 bent - and
    /// poses the world.
    void pose(Entity entity, float time) {
        world.add<AnimatorComponent>(entity) = {.skeleton = idOf("bar.gltf#skeleton/0"),
                                                .clip = idOf("bar.gltf#animation/0"),
                                                .time = time,
                                                .loop = false,
                                                .playing = false};
        update();
    }

    void update() {
        animation.evaluate(world, *assets);
        world.updateWorldTransforms();
    }

    [[nodiscard]] PosedMesh mesh(Entity entity) {
        std::optional<PosedMesh> posed = posedMeshOf(world, *assets, entity);
        REQUIRE(posed.has_value());
        return std::move(*posed);
    }

    /// How far along a ray towards -Z from (x, y, 5) the bar of `entity` is hit.
    [[nodiscard]] std::optional<float> hitFromTheFront(Entity entity, float x, float y) {
        const std::optional<MeshHit> hit =
            intersect(Ray{Vec3(x, y, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}, mesh(entity), world.worldMatrix(entity));
        return hit ? std::optional<float>(hit->distance) : std::nullopt;
    }

    TempDirectory directory;
    std::unique_ptr<AssetManager> assets;
    World world;
    AnimationSystem animation;
    Entity bar;
};

} // namespace

TEST_SUITE("PosedMesh") {
    TEST_CASE("a mesh without a pose is where it was modelled") {
        Stage stage;
        stage.update();
        const PosedMesh mesh = stage.mesh(stage.bar);
        REQUIRE(mesh.mesh != nullptr);
        CHECK(mesh.positions.empty());
        CHECK(mesh.bounds.min == mesh.mesh->bounds.min);
        CHECK(mesh.bounds.max == mesh.mesh->bounds.max);
        CHECK(mesh.position(0) == mesh.mesh->vertices[0].position);
        // Hit where it stands: on the side that faces +Z, a tenth of a metre out.
        CHECK(stage.hitFromTheFront(stage.bar, 0.0f, 1.7f).value() == doctest::Approx(4.9f));
        CHECK_FALSE(stage.hitFromTheFront(stage.bar, -0.5f, 1.0f).has_value());

        // A mesh that follows no joints is no different.
        const Entity cube = stage.world.createEntity("Cube");
        stage.world.add<MeshRendererComponent>(cube).mesh = builtinMeshId(BuiltinMesh::Cube);
        stage.pose(cube, 1.0f);
        CHECK(stage.mesh(cube).positions.empty());
        CHECK(stage.hitFromTheFront(cube, 0.2f, 0.2f).value() == doctest::Approx(4.5f));
    }

    TEST_CASE("an entity that draws no mesh has none") {
        Stage stage;
        const Entity empty = stage.world.createEntity("Empty");
        CHECK_FALSE(posedMeshOf(stage.world, *stage.assets, empty).has_value());
        // A renderer without a mesh, and one whose mesh does not exist or is no mesh.
        MeshRendererComponent& renderer = stage.world.add<MeshRendererComponent>(empty);
        CHECK_FALSE(posedMeshOf(stage.world, *stage.assets, empty).has_value());
        renderer.mesh = *Uuid::parse("11111111-1111-4111-8111-111111111111");
        CHECK_FALSE(posedMeshOf(stage.world, *stage.assets, empty).has_value());
        renderer.mesh = stage.idOf("bar.gltf#skeleton/0");
        CHECK_FALSE(posedMeshOf(stage.world, *stage.assets, empty).has_value());
    }

    TEST_CASE("a skinned mesh is where its pose puts it") {
        Stage stage;
        stage.pose(stage.bar, 1.0f);
        const PosedMesh mesh = stage.mesh(stage.bar);
        REQUIRE(mesh.positions.size() == mesh.mesh->vertices.size());
        // Bent a quarter turn about the joint one metre up: the upper half lies along
        // -X at that height, and the ring half way up the lower half - which follows
        // both joints - is drawn out to +X.
        CHECK_NEAR(mesh.bounds.min, Vec3(-1.0f, 0.0f, -0.1f), 1.0e-5f);
        CHECK_NEAR(mesh.bounds.max, Vec3(0.3f, 1.1f, 0.1f), 1.0e-5f);
        for (size_t index = 0; index < mesh.positions.size(); ++index) {
            CHECK(mesh.position(index) == mesh.positions[index]);
        }

        // The upper half is hit where it lies, and no longer where it stood.
        CHECK(stage.hitFromTheFront(stage.bar, -0.5f, 1.0f).value() == doctest::Approx(4.9f));
        CHECK(stage.hitFromTheFront(stage.bar, -0.95f, 1.05f).value() == doctest::Approx(4.9f));
        CHECK_FALSE(stage.hitFromTheFront(stage.bar, 0.0f, 1.7f).has_value());
        CHECK_FALSE(stage.hitFromTheFront(stage.bar, -1.1f, 1.0f).has_value());
        // The foot has not moved.
        CHECK(stage.hitFromTheFront(stage.bar, 0.0f, 0.1f).value() == doctest::Approx(4.9f));

        // The entity places what the pose made: moved, and twice as large.
        stage.world.transform(stage.bar) = {.position = Vec3(3.0f, 0.0f, 1.0f), .scale = Vec3(2.0f)};
        stage.update();
        CHECK(stage.hitFromTheFront(stage.bar, 2.0f, 2.0f).value() == doctest::Approx(3.8f));
        CHECK_FALSE(stage.hitFromTheFront(stage.bar, -0.5f, 1.0f).has_value());
    }

    TEST_CASE("the pose is that of the nearest entity above, when it has every joint the mesh follows") {
        Stage stage;
        const Entity figure = stage.world.createEntity("Figure");
        REQUIRE(stage.world.setParent(stage.bar, figure, false).has_value());
        stage.pose(figure, 1.0f);
        CHECK_NEAR(stage.mesh(stage.bar).bounds.min, Vec3(-1.0f, 0.0f, -0.1f), 1.0e-5f);
        CHECK(stage.hitFromTheFront(stage.bar, -0.5f, 1.0f).has_value());

        // A pose of two joints, for a mesh that follows three: as it was modelled.
        stage.world.remove<AnimatorComponent>(figure);
        stage.update();
        const Mat4 aside = glm::translate(Mat4(1.0f), Vec3(5.0f, 0.0f, 0.0f));
        stage.world.add<SkeletonPoseComponent>(figure).skinning = {aside, aside};
        CHECK(stage.mesh(stage.bar).positions.empty());
        CHECK(stage.hitFromTheFront(stage.bar, 0.0f, 1.7f).has_value());
        // With all three, it is moved aside whole.
        stage.world.tryGet<SkeletonPoseComponent>(figure)->skinning = {aside, aside, aside};
        CHECK_NEAR(stage.mesh(stage.bar).bounds.min, Vec3(4.9f, 0.0f, -0.1f), 1.0e-5f);
        CHECK(stage.hitFromTheFront(stage.bar, 5.0f, 1.7f).has_value());

        // A joint whose matrix is no number moves nothing, as on the GPU.
        const Mat4 nothing(std::numeric_limits<float>::quiet_NaN());
        stage.world.tryGet<SkeletonPoseComponent>(figure)->skinning = {Mat4(1.0f), nothing, nothing};
        const PosedMesh broken = stage.mesh(stage.bar);
        CHECK_NEAR(broken.bounds.min, Vec3(-0.1f, 0.0f, -0.1f), 1.0e-5f);
        CHECK_NEAR(broken.bounds.max, Vec3(0.1f, 2.0f, 0.1f), 1.0e-5f);
    }

    TEST_CASE("a hit says which submesh it met, the nearest whatever the order of the submeshes") {
        // Two squares facing +Z: the first a metre behind, two wide; the second in front,
        // over its right half alone.
        const auto square = [](float left, float right, float z) {
            return std::vector<Vec3>{{left, -1.0f, z}, {right, -1.0f, z}, {right, 1.0f, z}, {left, 1.0f, z}};
        };
        const std::vector<std::vector<Vec3>> squares = {square(-1.0f, 1.0f, -1.0f), square(0.0f, 1.0f, 0.0f)};
        auto data = std::make_shared<MeshData>();
        for (const std::vector<Vec3>& corners : squares) {
            const auto first = static_cast<uint32_t>(data->vertices.size());
            for (const Vec3& corner : corners) {
                data->vertices.push_back({.position = corner, .normal = Vec3(0.0f, 0.0f, 1.0f)});
            }
            data->submeshes.push_back({.firstIndex = static_cast<uint32_t>(data->indices.size()), .indexCount = 6});
            for (const uint32_t corner : std::initializer_list<uint32_t>{0, 1, 2, 0, 2, 3}) {
                data->indices.push_back(first + corner);
            }
        }
        updateBounds(*data);
        REQUIRE(validateMesh(*data).has_value());
        const PosedMesh mesh{.mesh = data, .bounds = data->bounds};
        // Placed a metre further towards the rays than it was modelled.
        const Mat4 placement = glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 1.0f));

        const std::optional<MeshHit> front =
            intersect(Ray{Vec3(0.5f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}, mesh, placement);
        REQUIRE(front.has_value());
        CHECK(front->submesh == 1);
        CHECK(front->distance == doctest::Approx(4.0f));
        const std::optional<MeshHit> behind =
            intersect(Ray{Vec3(-0.5f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}, mesh, placement);
        REQUIRE(behind.has_value());
        CHECK(behind->submesh == 0);
        CHECK(behind->distance == doctest::Approx(5.0f));
        CHECK_FALSE(intersect(Ray{Vec3(-1.5f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}, mesh, placement).has_value());
    }

    TEST_CASE("a ray through the world meets the nearest mesh of an entity that is enabled, or none") {
        Stage stage;
        World& world = stage.world;
        // Two cubes along the way of the ray, away from the bar.
        const auto cube = [&](float z) {
            const Entity entity = world.createEntity("Cube");
            world.add<MeshRendererComponent>(entity).mesh = builtinMeshId(BuiltinMesh::Cube);
            world.transform(entity).position = Vec3(5.0f, 0.0f, z);
            return entity;
        };
        const Entity near = cube(0.0f);
        const Entity far = cube(-3.0f);
        stage.update();
        const Ray ray{Vec3(5.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)};

        std::optional<WorldMeshHit> hit = raycastMeshes(world, *stage.assets, ray);
        REQUIRE(hit.has_value());
        CHECK(hit->entity == near);
        CHECK(hit->hit.distance == doctest::Approx(4.5f));
        CHECK(hit->hit.submesh == 0);

        // What is switched off is not there to be met.
        world.setEnabled(near, false);
        hit = raycastMeshes(world, *stage.assets, ray);
        REQUIRE(hit.has_value());
        CHECK(hit->entity == far);
        CHECK(hit->hit.distance == doctest::Approx(7.5f));

        CHECK_FALSE(raycastMeshes(world, *stage.assets, Ray{Vec3(9.0f, 0.0f, 5.0f), Vec3(0.0f, 0.0f, -1.0f)}));
    }

    TEST_CASE("a mesh that is flattened to nothing cannot be hit") {
        Stage stage;
        stage.world.transform(stage.bar).scale = Vec3(1.0f, 1.0f, 0.0f);
        stage.update();
        CHECK_FALSE(stage.hitFromTheFront(stage.bar, 0.0f, 1.0f).has_value());
        // A ray that goes nowhere hits nothing either.
        const Ray still{Vec3(0.0f, 1.0f, 5.0f), Vec3(0.0f)};
        stage.world.transform(stage.bar).scale = Vec3(1.0f);
        stage.update();
        CHECK_FALSE(intersect(still, stage.mesh(stage.bar), stage.world.worldMatrix(stage.bar)).has_value());
    }
}
