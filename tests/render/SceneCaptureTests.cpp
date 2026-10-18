#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/CameraShake.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Log.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/world/Components.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::LogCapture;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::SceneFixture;

namespace {

/// The color the capture camera sees: a mid tone in every channel, so that a picture read
/// in the wrong color space shows another color - which pure red would not.
constexpr Vec3 kSeen(0.2f, 0.5f, 0.8f);

/// A cube of kSeen in front of a capture camera that looks along -Z from the origin, drawing
/// into a render target of 64 by 32 pixels; and a screen - a plane that shows the target
/// as its light - in front of the main camera, which looks along -Z from far away.
struct Studio {
    Studio() {
        REQUIRE(writeTextFile(scene.file("monitor.target.json"),
                              R"({"format": "cordite.target", "version": 1, "width": 64, "height": 32})")
                    .has_value());
        scene.writeFlatMaterial("seen", kSeen);
        scene.open();
        target = scene.asset("monitor.target.json");
        scene.writeMaterial("screen", fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, )"
                                                  R"("emissiveColor": [1, 1, 1], "emissiveTexture": "{}")",
                                                  target.toString()));
        REQUIRE(scene.assets->refresh().has_value());

        scene.add(BuiltinMesh::Cube, scene.material("seen"), at(Vec3(0.0f, 0.0f, -3.0f), Vec3(4.0f, 4.0f, 0.5f)));
        capture = scene.world.createEntity("Capture");
        CameraComponent lens;
        lens.primary = false;
        lens.verticalFovDegrees = 40.0f;
        scene.world.add<CameraComponent>(capture) = lens;
        scene.world.add<SceneCaptureComponent>(capture) = {.target = target};

        // The screen, a hundred metres away from all of that, facing +Z.
        scene.add(BuiltinMesh::Plane, scene.material("screen"),
                  at(Vec3(0.0f, 0.0f, 100.0f), Vec3(2.0f, 1.0f, 1.0f),
                     glm::angleAxis(glm::radians(90.0f), Vec3(1.0f, 0.0f, 0.0f))));
        view = scene.orthographicView(Vec3(0.0f, 0.0f, 110.0f), Vec3(0.0f, 0.0f, 100.0f), 1.0f);
    }

    /// Draws the captures, then the main view; returns what the main view shows.
    Image render(const SceneRenderSettings& settings = SceneFixture::flatSettings()) {
        scene.world.updateWorldTransforms();
        recordAndExecute(scene.context, [&](nvrhi::ICommandList* commandList) {
            scene.renderer->renderCaptures(commandList, scene.world, settings);
            scene.renderer->render(commandList, scene.world, view, scene.targets, settings);
        });
        return readToImage(*scene.context.graphics, scene.targets.display());
    }

    /// What the render target holds now.
    Image picture() {
        nvrhi::ITexture* texture = scene.renderer->resources().renderTarget(target);
        REQUIRE(texture != nullptr);
        return readToImage(*scene.context.graphics, texture);
    }

    SceneFixture scene{UVec2(64)};
    Uuid target;
    Entity capture;
    CameraView view;
};

} // namespace

TEST_SUITE("SceneCapture") {
    TEST_CASE("a capture draws what its camera sees into its render target, which a material shows") {
        Studio studio;
        // Before anything is drawn into it, a render target is black.
        CHECK(Vec3(studio.picture().pixel(32, 16)) == Vec3(0.0f));

        const Image shown = studio.render();
        const SceneRenderStatistics frame = studio.scene.renderer->takeFrameStatistics();
        CHECK(frame.captures == 1);
        CHECK(frame.views == 2);

        // As large as the file says; the color the camera sees in the middle, and the
        // background around the cube.
        const Image picture = studio.picture();
        CHECK(picture.width() == 64);
        CHECK(picture.height() == 32);
        checkPixel(picture, 32, 16, kSeen);
        // On the screen, as its light.
        checkPixel(shown, 32, 32, kSeen);
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a capture that sees its own picture sees the one before") {
        Studio studio;
        // First the camera sees the cube.
        static_cast<void>(studio.render());
        checkPixel(studio.picture(), 32, 16, kSeen);
        // Then, close enough for it to fill its view, the screen that shows its picture:
        // the color it drew, which it goes on drawing - read while nothing writes it.
        studio.scene.world.transform(studio.capture).position = Vec3(0.0f, 0.0f, 101.0f);
        for (int frame = 0; frame < 3; ++frame) {
            static_cast<void>(studio.render());
            checkPixel(studio.picture(), 32, 16, kSeen);
        }
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a capture is shaken as its camera is") {
        Studio studio;
        World& world = studio.scene.world;
        // The camera looks at the right edge of the cube: the cube on the left of its
        // picture, the background on the right.
        world.transform(studio.capture).position = Vec3(2.0f, 0.0f, 0.0f);
        const auto edgeOf = [](const Image& picture) {
            uint32_t column = 0;
            while (column < picture.width() &&
                   glm::distance(Vec3(picture.pixel(column, 16)), test::displayed(kSeen)) < 0.1f) {
                ++column;
            }
            return column;
        };
        static_cast<void>(studio.render());
        CHECK(edgeOf(studio.picture()) == 32);

        // Shaken from side to side alone, at a moment when the shake has turned it well away.
        CameraShakeComponent shake{.trauma = 1.0f, .maxOffset = Vec3(0.0f), .maxRotation = Vec3(0.0f, 10.0f, 0.0f)};
        while (std::abs(eulerDegreesFromQuat(cameraShakeOffset(shake).rotation).y) < 5.0f) {
            shake.phase += 0.1f;
            REQUIRE(shake.phase < kCameraShakePeriod);
        }
        world.add<CameraShakeComponent>(studio.capture) = shake;
        world.updateWorldTransforms();
        const CameraView shaken = makeCameraView(world, studio.capture, 2.0f);
        const std::optional<Vec2> edge = worldToScreen(shaken, Vec3(2.0f, 0.0f, -2.75f), Vec2(64.0f, 32.0f));
        REQUIRE(edge.has_value());
        REQUIRE(std::abs(edge->x - 32.0f) > 3.0f);

        static_cast<void>(studio.render());
        // The first column of the background is the one whose middle is past the edge.
        CHECK(edgeOf(studio.picture()) == static_cast<uint32_t>(std::ceil(edge->x - 0.5f)));
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a capture keeps its picture for its interval") {
        Studio studio;
        studio.scene.world.tryGet<SceneCaptureComponent>(studio.capture)->interval = 0.25f;
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.deltaSeconds = 0.1f;
        // The first frame draws, the next two keep it, the third has waited 0.3 s.
        uint32_t drawn = 0;
        for (const uint32_t expected : {1u, 0u, 0u, 1u, 0u, 0u, 1u}) {
            static_cast<void>(studio.render(settings));
            const uint32_t captures = studio.scene.renderer->takeFrameStatistics().captures;
            CHECK(captures == expected);
            drawn += captures;
        }
        CHECK(drawn == 3);
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a capture without a camera, or without a target it can draw into, draws nothing") {
        Studio studio;
        LogCapture log;
        studio.scene.world.remove<CameraComponent>(studio.capture);
        static_cast<void>(studio.render());
        static_cast<void>(studio.render());
        CHECK(studio.scene.renderer->takeFrameStatistics().captures == 0);
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("has no Camera"));

        // Nothing to draw into: a target that is none, or names another kind of asset.
        CameraComponent lens;
        lens.primary = false;
        studio.scene.world.add<CameraComponent>(studio.capture) = lens;
        studio.scene.world.tryGet<SceneCaptureComponent>(studio.capture)->target = Uuid();
        static_cast<void>(studio.render());
        CHECK(studio.scene.renderer->takeFrameStatistics().captures == 0);
        studio.scene.world.tryGet<SceneCaptureComponent>(studio.capture)->target = studio.scene.material("seen");
        static_cast<void>(studio.render());
        CHECK(studio.scene.renderer->takeFrameStatistics().captures == 0);
        CHECK(log.contains("cannot use render target"));
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a frame draws a few captures at the most, by the ids of their entities, and counts the rest") {
        Studio studio;
        LogCapture log;
        for (uint32_t index = 0; index < kMaxSceneCaptures + 2; ++index) {
            const Entity extra = studio.scene.world.createEntity("Extra");
            CameraComponent lens;
            lens.primary = false;
            studio.scene.world.add<CameraComponent>(extra) = lens;
            studio.scene.world.add<SceneCaptureComponent>(extra) = {.target = studio.target};
        }
        static_cast<void>(studio.render());
        const SceneRenderStatistics frame = studio.scene.renderer->takeFrameStatistics();
        CHECK(frame.captures == kMaxSceneCaptures);
        CHECK(frame.droppedCaptures == 3);
        CHECK(log.count(LogLevel::Warn) == 1);
        static_cast<void>(studio.render());
        CHECK(log.count(LogLevel::Warn) == 1);
        checkNoValidationIssues(*studio.scene.context.graphics);
    }

    TEST_CASE("a render target is a picture: in a slot of numbers it is reported once and stands back") {
        Studio studio;
        LogCapture log;
        studio.scene.writeMaterial("bumpy", fmt::format(R"("normalTexture": "{}")", studio.target.toString()));
        REQUIRE(studio.scene.assets->refresh().has_value());
        studio.scene.add(BuiltinMesh::Cube, studio.scene.material("bumpy"), at(Vec3(5.0f, 0.0f, 100.0f)));
        static_cast<void>(studio.render());
        static_cast<void>(studio.render());
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("as numbers"));
        checkNoValidationIssues(*studio.scene.context.graphics);
    }
}
