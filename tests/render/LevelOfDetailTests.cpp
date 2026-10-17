#include "cordite/renderer/LevelOfDetail.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Mesh.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels wide and high.
constexpr uint32_t kSize = 128;

/// A ball of the built-in sphere in front of a camera at the origin that looks along -Z.
struct Ball {
    Ball() {
        scene.writeFlatMaterial("white", Vec3(1.0f));
        scene.open();
        mesh = unwrap(scene.assets->loadMesh(builtinMeshId(BuiltinMesh::Sphere)));
        ball = scene.add(BuiltinMesh::Sphere, scene.material("white"), at(Vec3(0.0f, 0.0f, -2.0f)));
        view = scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 50.0f);
    }

    /// Puts the ball `distance` metres in front of the camera.
    void placeAt(float distance) { scene.world.transform(ball).position = Vec3(0.0f, 0.0f, -distance); }

    Image render(float lodErrorPixels = 1.0f, RenderDebugView debugView = RenderDebugView::None) {
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.lodErrorPixels = lodErrorPixels;
        settings.debugView = debugView;
        return scene.render(view, settings);
    }

    /// The level the ball is to be drawn at, as LevelOfDetail.h works it out.
    [[nodiscard]] uint32_t expectedLevel(float lodErrorPixels = 1.0f) {
        scene.world.updateWorldTransforms();
        const Mat4& placement = scene.world.worldMatrix(ball);
        std::vector<float> errors;
        for (const MeshLod& lod : mesh->lods) {
            errors.push_back(lod.error);
        }
        return selectLod(errors, mesh->bounds.transformed(placement), scaleOf(placement),
                         lodViewOf(view, kSize, lodErrorPixels));
    }

    /// The triangles of the sphere at `level`.
    [[nodiscard]] uint64_t trianglesAt(uint32_t level) const {
        return level == 0 ? mesh->triangleCount() : mesh->lods[level - 1].indices.size() / 3;
    }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene{UVec2(kSize)};
    std::shared_ptr<const MeshData> mesh;
    Entity ball;
    CameraView view;
};

} // namespace

TEST_SUITE("LevelsOfDetail") {
    TEST_CASE("a fine mesh comes with levels of detail, and one of few triangles without") {
        Ball ball;
        CHECK(ball.mesh->lods.size() >= 2);
        CHECK(unwrap(ball.scene.assets->loadMesh(builtinMeshId(BuiltinMesh::Cube)))->lods.empty());
    }

    TEST_CASE("a mesh further away is drawn at a coarser level, with fewer triangles, and casts the shadow of it") {
        Ball ball;
        // A spot light beside the camera that casts the ball's shadow into one map.
        LightComponent spot{.type = LightType::Spot, .range = 200.0f, .outerConeDegrees = 20.0f, .castsShadows = true};
        spot.innerConeDegrees = 10.0f;
        ball.scene.addLight(spot, Vec3(0.2f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));

        uint32_t previous = 0;
        for (const float distance : {1.0f, 4.0f, 16.0f, 64.0f}) {
            CAPTURE(distance);
            ball.placeAt(distance);
            const uint32_t level = ball.expectedLevel();
            static_cast<void>(ball.render());
            CHECK(ball.statistics().lodInstances[level] == 1);
            CHECK(ball.statistics().basePass.triangles == ball.trianglesAt(level));
            CHECK(ball.statistics().shadowPass.triangles == ball.trianglesAt(level));
            CHECK(level >= previous);
            previous = level;
        }
        // Far enough to reach the coarse levels; near enough at first to see all of it.
        CHECK(previous >= 2);
        ball.placeAt(1.0f);
        CHECK(ball.expectedLevel() == 0);

        // A view that allows no error draws it at its finest, and so do its shadows.
        ball.placeAt(64.0f);
        static_cast<void>(ball.render(0.0f));
        CHECK(ball.statistics().lodInstances[0] == 1);
        CHECK(ball.statistics().basePass.triangles == ball.trianglesAt(0));
        CHECK(ball.statistics().shadowPass.triangles == ball.trianglesAt(0));
        // One that allows more takes a coarser level sooner.
        ball.placeAt(4.0f);
        CHECK(ball.expectedLevel(4.0f) > ball.expectedLevel(1.0f));
        checkNoValidationIssues(*ball.scene.context.graphics);
    }

    TEST_CASE("meshes of one kind at different levels are drawn in a call each") {
        Ball ball;
        ball.placeAt(1.0f);
        const uint32_t near = ball.expectedLevel();
        // A second ball further away, beside the first.
        const Entity far =
            ball.scene.add(BuiltinMesh::Sphere, ball.scene.material("white"), at(Vec3(1.5f, 0.0f, -20.0f)));
        static_cast<void>(ball.render());
        ball.scene.world.updateWorldTransforms();
        const Mat4& placement = ball.scene.world.worldMatrix(far);
        std::vector<float> errors;
        for (const MeshLod& lod : ball.mesh->lods) {
            errors.push_back(lod.error);
        }
        const uint32_t farLevel = selectLod(errors, ball.mesh->bounds.transformed(placement), scaleOf(placement),
                                            lodViewOf(ball.view, kSize, 1.0f));
        REQUIRE(farLevel > near);
        CHECK(ball.statistics().basePass.drawCalls == 2);
        CHECK(ball.statistics().basePass.triangles == ball.trianglesAt(near) + ball.trianglesAt(farLevel));
        // Allowing no error, they are one mesh at one level again: one call of two.
        static_cast<void>(ball.render(0.0f));
        CHECK(ball.statistics().basePass.drawCalls == 1);
        CHECK(ball.statistics().basePass.instances == 2);
        checkNoValidationIssues(*ball.scene.context.graphics);
    }

    TEST_CASE("a coarser level looks like the finest to within a pixel") {
        Ball ball;
        ball.placeAt(4.0f);
        REQUIRE(ball.expectedLevel() >= 1);
        const Image coarse = ball.render();
        const Image finest = ball.render(0.0f);
        // The ball is some thirty-five pixels across. What may differ is a pixel along its
        // edge.
        uint32_t covered = 0;
        uint32_t different = 0;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                covered += finest.pixel(x, y).r > 0.5f ? 1 : 0;
                different += coarse.pixel(x, y) == finest.pixel(x, y) ? 0 : 1;
            }
        }
        CHECK(covered > 800);
        CHECK(different * 10 < covered);
        checkNoValidationIssues(*ball.scene.context.graphics);
    }

    TEST_CASE("the levels of detail view shows each mesh in the color of its level") {
        Ball ball;
        // Up close, the mesh itself: grey. The middle of the ball faces the camera, along
        // the horizon, which the view shades at three quarters and a little more.
        ball.placeAt(1.0f);
        REQUIRE(ball.expectedLevel() == 0);
        const float facing = 0.55f + 0.45f * 0.5f;
        checkPixel(ball.render(1.0f, RenderDebugView::Lod), kSize / 2, kSize / 2, Vec3(0.60f) * facing);
        // Further away, its levels.
        const std::vector<Vec3> colors = {Vec3(0.60f, 0.60f, 0.60f), Vec3(0.20f, 0.75f, 0.20f),
                                          Vec3(0.85f, 0.80f, 0.15f), Vec3(0.90f, 0.45f, 0.10f),
                                          Vec3(0.85f, 0.15f, 0.10f)};
        for (const float distance : {6.0f, 20.0f}) {
            CAPTURE(distance);
            ball.placeAt(distance);
            const uint32_t level = ball.expectedLevel();
            REQUIRE(level >= 1);
            const Image picture = ball.render(1.0f, RenderDebugView::Lod);
            CHECK(ball.statistics().lodInstances[level] == 1);
            // The normal of a coarse level's triangle in the middle is not quite the
            // sphere's: a little more tolerance than for the mesh itself.
            CHECK_NEAR(Vec3(picture.pixel(kSize / 2, kSize / 2)), test::displayed(colors[level] * facing), 0.04f);
        }
        checkNoValidationIssues(*ball.scene.context.graphics);
    }
}
