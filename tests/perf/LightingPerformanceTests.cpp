/// @file
/// What hundreds of lights cost, and what dozens of shadow maps cost: measured, written
/// down, and held against budgets.
///
/// The scene is made here, the same on every run: a floor with a field of crates and
/// balls on it, a sun, and six hundred point and spot lights spread over it at random,
/// seen by a camera that looks across the field. That is far more lights than a scene
/// is likely to have in view, and what the clustered lighting is for: a pixel asks the
/// few lights near it, not all of them.
///
/// Two scenarios are run on it:
///
/// - "lights": no light casts a shadow. Besides the frame times it measures what the
///   CPU does for the lights every frame and view - gathering them from the world,
///   sorting them into the clusters and planning their shadow maps - and counts the
///   entries of the cluster lists, which says how well the lights are told apart: a
///   change that puts lights into lists they do not belong in shows there first.
/// - "shadows": the sun and a dozen of the lights cast shadows, most of them point
///   lights with six shadow maps each - more than fifty maps, each of which draws the
///   part of the field its light reaches once more.
///
/// How the numbers are reported and checked is described in PerformanceTestSupport.h.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/engine/Engine.h"
#include "cordite/math/Random.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/ShadowPlan.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "perf/PerformanceTestSupport.h"
#include "render/RenderTestSupport.h"

#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkBudget;
using cordite::test::FrameTimes;
using cordite::test::millisecondsFor;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Frames run before measuring, in which everything is loaded and the buffers grow.
constexpr uint32_t kWarmUpFrames = 30;
/// Frames measured.
constexpr uint32_t kSteadyFrames = 240;
/// How often the work for the lights is measured on its own.
constexpr uint32_t kLightingRounds = 200;

constexpr uint32_t kLightCount = 600;
constexpr int32_t kFieldSize = 20;
constexpr float kFieldSpacing = 3.0f;
/// With shadows, every so-many-th light casts them: twelve of the six hundred, four of
/// them spot lights.
constexpr uint32_t kShadowLightEvery = 50;

/// Fills the world with the scene described at the top of the file.
void buildScene(World& world, bool withShadows) {
    const Entity floor = world.createEntity("Floor");
    world.transform(floor).scale = Vec3(90.0f, 1.0f, 90.0f);
    world.add<MeshRendererComponent>(floor).mesh = builtinMeshId(BuiltinMesh::Plane);

    // Crates and balls in turn, with the default material: two instanced draw calls a pass.
    for (int32_t row = 0; row < kFieldSize; ++row) {
        for (int32_t column = 0; column < kFieldSize; ++column) {
            const Entity entity = world.createEntity("Thing");
            const float offset = 0.5f * static_cast<float>(kFieldSize - 1);
            world.transform(entity).position = Vec3((static_cast<float>(column) - offset) * kFieldSpacing, 0.5f,
                                                    (static_cast<float>(row) - offset) * kFieldSpacing);
            world.add<MeshRendererComponent>(entity).mesh =
                builtinMeshId((row + column) % 2 == 0 ? BuiltinMesh::Cube : BuiltinMesh::Sphere);
        }
    }

    // Seeded: the same lights on every run and every machine.
    Random random(600613);
    const float extent = 0.5f * static_cast<float>(kFieldSize) * kFieldSpacing;
    for (uint32_t index = 0; index < kLightCount; ++index) {
        const Entity entity = world.createEntity("Light");
        world.transform(entity).position =
            Vec3(random.range(-extent, extent), random.range(1.0f, 3.0f), random.range(-extent, extent));
        world.transform(entity).rotation =
            lookRotation(Vec3(random.range(-0.5f, 0.5f), -1.0f, random.range(-0.5f, 0.5f)));
        LightComponent& light = world.add<LightComponent>(entity);
        light.type = index % 3 == 0 ? LightType::Spot : LightType::Point;
        light.color = Vec3(random.range(0.2f, 1.0f), random.range(0.2f, 1.0f), random.range(0.2f, 1.0f));
        light.intensity = random.range(0.5f, 3.0f);
        light.range = random.range(3.0f, 8.0f);
        light.innerConeDegrees = random.range(15.0f, 30.0f);
        light.outerConeDegrees = light.innerConeDegrees + random.range(5.0f, 20.0f);
        light.castsShadows = withShadows && index % kShadowLightEvery == 0;
    }

    const Entity sun = world.createEntity("Sun");
    world.transform(sun).rotation = lookRotation(Vec3(-0.4f, -0.8f, -0.4f));
    world.add<LightComponent>(sun) = {.type = LightType::Directional, .intensity = 0.3f, .castsShadows = withShadows};
    const Entity sky = world.createEntity("Sky");
    world.add<AmbientLightComponent>(sky);

    // Looking across the field from one of its edges, a little above it.
    const Entity camera = world.createEntity("Camera");
    world.transform(camera).position = Vec3(0.0f, 5.0f, extent + 6.0f);
    world.transform(camera).rotation = lookRotation(Vec3(0.0f, -0.2f, -1.0f));
    CameraComponent& lens = world.add<CameraComponent>(camera);
    lens.farPlane = 200.0f;
    lens.primary = true;
}

/// Runs the scene as the scenario `name`, with or without shadows: measures it, writes
/// the report and checks it against the scenario's budgets.
void runScenario(const std::string& name, bool withShadows) {
    // Nothing but built-in meshes and the default material: no assets to load.
    const TempDirectory assets;
    REQUIRE(ensureDirectory(assets.path()).has_value());
    const TempDirectory cache;
    const EngineDesc desc = test::measuredEngineDesc("Lights", assets.path(), cache.path());
    std::unique_ptr<Engine> engine = test::startEngine(desc);
    const std::string adapter = engine->graphics().adapter().name;

    World& world = engine->world();
    buildScene(world, withShadows);
    for (uint32_t frame = 0; frame < kWarmUpFrames; ++frame) {
        REQUIRE(unwrap(engine->runFrame()));
    }

    // --- Frames like any other ---------------------------------------------------------------
    Json milliseconds = Json::object();
    const FrameTimes times = test::measureFrames(*engine, kSteadyFrames);
    test::describeFrameTimes(milliseconds, times);
    const FrameStatistics statistics = engine->statistics();

    // --- The work for the lights on its own, as the renderer does it for every view --------
    const Entity camera = findPrimaryCamera(world);
    REQUIRE_FALSE(camera.isNull());
    const CameraView view =
        makeCameraView(world.worldMatrix(camera), *world.tryGet<CameraComponent>(camera),
                       static_cast<float>(desc.renderSize.x) / static_cast<float>(desc.renderSize.y));
    SceneLights lights;
    LightClusterBuilder builder;
    LightClusters clusters;
    ShadowPlan shadowPlan;
    const ShadowSettings shadowSettings = shadowSettingsFor(engine->renderSettings().shadowQuality);
    std::vector<double> collectSamples;
    std::vector<double> clusterSamples;
    std::vector<double> planSamples;
    for (uint32_t round = 0; round < kLightingRounds; ++round) {
        collectSamples.push_back(millisecondsFor([&] { lights.collect(world); }));
        clusterSamples.push_back(millisecondsFor([&] { builder.build(view, lights.lights(), clusters); }));
        planSamples.push_back(
            millisecondsFor([&] { planShadows(view, lights.lights(), clusters, shadowSettings, shadowPlan); }));
    }
    const test::Distribution collectTime = test::distributionOf(collectSamples);
    const test::Distribution clusterTime = test::distributionOf(clusterSamples);
    const test::Distribution planTime = test::distributionOf(planSamples);
    milliseconds["lightCollect"] = toJson(collectTime);
    milliseconds["lightClusters"] = toJson(clusterTime);
    milliseconds["shadowPlan"] = toJson(planTime);

    const std::vector<std::string> deviceIssues = engine->graphics().validationIssues();
    engine.reset();

    // --- The report -----------------------------------------------------------------------------
    Json report = test::startReport(name, adapter, desc.renderSize, kSteadyFrames);
    report["milliseconds"] = milliseconds;
    report["scene"] = test::describeScene(statistics.scene);
    report["resources"] = test::describeResources(statistics.resources);
    test::writeReport(report);

    MESSAGE(name << " on " << adapter << ": CPU " << times.cpu.median << " ms, GPU " << times.gpu.median << " ms; "
                 << statistics.scene.lights << " lights in " << statistics.scene.lightListEntries
                 << " list entries, collected in " << collectTime.median << " ms and sorted into clusters in "
                 << clusterTime.median << " ms; " << statistics.scene.shadowMaps << " shadow maps, planned in "
                 << planTime.median << " ms, with " << statistics.scene.shadowPass.drawCalls << " draw calls");

    // --- The budgets ----------------------------------------------------------------------------
    const Json budgets = test::loadBudgets(name);

    for (const std::string& issue : deviceIssues) {
        FAIL_CHECK("the graphics device reported: " << issue);
    }

    // The scene is what it is meant to be: the whole field in two draw calls a pass
    // and the floor in one, and every light either in view or out of it, none left out.
    CHECK(statistics.scene.views == 1);
    CHECK(statistics.scene.meshRenderers == kFieldSize * kFieldSize + 1);
    CHECK(statistics.scene.basePass.drawCalls == 3);
    CHECK(statistics.scene.droppedLights == 0);
    CHECK(statistics.scene.lights > kLightCount / 2);
    CHECK(statistics.scene.lights <= kLightCount + 1);
    // What was measured on its own is what the renderer did.
    CHECK(clusters.lights.size() == statistics.scene.lights);
    CHECK(clusters.indices.size() == statistics.scene.lightListEntries);
    CHECK(shadowPlan.views.size() == statistics.scene.shadowMaps);
    // Every light that is to cast shadows got its maps.
    CHECK(statistics.scene.lightsWithoutShadows == 0);
    // The field has no environment: nothing is made or held for one.
    CHECK(statistics.scene.environmentsMade == 0);
    CHECK(statistics.scene.environmentBytes == 0);
    CHECK((statistics.scene.shadowMaps > 0) == withShadows);

    checkBudget(budgets, "drawCalls", statistics.scene.meshes.drawCalls);
    checkBudget(budgets, "screenPasses", statistics.scene.screenPasses);
    checkBudget(budgets, "triangles", static_cast<double>(statistics.scene.meshes.triangles));
    checkBudget(budgets, "lights", statistics.scene.lights);
    checkBudget(budgets, "lightListEntries", statistics.scene.lightListEntries);
    checkBudget(budgets, "shadowMaps", statistics.scene.shadowMaps);
    checkBudget(budgets, "shadowAtlasBytes", static_cast<double>(statistics.scene.shadowAtlasBytes));
    checkBudget(budgets, "ambientOcclusionBytes", static_cast<double>(statistics.scene.ambientOcclusionBytes));
    checkBudget(budgets, "depthOfFieldBytes", static_cast<double>(statistics.scene.depthOfFieldBytes));
    checkBudget(budgets, "lightShaftBytes", static_cast<double>(statistics.scene.lightShaftBytes));

    REQUIRE(budgets.contains("milliseconds"));
    const Json& timeBudgets = budgets["milliseconds"];
#if defined(NDEBUG)
    checkBudget(timeBudgets, "lightCollectMedian", collectTime.median);
    checkBudget(timeBudgets, "lightClustersMedian", clusterTime.median);
    checkBudget(timeBudgets, "shadowPlanMedian", planTime.median);
#endif
    test::checkFrameTimeBudgets(timeBudgets, times);
}

} // namespace

TEST_SUITE("Performance") {
    TEST_CASE("a scene with hundreds of lights stays within its budgets") {
        runScenario("lights", false);
    }

    TEST_CASE("a scene with dozens of shadow maps stays within its budgets") {
        runScenario("shadows", true);
    }
}
