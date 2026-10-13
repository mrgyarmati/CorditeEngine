#include "cordite/engine/Engine.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/assets/Sound.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/engine/Project.h"
#include "cordite/math/Color.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/platform/InputMap.h"
#include "cordite/platform/KeyCodes.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptSystem.h"
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
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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
        // What a game is made of: the assets of its project, the controls the project
        // names, and the scene it starts with.
        const Project project = test::unwrap(loadProject(test::sourceDirectory() / "samples" / "showcase"));
        EngineDesc desc = headlessDesc(UVec2(960, 540));
        desc.assetDirectory = project.assetDirectory();
        const std::unique_ptr<Engine> engine = createEngine(desc);
        engine->actions().setMap(test::unwrap(loadInputMap(project.inputMapPath())));
        REQUIRE(loadScene(project.startScenePath(), engine->world(), engine->components()).has_value());

        runFrames(*engine, 2);
        test::checkAgainstReference(test::unwrap(engine->captureFrame()), "showcase-game-960x540");

        // The light at the end of the sentry's beat is dark: nothing stands in the
        // trigger there.
        const Entity post = engine->world().findByName("Sentry post");
        REQUIRE(engine->world().has<LightComponent>(post));
        const auto brightness = [&] { return engine->world().tryGet<LightComponent>(post)->intensity; };
        CHECK(brightness() == 0.0f);

        // Everything in the scene that is enabled was found; none of it failed to load.
        const SceneRenderStatistics& statistics = engine->sceneRenderer().statistics();
        CHECK(statistics.meshRenderers == 30);
        // The cable of the hoist is skinned to six joints, and it swings: a game lets
        // time pass, and the animator's clock has moved on by the two frames.
        CHECK(statistics.skinnedMeshes == 1);
        CHECK(statistics.jointMatrices == 6);
        CHECK(engine->statistics().animation == AnimationStatistics{.animators = 1, .joints = 6});
        // Seventeen bodies: the floor, the pillar, the cone, the grating, the triggers
        // over the walkway and at the sentry's post, the beam of the rack, what the
        // simulation moves - the crate with its lid, the barrel, the ball, the gas
        // cylinder and the five things that hang from the rack, each by a joint - and the
        // body that stands for the sentry, which is a character. Two have shapes made of
        // meshes: the hull around the barrel, the triangles of the grating. A frame of a
        // sixtieth of a second is one step.
        CHECK(engine->statistics().physics.steps == 1);
        CHECK(engine->statistics().physics.bodies == 17);
        CHECK(engine->statistics().physics.joints == 5);
        CHECK(engine->statistics().physics.characters == 1);
        CHECK(engine->statistics().physics.geometries == 2);
        CHECK(engine->world().transform(engine->world().findByName("Barrel")).position.y ==
              doctest::Approx(0.45f).epsilon(0.01));
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

        // What hangs from the rack is held there: a second and a half later, in which
        // whatever nothing held would have fallen to the floor, each thing is as high
        // as the scene has it.
        runFrames(*engine, 90);
        const auto heightOf = [&](std::string_view name) {
            return engine->world().computeWorldMatrix(engine->world().findByName(name))[3].y;
        };
        CHECK(heightOf("Bracket") == doctest::Approx(2.1f).epsilon(0.005));
        CHECK(heightOf("Lantern") == doctest::Approx(1.6f).epsilon(0.005));
        CHECK(heightOf("Sign") == doctest::Approx(1.95f).epsilon(0.005));
        CHECK(heightOf("Runner") == doctest::Approx(2.1f).epsilon(0.005));
        CHECK(heightOf("Weight") == doctest::Approx(1.5f).epsilon(0.005));

        // The sentry patrols along the wall. Its script says where its character is to
        // walk - a property of a component, set in every frame before the simulation
        // steps - and the simulation of the same frame walks it: on the floor, turned
        // as it was. Ninety-two frames into the game it has gone as far as 0.8 metres a
        // second take it.
        World& world = engine->world();
        const Entity sentry = world.findByName("Sentry");
        const auto controller = [&] { return world.tryGet<CharacterControllerComponent>(sentry); };
        REQUIRE(controller() != nullptr);
        CHECK(controller()->ground == GroundState::OnGround);
        CHECK(controller()->groundEntity == world.id(world.findByName("Floor")));
        const float walked = 0.8f * 92.0f / 60.0f;
        CHECK_NEAR(world.transform(sentry).position, Vec3(-0.8f - walked, 0.0f, -3.9f), 2.0e-2f);
        CHECK_NEAR(controller()->velocity, Vec3(-0.8f, 0.0f, 0.0f), 1.0e-2f);
        CHECK(world.transform(sentry).rotation == kIdentityQuat);
        // What is seen of it has come along.
        CHECK_NEAR(Vec3(world.computeWorldMatrix(world.findByName("Sentry body"))[3]),
                   Vec3(-0.8f - walked, 0.9f, -3.9f), 2.0e-2f);

        // It has just walked into the trigger of its post, which reaches a little way
        // along the beat: the simulation told the script there, and the script has lit
        // the light.
        CHECK(brightness() == 3.0f);

        // The 1.8 metres to the end of its beat take two seconds and a quarter; three
        // seconds into the game it stands there, resting, and two seconds after that it
        // is on its way back - out of the trigger, and the light is dark again.
        runFrames(*engine, 88);
        CHECK_NEAR(world.transform(sentry).position, Vec3(-2.6f, 0.0f, -3.9f), 6.0e-2f);
        CHECK_NEAR(controller()->velocity, Vec3(0.0f), 1.0e-2f);
        CHECK(brightness() == 3.0f);
        runFrames(*engine, 120);
        CHECK_NEAR(controller()->velocity, Vec3(0.8f, 0.0f, 0.0f), 1.0e-2f);
        CHECK(world.transform(sentry).position.x > -1.5f);
        CHECK(world.transform(sentry).position.x < -0.9f);
        CHECK(brightness() == 0.0f);

        // The seven scripts of the showcase run, and none has a problem: the self-test,
        // which compares what every function of the script API gives with what it should
        // and would have stopped at the first that differs, here with the simulation
        // running beside it; the patrol; the clock, which ticks twice a second and has
        // counted the five seconds; the lamp that flickers; and the three that only hear
        // what they are told - the post, the ball, the way out. Two are updated every
        // frame, and in the frame of a tick the clock's timer is called as well. Two
        // things wait for their time: that timer, and the task that makes the lamp
        // flicker. What the self-test made for a few frames - an entity with a clock of
        // its own, timers, a task - is gone again.
        for (const ScriptProblem& problem : engine->scripts().problems()) {
            FAIL_CHECK(problem.message);
        }
        CHECK(engine->statistics().scripts.scripts == 7);
        CHECK(engine->statistics().scripts.instances == 7);
        CHECK(engine->statistics().scripts.calls >= 2);
        CHECK(engine->statistics().scripts.calls <= 3);
        CHECK(engine->statistics().scripts.timers == 2);
        CHECK(world.findByName("Scratch pad").isNull());
        const Entity selfTest = world.findByName("Self-test");
        REQUIRE_FALSE(selfTest.isNull());
        CHECK(world.children(selfTest).empty());
        // Three hundred sixtieths do not add up to five seconds exactly: the tenth tick
        // is this frame's or the next.
        const Entity clock = world.findById(Uuid::parse("10000000-0000-4000-8000-000000000038").value_or(Uuid()));
        REQUIRE_FALSE(clock.isNull());
        CHECK((world.name(clock) == "Clock (9)" || world.name(clock) == "Clock (10)"));

        // Whoever plays takes the sentry over, with the controls of the project. S is
        // "moveY" pulled all the way back: away from the wall, towards +Z, for as long
        // as it is held - half a second, 0.4 metres.
        const Vec3 taken = world.transform(sentry).position;
        engine->input().injectKey(Key::S, true);
        runFrames(*engine, 30);
        CHECK_NEAR(controller()->velocity, Vec3(0.0f, 0.0f, 0.8f), 1.0e-2f);
        CHECK_NEAR(world.transform(sentry).position, taken + Vec3(0.0f, 0.0f, 0.4f), 3.0e-2f);
        // Space is "jump": its script is told when the action goes down, and tells the
        // character, which leaves the floor at four metres a second - a tenth of a
        // second later it is a third of a metre up, and still going.
        engine->input().injectKey(Key::Space, true);
        runFrames(*engine, 6);
        CHECK(controller()->ground == GroundState::InAir);
        CHECK(world.transform(sentry).position.y > 0.2f);
        CHECK(controller()->velocity.y > 2.0f);
        // Held, the key does not make it jump again: it comes down and stays down.
        runFrames(*engine, 90);
        CHECK(controller()->ground == GroundState::OnGround);
        CHECK(world.transform(sentry).position.y == doctest::Approx(0.0).epsilon(0.01));
        // The left button of the pointer is "fire", which knocks the ball off the floor:
        // six newton seconds on two kilograms are three metres a second, upwards. The
        // simulation tells its script that it has left the floor and, when it comes
        // down, that the two have met again - which the script counts in its name.
        const Entity ball = world.findByName("Ball");
        REQUIRE_FALSE(ball.isNull());
        engine->input().injectMouseButton(MouseButton::Left, true);
        runFrames(*engine, 12);
        CHECK(world.transform(ball).position.y > 0.6f);
        CHECK(world.name(ball) == "Ball");
        runFrames(*engine, 48);
        CHECK(world.name(ball).starts_with("Ball ("));

        // Let go of, it takes up its beat again from where it was walked to: back
        // towards the wall, to the end it was heading for, at its own speed.
        engine->input().releaseAll();
        runFrames(*engine, 30);
        CHECK(glm::length(controller()->velocity) == doctest::Approx(0.8).epsilon(0.02));
        CHECK(controller()->velocity.z < -0.5f);
        for (const ScriptProblem& problem : engine->scripts().problems()) {
            FAIL_CHECK(problem.message);
        }

        // T is "travel": the script of the way out asks for the annex. The scene goes on
        // to the end of that frame, and the next begins in the other - whose two bodies
        // are made by its first step, and which is drawn through a camera of its own.
        engine->input().injectKey(Key::T, true);
        runFrames(*engine, 1);
        engine->input().injectKey(Key::T, false);
        CHECK(world.sceneName() == "Showcase");
        runFrames(*engine, 2);
        CHECK(world.sceneName() == "Annex");
        CHECK(world.findByName("Sentry").isNull());
        CHECK_FALSE(world.findByName("Plinth").isNull());
        CHECK(engine->statistics().scripts.instances == 1);
        CHECK(engine->statistics().scripts.timers == 0);
        CHECK(engine->statistics().physics.bodies == 2);
        CHECK(engine->statistics().physics.characters == 0);
        CHECK(engine->statistics().animation == AnimationStatistics{});
        CHECK(engine->sceneRenderer().statistics().meshRenderers == 2);
        CHECK(engine->sceneRenderer().statistics().views == 1);
        // And back, where everything starts anew: the clock at nothing, the sentry at
        // the beginning of its beat.
        engine->input().injectKey(Key::T, true);
        runFrames(*engine, 1);
        engine->input().injectKey(Key::T, false);
        runFrames(*engine, 2);
        CHECK(world.sceneName() == "Showcase");
        CHECK(world.findByName("Plinth").isNull());
        CHECK_FALSE(world.findByName("Clock").isNull());
        CHECK(engine->statistics().physics.bodies == 17);
        CHECK(engine->sceneRenderer().statistics().meshRenderers == 30);
        const Entity back = world.findByName("Sentry");
        REQUIRE_FALSE(back.isNull());
        CHECK_NEAR(world.transform(back).position, Vec3(-0.8f, 0.0f, -3.9f), 5.0e-2f);
        for (const ScriptProblem& problem : engine->scripts().problems()) {
            FAIL_CHECK(problem.message);
        }
        checkNoValidationIssues(engine->graphics());
    }

    TEST_CASE("the engine works out the controls of the game from its input, once a frame") {
        const std::unique_ptr<Engine> engine = createEngine(headlessDesc());
        // Without a map there are no controls.
        CHECK(engine->actions().map() == InputMap{});
        InputMap map;
        map.actions.push_back({.name = "jump", .bindings = {ActionBinding(Key::Space)}});
        map.axes.push_back(
            {.name = "moveX", .bindings = {AxisBinding{.source = KeyPair{.positive = Key::D, .negative = Key::A}}}});
        engine->actions().setMap(std::move(map));
        runFrames(*engine, 1);
        CHECK_FALSE(engine->actions().isDown("jump"));

        // What is injected between two frames is what the next one sees: from its
        // beginning, and the same all through it.
        engine->input().injectKey(Key::Space, true);
        engine->input().injectKey(Key::D, true);
        CHECK_FALSE(engine->actions().isDown("jump"));
        REQUIRE(test::unwrap(engine->beginFrame()));
        CHECK(engine->actions().isDown("jump"));
        CHECK(engine->actions().wasPressed("jump"));
        CHECK(engine->actions().axis("moveX") == 1.0f);
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->actions().wasPressed("jump"));

        // Held, it is down and no longer pressed; let go of, it is released for a frame.
        runFrames(*engine, 1);
        CHECK(engine->actions().isDown("jump"));
        CHECK_FALSE(engine->actions().wasPressed("jump"));
        engine->input().injectKey(Key::Space, false);
        engine->input().injectKey(Key::A, true);
        runFrames(*engine, 1);
        CHECK(engine->actions().wasReleased("jump"));
        CHECK(engine->actions().axis("moveX") == 0.0f);
        runFrames(*engine, 1);
        CHECK_FALSE(engine->actions().wasReleased("jump"));
        // A key that is tapped between two frames was pressed, and released.
        engine->input().injectKey(Key::Space, true);
        engine->input().injectKey(Key::Space, false);
        runFrames(*engine, 1);
        CHECK(engine->actions().wasPressed("jump"));
        CHECK(engine->actions().wasReleased("jump"));
        CHECK_FALSE(engine->actions().isDown("jump"));
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

        // What the frame brought together is there to be read after it: the crate has
        // long landed, and nothing was said in the last frame.
        CHECK(engine->physics().events().empty());
        const Entity zone = world.createEntity("Zone");
        world.add<ColliderComponent>(zone) = {.size = Vec3(4.0f), .trigger = true};
        runFrames(*engine, 1);
        REQUIRE(engine->physics().events().size() == 1);
        CHECK(engine->physics().events()[0].type == CollisionEventType::TriggerEntered);
        CHECK(engine->physics().events()[0].first == zone);
        CHECK(engine->physics().events()[0].second == crate);
        world.destroyEntity(zone);
        runFrames(*engine, 1);
        REQUIRE(engine->physics().events().size() == 1);
        CHECK(engine->physics().events()[0].type == CollisionEventType::TriggerLeft);

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

    TEST_CASE("the engine plays the sounds of its world while its clock runs") {
        const TempDirectory directory;
        // A second of sound, a quarter as loud as can be.
        const SoundData steady{.sampleRate = 48000, .channels = 1, .samples = std::vector<float>(48000, 0.25f)};
        REQUIRE(writeBinaryFile(directory.path() / "steady.wav", test::unwrap(encodeWav(steady))).has_value());
        REQUIRE(writeTextFile(directory.path() / "empty.scene.json",
                              R"({"format": "cordite.scene", "version": 1, "name": "Empty", "entities": []})")
                    .has_value());
        REQUIRE(writeTextFile(directory.path() / "beep.lua",
                              "return {start = function(self) self.voice = cordite.audio.play('steady.wav') end}")
                    .has_value());
        EngineDesc desc = headlessDesc();
        desc.assetDirectory = directory.path();
        const std::unique_ptr<Engine> engine = createEngine(desc);
        // Sound is one of the things a scene can put on an entity, and so is hearing.
        CHECK(engine->components().find("AudioSource") != nullptr);
        CHECK(engine->components().find("AudioListener") != nullptr);
        // Unless it is asked to, an engine plays nothing on the machine it runs on: it
        // mixes what would be heard, as its frames say time passes.
        CHECK(engine->audio().output() == AudioOutput::Offline);
        CHECK(EngineDesc().audio.output == AudioOutput::Offline);

        const AssetDatabase& database = engine->assets().database();
        const AssetInfo* sound = database.findByReference("steady.wav");
        const AssetInfo* scene = database.findByReference("empty.scene.json");
        REQUIRE(sound != nullptr);
        REQUIRE(scene != nullptr);
        World& world = engine->world();
        const Entity speaker = world.createEntity("Speaker");
        world.add<AudioSourceComponent>(speaker) = {.sound = sound->id, .spatial = false};

        // runFrame() lets the time of a frame pass, and that much is heard: a sixtieth of
        // a second is 800 frames of sound, left and right.
        runFrames(*engine, 3);
        CHECK(engine->statistics().audio.sources == 1);
        CHECK(engine->statistics().audio.voices == 1);
        CHECK(engine->statistics().audio.sounds == 1);
        CHECK(engine->statistics().audio.soundBytes == 48000 * 4);
        REQUIRE(engine->audio().mixed().size() == 1600);
        CHECK(engine->audio().mixed().back() == 0.25f);

        // While time does not pass for the world, nothing is heard and nothing moves on.
        REQUIRE(test::unwrap(engine->beginFrame()));
        engine->updateWorld(WorldClock::Stopped);
        CHECK(engine->audio().mixed().empty());
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->statistics().audio == AudioStatistics());
        CHECK(world.tryGet<AudioSourceComponent>(speaker)->playing);

        // The second of sound is over after sixty frames with a running clock, and not
        // before - the frame that stood still is not one of them.
        runFrames(*engine, 56);
        CHECK(world.tryGet<AudioSourceComponent>(speaker)->playing);
        CHECK(engine->statistics().audio.sources == 1);
        runFrames(*engine, 2);
        CHECK_FALSE(world.tryGet<AudioSourceComponent>(speaker)->playing);
        CHECK(engine->statistics().audio.sources == 0);

        // What has a place is heard from where the frame has put it: here, five metres
        // to the right of whoever listens, which the right ear hears five times as well.
        const Entity ears = world.createEntity("Ears");
        world.add<AudioListenerComponent>(ears);
        world.tryGet<Transform>(speaker)->position = Vec3(5.0f, 0.0f, 0.0f);
        *world.tryGet<AudioSourceComponent>(speaker) = {.sound = sound->id, .loop = true};
        runFrames(*engine, 12);
        CHECK(engine->statistics().audio.listeners == 1);
        const std::span<const float> heard = engine->audio().mixed();
        REQUIRE(heard.size() == 1600);
        CHECK(heard[1599] == doctest::Approx(0.05f).epsilon(0.01));
        CHECK(heard[1598] == doctest::Approx(0.01f).epsilon(0.01));

        // A sound of the program's own, without an entity.
        const AudioVoiceId shot = test::unwrap(engine->audio().play(engine->assets(), {.sound = sound->id}));
        runFrames(*engine, 1);
        CHECK(engine->statistics().audio.voices == 2);
        CHECK(engine->statistics().audio.sources == 1);

        // And one of a script's: the scripts of the engine are given what plays.
        const AssetInfo* beep = database.findByReference("beep.lua");
        REQUIRE(beep != nullptr);
        world.add<ScriptComponent>(world.createEntity("Beeper")).script = beep->id;
        runFrames(*engine, 1);
        CHECK(engine->scripts().problems().empty());
        CHECK(engine->statistics().audio.voices == 3);

        // A scene is ended, like it is changed, between two frames.
        REQUIRE(test::unwrap(engine->beginFrame()));
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)engine->endScene(), test::AssertionFailure);
#endif
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->audio().isPlaying(shot));

        // Another scene: what sounded in the one that goes is over.
        REQUIRE(engine->loadScene(scene->id).has_value());
        CHECK_FALSE(engine->audio().isPlaying(shot));
        runFrames(*engine, 1);
        CHECK(engine->statistics().audio == AudioStatistics());
        CHECK(std::ranges::all_of(engine->audio().mixed(), [](float sample) { return sample == 0.0f; }));

        // And a scene that is only ended: the world stays, and what ran for it does not.
        const Entity again = world.createEntity("Again");
        world.add<AudioSourceComponent>(again) = {.sound = sound->id, .loop = true, .spatial = false};
        world.add<ScriptComponent>(again).script = beep->id;
        runFrames(*engine, 2);
        CHECK(engine->statistics().audio.voices == 2);
        CHECK(engine->statistics().scripts.instances == 1);
        REQUIRE(engine->endScene().has_value());
        CHECK(world.isValid(again));
        CHECK(engine->scripts().problems().empty());
        REQUIRE(test::unwrap(engine->beginFrame()));
        engine->updateWorld(WorldClock::Stopped);
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->statistics().audio == AudioStatistics());
        CHECK(engine->statistics().scripts == ScriptStatistics());
        // Played on, it begins anew: the source from its beginning, the script with its start.
        runFrames(*engine, 1);
        CHECK(engine->statistics().audio.voices == 2);
        CHECK(engine->statistics().scripts.instances == 1);
        CHECK(engine->graphics().validationIssues().empty());
    }

    TEST_CASE("the engine runs the scripts of its world while its clock runs") {
        const TempDirectory directory;
        REQUIRE(
            writeTextFile(directory.path() / "counter.lua",
                          "local Counter = {}\n"
                          "function Counter:start() self.frames = 0 end\n"
                          "function Counter:update(deltaSeconds)\n"
                          "    self.frames = self.frames + 1\n"
                          "    self.entity:setName(string.format('%d frames of %.4f s', self.frames, deltaSeconds))\n"
                          "end\n"
                          "return Counter\n")
                .has_value());
        REQUIRE(
            writeTextFile(directory.path() / "broken.lua", "return {update = function(self) self.missing.x = 1 end}")
                .has_value());
        EngineDesc desc = headlessDesc();
        desc.assetDirectory = directory.path();
        // What a script may use up is the engine's to say.
        desc.scripts.runtime.instructionLimit = 50'000;
        const std::unique_ptr<Engine> engine = createEngine(desc);
        // A script is one of the things a scene can put on an entity.
        CHECK(engine->components().find("Script") != nullptr);

        const AssetDatabase& database = engine->assets().database();
        const AssetInfo* counter = database.findByReference("counter.lua");
        const AssetInfo* broken = database.findByReference("broken.lua");
        REQUIRE(counter != nullptr);
        REQUIRE(broken != nullptr);
        World& world = engine->world();
        const Entity entity = world.createEntity("Counter");
        world.add<ScriptComponent>(entity).script = counter->id;

        // runFrame() lets the time of a frame pass, and the scripts run in it.
        runFrames(*engine, 3);
        CHECK(world.name(entity) == "3 frames of 0.0167 s");
        const ScriptStatistics running = engine->statistics().scripts;
        CHECK(running.scripts == 1);
        CHECK(running.instances == 1);
        CHECK(running.stopped == 0);
        CHECK(running.calls == 1);
        CHECK(running.memoryBytes > 0);
        // What they took of the frame is measured: some time, and far less than a frame.
        CHECK(engine->statistics().scriptMilliseconds > 0.0);
        CHECK(engine->statistics().scriptMilliseconds < engine->statistics().cpuMilliseconds);
        CHECK(engine->scripts().problems().empty());

        // A program that drives its frames itself says whether time passes for the
        // world: while it does not, no script runs - which is what keeps an editor from
        // playing the scene it shows.
        REQUIRE(test::unwrap(engine->beginFrame()));
        engine->updateWorld(WorldClock::Stopped);
        CHECK(world.name(entity) == "3 frames of 0.0167 s");
        REQUIRE(engine->endFrame().has_value());
        CHECK(engine->statistics().scripts == ScriptStatistics());
        CHECK(engine->statistics().scriptMilliseconds == 0.0);
        REQUIRE(test::unwrap(engine->beginFrame()));
        engine->updateWorld();
        CHECK(world.name(entity) == "4 frames of 0.0167 s");
        REQUIRE(engine->endFrame().has_value());

        // A script that is wrong is a problem the engine can be asked about, and goes
        // on being one while everything else goes on.
        const Entity wrong = world.createEntity("Wrong");
        world.add<ScriptComponent>(wrong).script = broken->id;
        {
            const test::LogCapture log(LogLevel::Error);
            runFrames(*engine, 2);
            CHECK(log.count(LogLevel::Error) == 1);
        }
        CHECK(world.name(entity) == "6 frames of 0.0167 s");
        REQUIRE(engine->scripts().problems().size() == 1);
        CHECK(engine->scripts().problems()[0].entity == wrong);
        CHECK(engine->scripts().problems()[0].message.starts_with(
            "'Wrong' (broken.lua): broken.lua:1: attempt to index a nil value (field 'missing')"));
        CHECK(engine->statistics().scripts.stopped == 1);
        CHECK(engine->statistics().scripts.instances == 2);
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
