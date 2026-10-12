#include "cordite/engine/Engine.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/math/Color.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::TempDirectory;

namespace {

EngineDesc headlessDesc(UVec2 size = UVec2(320, 180)) {
    EngineDesc desc;
    desc.applicationName = "Cordite engine tests";
    desc.headless = true;
    desc.validation = true;
    desc.renderSize = size;
    desc.fixedTimeStep = 1.0 / 60.0;
    // Tests must not depend on where the engine would look for shaders by default.
    desc.shaderDirectory = test::shaderDirectory();
    return desc;
}

std::unique_ptr<Engine> createEngine(const EngineDesc& desc) {
    Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
    REQUIRE_MESSAGE(engine.has_value(), test::errorOf(engine));
    return std::move(*engine);
}

void runFrames(Engine& engine, int count) {
    for (int frame = 0; frame < count; ++frame) {
        const Result<bool> keepRunning = engine.runFrame();
        REQUIRE_MESSAGE(keepRunning.has_value(), test::errorOf(keepRunning));
        REQUIRE(*keepRunning);
    }
}

} // namespace

TEST_SUITE("Engine") {
    TEST_CASE("a headless engine renders frames with a fixed time step") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        CHECK(engine->window() == nullptr);
        CHECK(engine->frameIndex() == 0);
        CHECK(engine->renderSize() == UVec2(320, 180));

        runFrames(*engine, 30);
        CHECK(engine->frameIndex() == 30);
        CHECK(engine->timeSeconds() == doctest::Approx(0.5));

        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("captureFrame returns the last rendered frame") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(200, 100)));

        const Result<Image> tooEarly = engine->captureFrame();
        REQUIRE_FALSE(tooEarly.has_value());
        CHECK(tooEarly.error().code == ErrorCode::InvalidState);

        runFrames(*engine, 1);
        const Result<Image> frame = engine->captureFrame();
        REQUIRE_MESSAGE(frame.has_value(), test::errorOf(frame));
        CHECK(frame->width() == 200);
        CHECK(frame->height() == 100);
        CHECK(frame->format() == ImageFormat::Rgba8);
        // The first frame is rendered at time zero: the test pattern's highlight bar sits
        // on the left edge, and alpha is opaque everywhere.
        CHECK(frame->pixel(0, 50).b > 0.5f);
        CHECK(frame->pixel(100, 50).a == 1.0f);

        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("two engines given the same inputs render identical frames") {
        // One engine at a time: each owns the process-wide window system.
        const auto renderTwelveFrames = [] {
            const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
            runFrames(*engine, 12);
            Result<Image> frame = engine->captureFrame();
            REQUIRE_MESSAGE(frame.has_value(), test::errorOf(frame));
            return std::move(*frame);
        };
        const Image firstFrame = renderTwelveFrames();
        const Image secondFrame = renderTwelveFrames();

        const ImageComparison comparison =
            test::unwrap(compareImages(firstFrame, secondFrame, {.channelTolerance = 0}));
        CHECK(comparison.matches);
        CHECK(comparison.maxChannelDifference == 0);
    }

    TEST_CASE("frames differ as time advances") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 1);
        const Image early = test::unwrap(engine->captureFrame());
        runFrames(*engine, 60);
        const Image late = test::unwrap(engine->captureFrame());

        const ImageComparison comparison = test::unwrap(compareImages(early, late, {.channelTolerance = 8}));
        CHECK_FALSE(comparison.matches);
        CHECK(comparison.differingPixels > 100);
    }

    TEST_CASE("requestExit ends the frame loop") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 2);

        engine->requestExit();
        const Result<bool> keepRunning = engine->runFrame();
        REQUIRE(keepRunning.has_value());
        CHECK_FALSE(*keepRunning);
        CHECK(engine->frameIndex() == 2);
    }

    TEST_CASE("the shader cache directory is used across engine instances") {
        TempDirectory cache;
        EngineDesc desc = headlessDesc();
        desc.cacheDirectory = cache.path();

        {
            const std::unique_ptr<Engine> engine = createEngine(desc);
            runFrames(*engine, 1);
            // The fullscreen triangle, the test pattern, the blit and the two interface
            // shaders; of the scene renderer the mesh vertex shader in its two forms - for
            // meshes as they are and for skinned ones - the pixel shaders of its five mesh
            // passes, the lighting in its three forms, the debug view, the display
            // encoding, what makes the cube map of an environment, the two of the ambient
            // occlusion, and the six that finish the picture: three for the bloom, one
            // for the exposure, the last pass and the anti-aliasing.
            CHECK(engine->shaders().stats().compilations == 26);
            CHECK(engine->shaders().stats().diskHits == 0);
        }

        const std::unique_ptr<Engine> engine = createEngine(desc);
        runFrames(*engine, 1);
        CHECK(engine->shaders().stats().compilations == 0);
        CHECK(engine->shaders().stats().diskHits == 26);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("a program can drive the frame step by step") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        CHECK_FALSE(engine->isFrameOpen());
        CHECK(engine->frameTarget().size() == UVec2(320, 180));

        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK(engine->isFrameOpen());
        CHECK(engine->deltaSeconds() == doctest::Approx(1.0f / 60.0f));
        CHECK(engine->commandList() != nullptr);
        // Counters advance when the frame ends, not when it begins.
        CHECK(engine->frameIndex() == 0);
        CHECK(engine->timeSeconds() == 0.0);

        engine->drawTestPattern();
        REQUIRE(engine->endFrame().has_value());
        CHECK_FALSE(engine->isFrameOpen());
        CHECK(engine->frameIndex() == 1);
        CHECK(engine->timeSeconds() == doctest::Approx(1.0 / 60.0));

        // The frame holds the test pattern: at time zero its bar is on the left edge.
        CHECK(test::unwrap(engine->captureFrame()).pixel(0, 90).b > 0.5f);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("a frame nobody draws into is cleared") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(64, 64)));
        runFrames(*engine, 1);

        REQUIRE(test::unwrap(engine->beginFrame()));
        REQUIRE(engine->endFrame().has_value());

        // Nothing of the test pattern drawn by the frame before is left.
        const Image frame = test::unwrap(engine->captureFrame());
        for (const UVec2 pixel : {UVec2(0, 0), UVec2(32, 32), UVec2(63, 63)}) {
            const Vec4 color = frame.pixel(pixel.x, pixel.y);
            CHECK(color.r == 0.0f);
            CHECK(color.g == 0.0f);
            CHECK(color.b == 0.0f);
            CHECK(color.a == 1.0f);
        }
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the interface is drawn on top of the frame") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());

        for (int frame = 0; frame < 3; ++frame) {
            REQUIRE(test::unwrap(engine->beginFrame()));
            engine->drawTestPattern();
            ImGui::SetNextWindowPos(ImVec2(100.0f, 40.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(120.0f, 100.0f), ImGuiCond_Always);
            ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
            ImGui::End();
            REQUIRE(engine->endFrame().has_value());
        }
        const Image withInterface = test::unwrap(engine->captureFrame());

        // Inside the window its dark background covers the pattern; outside, the pattern shows.
        const Vec4 inside = withInterface.pixel(160, 90);
        CHECK(inside.r < 0.25f);
        CHECK(inside.g < 0.25f);
        CHECK(inside.b < 0.25f);
        const Vec4 outside = withInterface.pixel(20, 150);
        CHECK(outside.r + outside.g + outside.b > inside.r + inside.g + inside.b);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("input injected between frames is seen by the next frame and by the interface") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        Input& input = engine->input();

        int clicks = 0;
        const auto frame = [&] {
            REQUIRE(test::unwrap(engine->beginFrame()));
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f), ImGuiCond_Always);
            ImGui::Begin("Clickable", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
            ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));
            clicks += ImGui::Button("Go", ImVec2(100.0f, 40.0f)) ? 1 : 0;
            ImGui::End();
            REQUIRE(engine->endFrame().has_value());
        };

        input.injectKey(Key::Space, true);
        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK(input.wasKeyPressed(Key::Space));
        CHECK(input.isKeyDown(Key::Space));
        REQUIRE(engine->endFrame().has_value());

        // The press belonged to that frame only; the key itself is still held.
        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK_FALSE(input.wasKeyPressed(Key::Space));
        CHECK(input.isKeyDown(Key::Space));
        REQUIRE(engine->endFrame().has_value());

        input.injectMouseMove(Vec2(60.0f, 30.0f));
        frame();
        frame();
        input.injectMouseButton(MouseButton::Left, true);
        frame();
        CHECK(clicks == 0);
        input.injectMouseButton(MouseButton::Left, false);
        frame();
        CHECK(clicks == 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine owns a world with the built-in components registered") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        CHECK(engine->components().find("Transform") != nullptr);
        CHECK(engine->components().find("Camera") != nullptr);
        CHECK(engine->world().entityCount() == 0);

        const Entity parent = engine->world().createEntity("Parent");
        const Entity child = engine->world().createEntity("Child");
        REQUIRE(engine->world().setParent(child, parent).has_value());
        engine->world().transform(parent).position = Vec3(1.0f, 2.0f, 3.0f);

        // runFrame() brings the world transforms up to date.
        runFrames(*engine, 1);
        CHECK(engine->world().isValid(child));
        CHECK(Vec3(engine->world().worldMatrix(child)[3]) == Vec3(1.0f, 2.0f, 3.0f));
    }

    TEST_CASE("the engine has the built-in assets, and those of an asset directory when given one") {
        {
            const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
            CHECK(engine->assets().database().find(builtinMeshId(BuiltinMesh::Cube)) != nullptr);
            CHECK(engine->assets().database().rootDirectory().empty());
            CHECK(engine->components().find("MeshRenderer") != nullptr);
        }

        EngineDesc desc = headlessDesc();
        desc.assetDirectory = test::sourceDirectory() / "samples" / "showcase" / "assets";
        const std::unique_ptr<Engine> engine = createEngine(desc);
        CHECK(engine->assets().database().findByReference("models/crate.gltf") != nullptr);
        CHECK(engine->assets().database().problems().empty());
    }

    TEST_CASE("an asset directory that does not exist is an error") {
        EngineDesc desc = headlessDesc();
        desc.assetDirectory = test::sourceDirectory() / "no-such-directory";
        const Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
        REQUIRE_FALSE(engine.has_value());
        CHECK(engine.error().code == ErrorCode::NotFound);
    }

    TEST_CASE("the world is drawn through its primary camera, and the test pattern without one") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(160, 90)));
        World& world = engine->world();

        // Nothing to look through: the test pattern, which is never a single color.
        runFrames(*engine, 1);
        const Image pattern = test::unwrap(engine->captureFrame());
        CHECK(pattern.pixel(10, 10) != pattern.pixel(150, 80));

        // A camera looking at a cube that fills the middle of its view.
        const Entity camera = world.createEntity("Camera");
        world.transform(camera).position = Vec3(0.0f, 0.0f, 3.0f);
        world.add<CameraComponent>(camera);
        const Entity cube = world.createEntity("Cube");
        world.add<MeshRendererComponent>(cube).mesh = builtinMeshId(BuiltinMesh::Cube);
        // A game shows what its scene says: without a light, the cube would be black.
        const Entity lamp = world.createEntity("Lamp");
        world.transform(lamp).position = Vec3(0.0f, 0.0f, 3.0f);
        world.add<LightComponent>(lamp) = {.type = LightType::Point, .intensity = 9.0f, .range = 10.0f};

        runFrames(*engine, 1);
        const Image frame = test::unwrap(engine->captureFrame());
        // The background in the corners, the lit default material in the middle.
        // (The same in opposite corners, but for the dithering of a finished picture.)
        const Vec4 corner = frame.pixel(2, 2);
        const Vec4 middle = frame.pixel(80, 45);
        CHECK_NEAR(frame.pixel(157, 87), corner, 2.5f / 255.0f);
        CHECK(corner.r < 0.2f);
        CHECK(middle.r > corner.r + 0.3f);
        CHECK(engine->sceneRenderer().statistics().meshRenderers == 1);
        CHECK(engine->sceneRenderer().statistics().basePass.triangles == 12);

        // A camera that is switched off is no camera.
        world.setEnabled(camera, false);
        runFrames(*engine, 1);
        CHECK(test::unwrap(engine->captureFrame()).pixel(10, 10) == pattern.pixel(10, 10));
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine draws the world with render settings a game can change") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(160, 90)));
        World& world = engine->world();
        const Entity camera = world.createEntity("Camera");
        world.transform(camera).position = Vec3(0.0f, 1.0f, 6.0f);
        world.add<CameraComponent>(camera);
        const Entity floor = world.createEntity("Floor");
        world.transform(floor).scale = Vec3(20.0f, 1.0f, 20.0f);
        world.add<MeshRendererComponent>(floor).mesh = builtinMeshId(BuiltinMesh::Plane);
        const Entity sun = world.createEntity("Sun");
        world.transform(sun).rotation = lookRotation(Vec3(-0.3f, -1.0f, -0.2f));
        world.add<LightComponent>(sun) = {.type = LightType::Directional, .castsShadows = true};

        // As it comes: shadows of medium quality, one map for each cascade of the sun,
        // and a picture that is finished at medium quality.
        CHECK(engine->renderSettings().shadowQuality == ShadowQuality::Medium);
        CHECK(engine->renderSettings().postProcessQuality == PostProcessQuality::Medium);
        CHECK(renderQualityOf(engine->renderSettings()) == RenderQuality::Medium);
        runFrames(*engine, 1);
        CHECK(engine->statistics().scene.shadowMaps == 4);
        CHECK(engine->statistics().scene.shadowPass.drawCalls == 4);
        const Vec4 lit = test::unwrap(engine->captureFrame()).pixel(80, 80);

        // A player who switches shadows off gets none from the next frame on.
        engine->renderSettings().shadowQuality = ShadowQuality::Off;
        runFrames(*engine, 1);
        CHECK(engine->statistics().scene.shadowMaps == 0);
        CHECK(engine->statistics().scene.shadowPass.drawCalls == 0);
        // The floor is lit as before: nothing stands in the sun.
        CHECK_NEAR(test::unwrap(engine->captureFrame()).pixel(80, 80), lit, 2.0f / 255.0f);

        // One word sets everything a view may cost.
        applyRenderQuality(RenderQuality::High, engine->renderSettings());
        CHECK(engine->renderSettings().shadowQuality == ShadowQuality::High);
        CHECK(engine->renderSettings().postProcessQuality == PostProcessQuality::High);
        runFrames(*engine, 1);
        CHECK(engine->statistics().scene.shadowMaps == 4);
        CHECK(engine->statistics().scene.shadowAtlasBytes == uint64_t{8192} * 8192 * 4);

        // The background is one of the settings too. A finished picture shows it through
        // the tone mapper: a red that is as bright as white is not at the top of what a
        // screen can show ...
        engine->renderSettings().backgroundColor = Vec3(1.0f, 0.0f, 0.0f);
        runFrames(*engine, 1);
        const Vec4 finished = test::unwrap(engine->captureFrame()).pixel(80, 3);
        CHECK(finished.r > 0.7f);
        CHECK(finished.r < 0.9f);
        CHECK(finished.g < 0.1f);
        // ... and one that is not finished shows it as it is.
        engine->renderSettings().postProcessQuality = PostProcessQuality::Off;
        runFrames(*engine, 1);
        CHECK_NEAR(test::unwrap(engine->captureFrame()).pixel(80, 3), Vec4(1.0f, 0.0f, 0.0f, 1.0f), 2.0f / 255.0f);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine finishes the picture as the world says, for the camera it is seen through, over time") {
        EngineDesc desc = headlessDesc(UVec2(64, 64));
        // A tenth of a second a frame, whatever the clock says.
        desc.fixedTimeStep = 0.1;
        const std::unique_ptr<Engine> engine = createEngine(desc);
        World& world = engine->world();
        engine->renderSettings().backgroundColor = Vec3(0.5f);

        // A camera that looks at nothing but the background, with a look of its own.
        const Entity camera = world.createEntity("Camera");
        world.add<CameraComponent>(camera);
        PostProcessComponent& own = world.add<PostProcessComponent>(camera);
        own.toneMapper = ToneMapper::Linear;
        own.bloomIntensity = 0.0f;
        own.exposure = -1.0f;
        // And one that the scene sets for every view.
        const Entity scene = world.createEntity("Look");
        {
            PostProcessComponent& look = world.add<PostProcessComponent>(scene);
            look.toneMapper = ToneMapper::Linear;
            look.bloomIntensity = 0.0f;
            look.exposure = 1.0f;
            look.saturation = 0.0f;
        }

        const auto brightness = [&] { return srgbToLinear(test::unwrap(engine->captureFrame()).pixel(32, 32).g); };
        // The camera's own settings have the last word: a stop down, not a stop up.
        runFrames(*engine, 1);
        CHECK(brightness() == doctest::Approx(0.25f).epsilon(0.03));
        // Without them, what the scene says.
        world.remove<PostProcessComponent>(camera);
        runFrames(*engine, 1);
        CHECK(brightness() == doctest::Approx(1.0f).epsilon(0.03));

        // Automatic exposure gets used to the picture frame by frame, by the engine's
        // clock: here from the stop it starts at ...
        PostProcessComponent& look = *world.tryGet<PostProcessComponent>(scene);
        look.exposure = 0.0f;
        look.exposureMode = ExposureMode::Automatic;
        look.adaptationSpeedUp = 2.0f;
        look.adaptationSpeedDown = 2.0f;
        look.autoExposureMinimum = -8.0f;
        look.autoExposureMaximum = 8.0f;
        runFrames(*engine, 1);
        CHECK(brightness() == doctest::Approx(0.18f).epsilon(0.03));
        // ... towards four times the light, a tenth of a second at a time.
        engine->renderSettings().backgroundColor = Vec3(2.0f);
        float stops = std::log2(0.18f / 0.5f);
        for (int frame = 0; frame < 3; ++frame) {
            runFrames(*engine, 1);
            stops += (std::log2(0.18f / 2.0f) - stops) * (1.0f - std::exp(-0.1f * 2.0f));
            CHECK(brightness() == doctest::Approx(std::min(2.0f * std::exp2(stops), 1.0f)).epsilon(0.03));
        }
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine reports what a frame cost") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc(UVec2(160, 90)));
        // Before the first frame there is nothing to report.
        CHECK(engine->statistics().cpuMilliseconds == 0.0);
        CHECK_FALSE(engine->statistics().gpuMilliseconds.has_value());
        CHECK(engine->statistics().scene.views == 0);

        // Two cubes in front of a camera: one mesh, drawn twice in one call.
        World& world = engine->world();
        const Entity camera = world.createEntity("Camera");
        world.transform(camera).position = Vec3(0.0f, 0.0f, 5.0f);
        world.add<CameraComponent>(camera);
        for (const float x : {-1.0f, 1.0f}) {
            const Entity cube = world.createEntity("Cube");
            world.transform(cube).position = Vec3(x, 0.0f, 0.0f);
            world.add<MeshRendererComponent>(cube).mesh = builtinMeshId(BuiltinMesh::Cube);
        }

        runFrames(*engine, 1);
        const FrameStatistics first = engine->statistics();
        CHECK(first.frameIndex == 0);
        CHECK(first.cpuMilliseconds > 0.0);
        CHECK(first.scene.views == 1);
        CHECK(first.scene.meshRenderers == 2);
        CHECK(first.scene.culled == 0);
        CHECK(first.scene.basePass == MeshPassStatistics{.drawCalls = 1, .instances = 2, .triangles = 24});
        // Depth first, then the base pass: two draw calls in all.
        CHECK(first.scene.depthPass == first.scene.basePass);
        CHECK(first.scene.meshes.drawCalls == 2);
        CHECK(first.resources.meshCount == 1);
        // 24 vertices of 56 bytes and 36 indices of two.
        CHECK(first.resources.meshBytes == 24 * 56 + 36 * 2);

        // The time the GPU took arrives once it has finished a frame, where it can measure.
        runFrames(*engine, 3);
        engine->graphics().waitIdle();
        runFrames(*engine, 1);
        CHECK(engine->statistics().frameIndex == 4);
        if (engine->graphics().adapter().supportsTimerQueries) {
            REQUIRE(engine->statistics().gpuMilliseconds.has_value());
            CHECK(*engine->statistics().gpuMilliseconds > 0.0);
            CHECK(*engine->statistics().gpuMilliseconds < 1000.0);
        } else {
            CHECK_FALSE(engine->statistics().gpuMilliseconds.has_value());
        }

        // A frame that draws no world has nothing in its scene numbers; what is on the
        // GPU stays there.
        world.setEnabled(camera, false);
        runFrames(*engine, 1);
        CHECK(engine->statistics().scene.views == 0);
        CHECK(engine->statistics().scene.meshes.drawCalls == 0);
        CHECK(engine->statistics().resources.meshCount == 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("drawWorld belongs between beginFrame and endFrame") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)engine->drawWorld(), test::AssertionFailure);
#else
        const Status outside = engine->drawWorld();
        REQUIRE_FALSE(outside.has_value());
        CHECK(outside.error().code == ErrorCode::InvalidState);
#endif

        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK(engine->drawWorld().has_value());
        REQUIRE(engine->endFrame().has_value());
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the showcase as a game shows it matches the reference image") {
        EngineDesc desc = headlessDesc(UVec2(960, 540));
        desc.assetDirectory = test::sourceDirectory() / "samples" / "showcase" / "assets";
        const std::unique_ptr<Engine> engine = createEngine(desc);
        REQUIRE(loadScene(desc.assetDirectory / "scenes" / "showcase.scene.json", engine->world(), engine->components())
                    .has_value());

        runFrames(*engine, 2);
        test::checkAgainstReference(test::unwrap(engine->captureFrame()), "showcase-game-960x540");

        // Everything in the scene that is enabled was found; none of it failed to load.
        const SceneRenderStatistics& statistics = engine->sceneRenderer().statistics();
        CHECK(statistics.meshRenderers == 23);
        // The cable of the hoist is skinned to six joints, and it swings: a game lets
        // time pass, and the animator's clock has moved on by the two frames.
        CHECK(statistics.skinnedMeshes == 1);
        CHECK(statistics.jointMatrices == 6);
        CHECK(engine->statistics().animation == AnimationStatistics{.animators = 1, .joints = 6});
        // Seven bodies: the floor, the pillar, the cone, and what the simulation moves -
        // the crate with its lid, the barrel, the ball and the gas cylinder. A frame of
        // a sixtieth of a second is one step.
        CHECK(engine->statistics().physics.steps == 1);
        CHECK(engine->statistics().physics.bodies == 7);
        // They stand and lie on the floor, and stay there.
        CHECK(engine->world().transform(engine->world().findByName("Crate")).position.y ==
              doctest::Approx(0.5f).epsilon(0.01));
        CHECK(engine->world().transform(engine->world().findByName("Ball")).position.y ==
              doctest::Approx(0.3f).epsilon(0.02));
        const Entity skeleton = engine->world().findByName("Skeleton");
        REQUIRE(engine->world().has<AnimatorComponent>(skeleton));
        CHECK(engine->world().tryGet<AnimatorComponent>(skeleton)->time == doctest::Approx(0.75 + 2.0 / 60.0));
        CHECK(engine->world().tryGet<AnimatorComponent>(skeleton)->blendTime == doctest::Approx(1.5 + 2.0 / 60.0));
        // The four wall panels, the two beams and the two barrels are drawn as groups.
        CHECK(statistics.basePass.drawCalls < statistics.basePass.instances);
        // The glass pane is the one surface that is blended over the rest.
        CHECK(statistics.forwardPass.drawCalls == 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine simulates the bodies of its world while its clock runs") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        // The physics components are among those a scene can hold.
        CHECK(engine->components().find("Collider") != nullptr);
        CHECK(engine->components().find("RigidBody") != nullptr);

        World& world = engine->world();
        const Entity floor = world.createEntity("Floor");
        world.add<ColliderComponent>(floor) = {.size = Vec3(20.0f, 1.0f, 20.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
        const Entity crate = world.createEntity("Crate");
        world.transform(crate).position = Vec3(0.0f, 4.0f, 0.0f);
        world.add<ColliderComponent>(crate);
        world.add<RigidBodyComponent>(crate);

        // runFrame() lets the time of a frame pass: a sixtieth of a second, one step.
        runFrames(*engine, 1);
        CHECK(engine->statistics().physics == PhysicsFrameStatistics{.steps = 1, .bodies = 2, .activeBodies = 1});
        CHECK(world.transform(crate).position.y < 4.0f);
        // The world transforms are up to date with where the body has gone.
        CHECK(world.worldMatrix(crate)[3].y == world.transform(crate).position.y);
        runFrames(*engine, 239);
        CHECK(world.transform(crate).position.y == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(engine->statistics().physics == PhysicsFrameStatistics{.steps = 1, .bodies = 2, .activeBodies = 0});

        // The simulation answers what a ray hits, and whose body that is.
        const std::optional<RayHit> hit =
            engine->physics().simulation().raycast(Ray{Vec3(0.2f, 10.0f, 0.2f), Vec3(0.0f, -1.0f, 0.0f)}, 100.0f);
        REQUIRE(hit.has_value());
        CHECK(engine->physics().entityOf(hit->body) == crate);
        CHECK(hit->position.y == doctest::Approx(1.0f).epsilon(0.02));

        // With the clock stopped nothing is simulated: a crate put in the air stays
        // there, and its body is with it.
        world.transform(crate).position = Vec3(5.0f, 8.0f, 0.0f);
        for (int frame = 0; frame < 10; ++frame) {
            REQUIRE(test::unwrap(engine->beginFrame()));
            engine->updateWorld(WorldClock::Stopped);
            REQUIRE(engine->endFrame().has_value());
        }
        CHECK(world.transform(crate).position == Vec3(5.0f, 8.0f, 0.0f));
        CHECK(engine->statistics().physics.steps == 0);
        CHECK(engine->statistics().physics.bodies == 2);
        CHECK(engine->physics().simulation().position(engine->physics().bodyOf(crate)) == Vec3(5.0f, 8.0f, 0.0f));
        // And falls from there when time passes again.
        runFrames(*engine, 1);
        CHECK(world.transform(crate).position.y < 8.0f);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine simulates as it is told to: gravity, and the length of a step") {
        {
            // No gravity, and steps of a thirtieth of a second: every other frame has one.
            EngineDesc desc = headlessDesc();
            desc.physics = {.world = {.gravity = Vec3(0.0f)}, .stepSeconds = 1.0 / 30.0};
            const std::unique_ptr<Engine> engine = createEngine(desc);
            CHECK(engine->physics().simulation().gravity() == Vec3(0.0f));
            World& world = engine->world();
            const Entity crate = world.createEntity("Crate");
            world.transform(crate).position = Vec3(0.0f, 4.0f, 0.0f);
            world.add<ColliderComponent>(crate);
            world.add<RigidBodyComponent>(crate) = {.linearDamping = 0.0f, .linearVelocity = Vec3(3.0f, 0.0f, 0.0f)};
            uint32_t steps = 0;
            for (int frame = 0; frame < 60; ++frame) {
                runFrames(*engine, 1);
                steps += engine->statistics().physics.steps;
            }
            CHECK(steps == 30);
            // A second at three metres a second, and nothing pulls it down.
            CHECK(world.transform(crate).position.x == doctest::Approx(3.0f).epsilon(1.0e-3));
            CHECK(world.transform(crate).position.y == 4.0f);
        }
        // Steps of no length are refused, with everything else left as it was.
        EngineDesc desc = headlessDesc();
        desc.physics.stepSeconds = 0.0;
        const Result<std::unique_ptr<Engine>> refused = Engine::create(desc);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ErrorCode::InvalidArgument);
        // And an engine can be made right after.
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        CHECK(engine->physics().simulation().gravity() == Vec3(0.0f, -9.81f, 0.0f));
    }

    TEST_CASE("the engine plays the animators of its world while its clock runs") {
        const TempDirectory directory;
        REQUIRE(exportGltf(test::makeSkinnedBar(), directory.path() / "bar.gltf").has_value());
        EngineDesc desc = headlessDesc();
        desc.assetDirectory = directory.path();
        const std::unique_ptr<Engine> engine = createEngine(desc);
        // The animator is one of the components a scene can hold.
        CHECK(engine->components().find("Animator") != nullptr);

        const AssetDatabase& database = engine->assets().database();
        const AssetInfo* skeleton = database.findByReference("bar.gltf#skeleton/0");
        const AssetInfo* bend = database.findByReference("bar.gltf#animation/0");
        REQUIRE(skeleton != nullptr);
        REQUIRE(bend != nullptr);
        World& world = engine->world();
        const Entity figure = world.createEntity("Figure");
        world.add<AnimatorComponent>(figure) = {.skeleton = skeleton->id, .clip = bend->id, .loop = false};
        const auto time = [&] { return world.tryGet<AnimatorComponent>(figure)->time; };

        // runFrame() lets the time of a frame pass: a sixtieth of a second each.
        runFrames(*engine, 30);
        CHECK(time() == doctest::Approx(0.5));
        const SkeletonPoseComponent* pose = world.tryGet<SkeletonPoseComponent>(figure);
        REQUIRE(pose != nullptr);
        // Half way through "Bend" the tip has gone an eighth of a turn round the middle.
        const float along = std::sqrt(0.5f);
        CHECK_NEAR(Vec3(pose->joints[2][3]), Vec3(-along, 1.0f + along, 0.0f), 1.0e-4f);
        CHECK(engine->statistics().animation == AnimationStatistics{.animators = 1, .joints = 3});

        // A program that drives its frames itself says whether time passes for the world.
        REQUIRE(test::unwrap(engine->beginFrame()));
        engine->updateWorld(WorldClock::Stopped);
        CHECK(time() == doctest::Approx(0.5));
        engine->updateWorld();
        CHECK(time() == doctest::Approx(0.5 + 1.0 / 60.0));
        // It also brings the world transforms up to date, either way.
        world.transform(figure).position = Vec3(4.0f, 5.0f, 6.0f);
        engine->updateWorld(WorldClock::Stopped);
        CHECK(Vec3(world.worldMatrix(figure)[3]) == Vec3(4.0f, 5.0f, 6.0f));
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->statistics().animation == AnimationStatistics{.animators = 1, .joints = 3});

        // Between frames no time passes. Posing is no mistake there; a running clock is.
        const float before = time();
        engine->updateWorld(WorldClock::Stopped);
        CHECK(time() == before);
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS(engine->updateWorld(WorldClock::Running), test::AssertionFailure);
#else
        engine->updateWorld(WorldClock::Running);
#endif
        CHECK(time() == before);

        // A frame in which the world is not brought up to date has posed nothing.
        REQUIRE(test::unwrap(engine->beginFrame()));
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->statistics().animation == AnimationStatistics());
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("frames must be begun and ended in pairs") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)engine->endFrame(), test::AssertionFailure);
        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK_THROWS_AS((void)engine->beginFrame(), test::AssertionFailure);
#else
        const Status unopened = engine->endFrame();
        REQUIRE_FALSE(unopened.has_value());
        CHECK(unopened.error().code == ErrorCode::InvalidState);
        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK_FALSE(engine->beginFrame().has_value());
#endif
        // The frame that was properly begun can still be finished.
        CHECK(engine->endFrame().has_value());
        CHECK(engine->frameIndex() == 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("an engine destroyed in the middle of a frame shuts down cleanly") {
        {
            const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
            REQUIRE(test::unwrap(engine->beginFrame()));
            engine->drawTestPattern();
            ImGui::Begin("Unfinished");
            ImGui::End();
        }
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("only one engine can exist at a time") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        const Result<std::unique_ptr<Engine>> second = Engine::create(headlessDesc());
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().code == ErrorCode::InvalidState);

        // The failed attempt did not disturb the engine that exists.
        runFrames(*engine, 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("invalid descriptions are rejected") {
        EngineDesc zeroSize = headlessDesc();
        zeroSize.renderSize = UVec2(0, 100);
        const Result<std::unique_ptr<Engine>> first = Engine::create(zeroSize);
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().code == ErrorCode::InvalidArgument);

        EngineDesc negativeStep = headlessDesc();
        negativeStep.fixedTimeStep = -1.0;
        CHECK_FALSE(Engine::create(negativeStep).has_value());

        EngineDesc missingShaders = headlessDesc();
        missingShaders.shaderDirectory = test::sourceDirectory() / "no-such-directory";
        const Result<std::unique_ptr<Engine>> third = Engine::create(missingShaders);
        REQUIRE_FALSE(third.has_value());
        CHECK(third.error().code == ErrorCode::NotFound);
    }

    TEST_CASE("an engine can be created again after a failed creation") {
        EngineDesc broken = headlessDesc();
        broken.shaderDirectory = test::sourceDirectory() / "no-such-directory";
        REQUIRE_FALSE(Engine::create(broken).has_value());

        // The failed attempt must have released the window system and the device.
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        runFrames(*engine, 1);
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("a windowed engine presents and follows window resizes") {
        EngineDesc desc = headlessDesc();
        desc.headless = false;
        desc.window.title = "Cordite engine test";
        desc.window.width = 320;
        desc.window.height = 200;
        desc.window.visible = false;

        const std::unique_ptr<Engine> engine = createEngine(desc);
        REQUIRE(engine->window() != nullptr);
        runFrames(*engine, 5);
        CHECK(engine->renderSize() == engine->window()->framebufferSize());

        engine->window()->setLogicalSize(UVec2(400, 240));
        runFrames(*engine, 3);
        CHECK(engine->renderSize() == engine->window()->framebufferSize());

        const Result<Image> frame = engine->captureFrame();
        REQUIRE(frame.has_value());
        CHECK(frame->width() == engine->renderSize().x);
        CHECK(frame->height() == engine->renderSize().y);

        engine->window()->requestClose();
        const Result<bool> keepRunning = engine->runFrame();
        REQUIRE(keepRunning.has_value());
        CHECK_FALSE(*keepRunning);

        checkNoValidationIssues(engine->graphics());
    }
}
