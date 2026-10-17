#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/renderer/MotionVectors.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"
#include "support/SkinnedModelSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::kRed;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels a side.
constexpr uint32_t kSize = 64;

/// What a view that shows its motion vectors is drawn with.
SceneRenderSettings motionSettings() {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.debugView = RenderDebugView::Motion;
    return settings;
}

/// Where a point of the world is on a picture of `view`: (0, 0) its top left corner, (1, 1)
/// its bottom right, as a motion vector counts.
Vec2 pictureUvOf(const CameraView& view, const Vec4& point) {
    const Vec4 clip = view.viewProjection * point;
    return {clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f};
}

/// Checks that the pixel (x, y) of a picture of the Motion debug view shows `motion`.
void checkMotion(const Image& image, uint32_t x, uint32_t y, const Vec2& motion) {
    CAPTURE(motion);
    checkPixel(image, x, y, motionDebugColor(motion, UVec2(kSize)));
}

/// A grey floor seen from above, and a box on it.
struct Stage {
    Stage() : scene{UVec2(kSize)} {
        scene.writeFlatMaterial("grey", Vec3(0.5f));
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        floor = scene.add(BuiltinMesh::Plane, scene.material("grey"), at(Vec3(0.0f), Vec3(40.0f, 1.0f, 40.0f)));
        box = scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f, 0.5f, 0.0f)));
    }

    /// Looks straight down at the floor from above the point (x, 0, z), and sees eight
    /// metres from top to bottom: a metre is eight pixels.
    [[nodiscard]] CameraView fromAbove(float x = 0.0f, float z = 0.0f) const {
        return scene.orthographicView(Vec3(x, 10.0f, z), Vec3(x, 0.0f, z), 8.0f, kWorldForward);
    }

    SceneFixture scene;
    Entity floor;
    Entity box;
};

} // namespace

TEST_SUITE("MotionVectors") {
    TEST_CASE("the first picture of a view has nothing moving in it") {
        Stage stage;
        const Image image = stage.scene.render(stage.fromAbove(), motionSettings());
        checkMotion(image, 32, 32, Vec2(0.0f));
        checkMotion(image, 5, 5, Vec2(0.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what moves shows how far it went, and nothing else moves") {
        Stage stage;
        static_cast<void>(stage.scene.render(stage.fromAbove(), motionSettings()));
        // The same again: nothing moved, to the bit.
        const Image still = stage.scene.render(stage.fromAbove(), motionSettings());
        CHECK(still.pixel(32, 32) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        CHECK(still.pixel(5, 50) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));

        // A metre to the right of the picture - +X, seen from above with -Z up - is an
        // eighth of it.
        stage.scene.world.transform(stage.box).position.x = 1.0f;
        const Image moved = stage.scene.render(stage.fromAbove(), motionSettings());
        checkMotion(moved, 32 + 8, 32, Vec2(1.0f / 8.0f, 0.0f));
        // The floor did not move, nor where the box was.
        checkMotion(moved, 32 - 4, 32, Vec2(0.0f));
        checkMotion(moved, 5, 50, Vec2(0.0f));

        // Half a metre towards the bottom of the picture: +Z.
        stage.scene.world.transform(stage.box).position.z = 0.5f;
        const Image down = stage.scene.render(stage.fromAbove(), motionSettings());
        checkMotion(down, 32 + 8, 32 + 4, Vec2(0.0f, 0.5f / 8.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a picture of a moment no time has passed since has nothing moving in it") {
        Stage stage;
        static_cast<void>(stage.scene.render(stage.fromAbove(), motionSettings()));
        // Moved a metre in no time at all: a jump - an edit, a second picture of a moment -
        // not a motion.
        stage.scene.world.transform(stage.box).position.x = 1.0f;
        SceneRenderSettings sameMoment = motionSettings();
        sameMoment.deltaSeconds = 0.0f;
        const Image jumped = stage.scene.render(stage.fromAbove(), sameMoment);
        checkMotion(jumped, 32 + 8, 32, Vec2(0.0f));
        // The next picture goes on from where that one had it.
        stage.scene.world.transform(stage.box).position.x = 1.5f;
        const Image moved = stage.scene.render(stage.fromAbove(), motionSettings());
        checkMotion(moved, 32 + 12, 32, Vec2(0.5f / 8.0f, 0.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what the camera follows stands still in the picture, and what it passes moves") {
        Stage stage;
        static_cast<void>(stage.scene.render(stage.fromAbove(), motionSettings()));
        stage.scene.world.transform(stage.box).position.x = 1.0f;
        const Image image = stage.scene.render(stage.fromAbove(1.0f), motionSettings());
        // The box is in the middle of both pictures; the floor went a metre to the left.
        checkMotion(image, 32, 32, Vec2(0.0f));
        checkMotion(image, 10, 50, Vec2(-1.0f / 8.0f, 0.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("the sky moves as the camera turns, and not as it goes") {
        SceneFixture scene{UVec2(kSize)};
        scene.open();
        const CameraView ahead = scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 60.0f);
        static_cast<void>(scene.render(ahead, motionSettings()));
        // Gone two metres, looking the same way: the sky is where it was.
        const CameraView gone = scene.perspectiveView(Vec3(0.0f, 0.0f, -2.0f), Vec3(0.0f, 0.0f, -3.0f), 60.0f);
        const Image went = scene.render(gone, motionSettings());
        checkMotion(went, 32, 32, Vec2(0.0f));
        checkMotion(went, 3, 60, Vec2(0.0f));

        // Turned five degrees to the left: what was straight ahead is to the right now,
        // as far as its direction projects - at every pixel by how its own direction does.
        const CameraView turned = scene.perspectiveView(
            Vec3(0.0f, 0.0f, -2.0f),
            Vec3(0.0f, 0.0f, -2.0f) + glm::angleAxis(glm::radians(5.0f), kWorldUp) * kWorldForward, 60.0f);
        const Image image = scene.render(turned, motionSettings());
        for (const UVec2 pixel : {UVec2(32, 32), UVec2(4, 4), UVec2(60, 20), UVec2(16, 58)}) {
            CAPTURE(pixel);
            const Vec2 uv = (Vec2(pixel) + 0.5f) / static_cast<float>(kSize);
            const Vec3 direction = screenRay(turned, Vec2(pixel) + 0.5f, Vec2(kSize)).direction;
            const Vec2 before = pictureUvOf(gone, Vec4(direction, 0.0f));
            checkMotion(image, pixel.x, pixel.y, uv - before);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a picture after a cut, or through another camera, has nothing moving in it") {
        Stage stage;
        const Uuid first = Uuid::generate();
        SceneRenderSettings settings = motionSettings();
        settings.viewEntity = first;
        static_cast<void>(stage.scene.render(stage.fromAbove(), settings));
        stage.scene.world.transform(stage.box).position.x = 1.0f;
        // Eleven metres in one picture is another shot: the floor has not moved.
        const Image jumped = stage.scene.render(stage.fromAbove(11.0f), settings);
        checkMotion(jumped, 32, 32, Vec2(0.0f));
        checkMotion(jumped, 5, 50, Vec2(0.0f));

        // Through another camera: another shot, however close.
        stage.scene.world.transform(stage.box).position.x = 2.0f;
        settings.viewEntity = Uuid::generate();
        const Image other = stage.scene.render(stage.fromAbove(11.0f), settings);
        checkMotion(other, 32, 32, Vec2(0.0f));
        // Through that one again, it goes on: the floor went a metre to the left.
        const Image again = stage.scene.render(stage.fromAbove(12.0f), settings);
        checkMotion(again, 32, 50, Vec2(-1.0f / 8.0f, 0.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a figure moves where its pose moves it, and not where it holds still") {
        SceneFixture scene{UVec2(kSize)};
        REQUIRE(exportGltf(test::makeSkinnedBar(), scene.file("bar.gltf")).has_value());
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        const Entity bar = scene.world.createEntity("Bar");
        MeshRendererComponent& renderer = scene.world.add<MeshRendererComponent>(bar);
        renderer.mesh = scene.asset("bar.gltf#mesh/0");
        renderer.materials = {scene.material("red")};
        scene.world.add<AnimatorComponent>(bar) = {.skeleton = scene.asset("bar.gltf#skeleton/0"),
                                                   .clip = scene.asset("bar.gltf#animation/0"),
                                                   .time = 0.5f,
                                                   .loop = false,
                                                   .playing = false};
        AnimationSystem animation;
        // From the front, four metres from top to bottom: sixteen pixels to the metre.
        const CameraView front = scene.orthographicView(Vec3(0.0f, 1.0f, 5.0f), Vec3(0.0f, 1.0f, 0.0f), 4.0f);
        const auto render = [&] {
            animation.evaluate(scene.world, *scene.assets);
            return scene.render(front, motionSettings());
        };
        static_cast<void>(render());
        // Held where it is: nothing moves, to the bit - the lower half, half a metre up and
        // a tenth of a metre up, and beside it.
        const Image held = render();
        for (const UVec2 pixel : {UVec2(32, 40), UVec2(32, 46), UVec2(50, 40)}) {
            CAPTURE(pixel);
            CHECK(held.pixel(pixel.x, pixel.y) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        // Bent on, from an eighth of a turn to a quarter: the upper half swings from up
        // and to the left of the middle joint down to its left. The lower half follows
        // the root and the middle joint by how near each is, and near the foot it moves a
        // little; beside the bar nothing moves.
        scene.world.tryGet<AnimatorComponent>(bar)->time = 1.0f;
        const Image bent = render();
        checkMotion(bent, 50, 40, Vec2(0.0f));
        const auto brightness = [&bent](uint32_t x, uint32_t y) {
            return std::max({bent.pixel(x, y).r, bent.pixel(x, y).g, bent.pixel(x, y).b});
        };
        CHECK(brightness(32, 46) > 0.0f);
        CHECK(brightness(32, 46) < brightness(18, 32) * 0.5f);
        // The upper half turns about the middle joint, a metre up: what a pixel of it
        // shows now was an eighth of a turn back, clockwise, about the joint.
        const auto uvOf = [](const Vec2& point) { return Vec2(0.5f + point.x / 4.0f, 0.5f - (point.y - 1.0f) / 4.0f); };
        for (const UVec2 pixel : {UVec2(18, 32), UVec2(24, 32)}) {
            CAPTURE(pixel);
            const Vec2 uv = (Vec2(pixel) + 0.5f) / static_cast<float>(kSize);
            const Vec2 now((uv.x - 0.5f) * 4.0f, 1.0f - (uv.y - 0.5f) * 4.0f);
            const Vec2 fromJoint = now - Vec2(0.0f, 1.0f);
            const float back = glm::radians(-45.0f);
            const Vec2 before = Vec2(0.0f, 1.0f) + Vec2(fromJoint.x * std::cos(back) - fromJoint.y * std::sin(back),
                                                        fromJoint.x * std::sin(back) + fromJoint.y * std::cos(back));
            checkMotion(bent, pixel.x, pixel.y, uvOf(now) - uvOf(before));
        }
        checkNoValidationIssues(*scene.context.graphics);
    }
}
