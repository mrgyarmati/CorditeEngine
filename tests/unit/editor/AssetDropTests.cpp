#include "cordite/editor/AssetDrop.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <optional>
#include <vector>

using namespace cordite;

namespace {

const DropReach kReach{.grid = 50.0f, .ahead = 10.0f};

/// A ray from `origin` towards `towards`, of length one.
Ray rayFrom(Vec3 origin, Vec3 towards) {
    return Ray{origin, glm::normalize(towards)};
}

} // namespace

TEST_SUITE("AssetDrop") {
    TEST_CASE("what can be placed is placed where it is dropped, a material only onto a mesh, and nothing else") {
        for (const AssetType type : {AssetType::Model, AssetType::Prefab, AssetType::Mesh}) {
            CHECK(viewportDropOf(type) == ViewportDrop::Placed);
        }
        CHECK(viewportDropOf(AssetType::Material) == ViewportDrop::OntoMesh);
        for (const AssetType type :
             {AssetType::Texture, AssetType::Scene, AssetType::Skeleton, AssetType::Animation, AssetType::Script,
              AssetType::Sound, AssetType::RenderTarget, AssetType::Sequence}) {
            CHECK(viewportDropOf(type) == ViewportDrop::None);
        }
    }

    TEST_CASE("something dropped lands on the surface the line of sight meets first") {
        // Even with the grid further along the way.
        const Ray down = rayFrom(Vec3(1.0f, 10.0f, 2.0f), Vec3(0.0f, -1.0f, 0.0f));
        CHECK_NEAR(dropPoint(down, 3.5f, kReach), Vec3(1.0f, 6.5f, 2.0f), 1.0e-6f);
        // A surface beyond the reach of the grid counts all the same: it is seen.
        CHECK_NEAR(dropPoint(down, 70.0f, kReach), Vec3(1.0f, -60.0f, 2.0f), 1.0e-5f);
    }

    TEST_CASE("where no surface is in the way, something dropped lands on the grid, from above or below") {
        const Vec3 ahead = dropPoint(rayFrom(Vec3(0.0f, 10.0f, 10.0f), Vec3(0.0f, -1.0f, -1.0f)), std::nullopt, kReach);
        CHECK_NEAR(ahead, Vec3(0.0f), 1.0e-5f);
        // On it, not a rounding error above or below - where the arithmetic of the way there
        // makes one, as it does along some of these.
        size_t roundedOff = 0;
        for (int step = 1; step <= 100; ++step) {
            const auto along = static_cast<float>(step);
            const Ray ray =
                rayFrom(Vec3(0.1f * along, 0.5f + 0.0137f * along, 0.3f), Vec3(0.2f, -1.0f, -0.013f * along));
            const float distance = -ray.origin.y / ray.direction.y;
            roundedOff += (ray.origin + ray.direction * distance).y != 0.0f ? 1 : 0;
            CHECK(dropPoint(ray, std::nullopt, kReach).y == 0.0f);
        }
        CHECK(roundedOff > 0);
        // A camera below the grid sees it too.
        CHECK_NEAR(dropPoint(rayFrom(Vec3(2.0f, -3.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)), std::nullopt, kReach),
                   Vec3(2.0f, 0.0f, 0.0f), 1.0e-6f);
        // As far as the grid is drawn, and no further.
        const Ray far = rayFrom(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, -49.0f));
        CHECK(dropPoint(far, std::nullopt, kReach).y == 0.0f);
        const Ray beyond = rayFrom(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, -51.0f));
        CHECK_NEAR(dropPoint(beyond, std::nullopt, kReach), beyond.origin + beyond.direction * 10.0f, 1.0e-5f);
    }

    TEST_CASE("where neither a surface nor the grid is ahead, something dropped lands where the camera looks") {
        // Up, away from the grid.
        const Ray up = rayFrom(Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 1.0f));
        CHECK_NEAR(dropPoint(up, std::nullopt, kReach), up.origin + up.direction * 10.0f, 1.0e-5f);
        // Along the grid, which a level line of sight never meets.
        const Ray level = rayFrom(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f));
        CHECK_NEAR(dropPoint(level, std::nullopt, kReach), Vec3(10.0f, 2.0f, 0.0f), 1.0e-5f);
        // From a camera on the grid, looking along it.
        const Ray onGrid = rayFrom(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
        CHECK_NEAR(dropPoint(onGrid, std::nullopt, kReach), Vec3(0.0f, 0.0f, -10.0f), 1.0e-6f);
        // As far ahead as it is told.
        CHECK_NEAR(dropPoint(up, std::nullopt, {.grid = 50.0f, .ahead = 3.0f}), up.origin + up.direction * 3.0f,
                   1.0e-5f);
    }

    TEST_CASE("a material is put in the place of the part it was dropped on, and what goes before it is kept") {
        const Uuid first = Uuid::generate();
        const Uuid second = Uuid::generate();
        const Uuid dropped = Uuid::generate();
        CHECK(materialsWith({}, 0, dropped) == std::vector<Uuid>{dropped});
        // Places that had none get the default material: nil.
        CHECK(materialsWith({}, 2, dropped) == std::vector<Uuid>{Uuid(), Uuid(), dropped});
        const std::vector<Uuid> two = {first, second};
        CHECK(materialsWith(two, 0, dropped) == std::vector<Uuid>{dropped, second});
        CHECK(materialsWith(two, 1, dropped) == std::vector<Uuid>{first, dropped});
        CHECK(materialsWith(two, 3, dropped) == std::vector<Uuid>{first, second, Uuid(), dropped});
        // The same material in its place again is the list as it was.
        CHECK(materialsWith(two, 1, second) == two);
    }
}
