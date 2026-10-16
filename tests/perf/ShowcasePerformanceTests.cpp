/// @file
/// What the showcase costs: measured, written down, and held against budgets.
///
/// The test starts the engine on the showcase project the way a game would, and measures
/// what the brief of the engine asks to be tracked: frame time, draw calls, GPU memory
/// and asset load time. (Script execution time joins them when there are scripts.)
///
/// How the numbers are reported and checked is described in PerformanceTestSupport.h.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Json.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/Project.h"
#include "cordite/platform/InputMap.h"
#include "cordite/world/SceneSerializer.h"

#include "perf/PerformanceTestSupport.h"
#include "render/RenderTestSupport.h"

#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkBudget;
using cordite::test::FrameTimes;
using cordite::test::millisecondsFor;
using cordite::test::startEngine;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Frames measured once everything is loaded.
constexpr uint32_t kSteadyFrames = 240;

/// Loads every asset of a project into memory, as a game that uses all of them would.
size_t loadEveryAsset(AssetManager& assets) {
    size_t loaded = 0;
    for (const AssetInfo& asset : assets.database().assets()) {
        CAPTURE(asset.reference());
        switch (asset.type) {
            case AssetType::Mesh:
                REQUIRE(assets.loadMesh(asset.id).has_value());
                break;
            case AssetType::Material:
                REQUIRE(assets.loadMaterial(asset.id).has_value());
                break;
            case AssetType::Texture:
                REQUIRE(assets.loadTexture(asset.id).has_value());
                break;
            case AssetType::Model:
                REQUIRE(assets.loadModel(asset.id).has_value());
                break;
            case AssetType::Skeleton:
                REQUIRE(assets.loadSkeleton(asset.id).has_value());
                break;
            case AssetType::Animation:
                REQUIRE(assets.loadAnimation(asset.id).has_value());
                break;
            case AssetType::Script:
                REQUIRE(assets.loadScript(asset.id).has_value());
                break;
            case AssetType::Sound:
                REQUIRE(assets.loadSound(asset.id).has_value());
                break;
            case AssetType::Prefab:
                REQUIRE(assets.loadPrefab(asset.id).has_value());
                break;
            case AssetType::Scene:
                continue;
        }
        ++loaded;
    }
    return loaded;
}

} // namespace

TEST_SUITE("Performance") {
    TEST_CASE("the showcase stays within its budgets") {
        const Project project = unwrap(loadProject(test::sourceDirectory() / "samples" / "showcase"));
        Json milliseconds = Json::object();

        // --- Assets: finding them, and loading all of them into memory -------------------
        size_t assetCount = 0;
        {
            std::unique_ptr<AssetManager> assets;
            milliseconds["assetIndex"] = millisecondsFor(
                [&] { assets = unwrap(AssetManager::create({.rootDirectory = project.assetDirectory()})); });
            milliseconds["assetLoad"] = millisecondsFor([&] { assetCount = loadEveryAsset(*assets); });
        }

        // --- Starting the engine: compiling every shader, then finding them in the cache --
        const TempDirectory cache;
        const EngineDesc desc = test::measuredEngineDesc(project.settings.name, project.assetDirectory(), cache.path());
        std::unique_ptr<Engine> engine;
        milliseconds["coldStart"] = millisecondsFor([&] { engine = startEngine(desc); });
        const std::string adapter = engine->graphics().adapter().name;
        engine.reset();
        milliseconds["warmStart"] = millisecondsFor([&] { engine = startEngine(desc); });

        // --- The scene, and the frame that loads what it shows -------------------------------
        milliseconds["sceneLoad"] = millisecondsFor(
            [&] { REQUIRE(loadScene(project.startScenePath(), engine->world(), engine->components()).has_value()); });
        // The controls of the project, which its scripts ask about; nobody touches them.
        engine->actions().setMap(unwrap(loadInputMap(project.inputMapPath())));
        milliseconds["firstFrame"] = millisecondsFor([&] { REQUIRE(unwrap(engine->runFrame())); });

        // --- Frames like any other -----------------------------------------------------------
        const FrameTimes times = test::measureFrames(*engine, kSteadyFrames);
        test::describeFrameTimes(milliseconds, times);

        const FrameStatistics statistics = engine->statistics();
        // Validation is off, but the device still reports what it cannot do - running
        // out of versions of a buffer because frames pile up, for one.
        const std::vector<std::string> deviceIssues = engine->graphics().validationIssues();
        // What keeps scripts of the showcase from running, which is nothing.
        std::vector<std::string> scriptProblems;
        for (const ScriptProblem& problem : engine->scripts().problems()) {
            scriptProblems.push_back(problem.message);
        }
        engine.reset();

        // --- The report -------------------------------------------------------------------------
        Json report = test::startReport("showcase", adapter, desc.renderSize, kSteadyFrames);
        report["assets"] = assetCount;
        report["milliseconds"] = milliseconds;
        report["scene"] = test::describeScene(statistics.scene);
        Json simulation = Json::object();
        simulation["bodies"] = statistics.physics.bodies;
        simulation["activeBodies"] = statistics.physics.activeBodies;
        simulation["geometries"] = statistics.physics.geometries;
        simulation["physicsJoints"] = statistics.physics.joints;
        simulation["characters"] = statistics.physics.characters;
        simulation["animators"] = statistics.animation.animators;
        simulation["joints"] = statistics.animation.joints;
        simulation["scripts"] = statistics.scripts.scripts;
        simulation["scriptInstances"] = statistics.scripts.instances;
        simulation["scriptCalls"] = statistics.scripts.calls;
        simulation["scriptTimers"] = statistics.scripts.timers;
        simulation["scriptBytes"] = statistics.scripts.memoryBytes;
        simulation["audioVoices"] = statistics.audio.voices;
        simulation["soundBytes"] = statistics.audio.soundBytes;
        report["simulation"] = std::move(simulation);
        report["resources"] = test::describeResources(statistics.resources);
        test::writeReport(report);

        MESSAGE("showcase on " << adapter << ": CPU " << times.cpu.median << " ms, GPU " << times.gpu.median << " ms, "
                               << statistics.scene.meshes.drawCalls << " draw calls, "
                               << statistics.scene.meshes.triangles << " triangles, " << statistics.scene.lights
                               << " lights, "
                               << (statistics.resources.meshBytes + statistics.resources.textureBytes) / 1024
                               << " KiB on the GPU; first frame " << milliseconds["firstFrame"].get<double>() << " ms");

        // --- The budgets ------------------------------------------------------------------------
        const Json budgets = test::loadBudgets("showcase");

        for (const std::string& issue : deviceIssues) {
            FAIL_CHECK("the graphics device reported: " << issue);
        }

        // The showcase is drawn completely: nothing failed to load, nothing was left out.
        CHECK(statistics.scene.views == 1);
        CHECK(statistics.scene.meshRenderers > 0);
        CHECK(statistics.scene.droppedLights == 0);
        CHECK(statistics.scene.lightsWithoutShadows == 0);
        // The sky was made ready in the first frame; a frame that makes it again has
        // lost it in between.
        CHECK(statistics.scene.environmentsMade == 0);
        CHECK(assetCount > 0);

        // What stands and lies in the showcase has come to rest by now, and sleeps.
        CHECK(statistics.physics.steps == 1);
        // But for the three that never do: the triggers over the walkway and at the
        // sentry's post, and the body that stands for the sentry.
        CHECK(statistics.physics.activeBodies <= 3);
        checkBudget(budgets, "bodies", statistics.physics.bodies);
        checkBudget(budgets, "geometries", statistics.physics.geometries);
        checkBudget(budgets, "physicsJoints", statistics.physics.joints);
        checkBudget(budgets, "characters", statistics.physics.characters);
        checkBudget(budgets, "joints", statistics.animation.joints);
        // Every script of the showcase runs: none was stopped, and none failed to load.
        for (const std::string& problem : scriptProblems) {
            FAIL_CHECK(problem);
        }
        CHECK(statistics.scripts.stopped == 0);
        checkBudget(budgets, "scripts", statistics.scripts.scripts);
        checkBudget(budgets, "scriptInstances", statistics.scripts.instances);
        checkBudget(budgets, "scriptCalls", statistics.scripts.calls);
        checkBudget(budgets, "scriptTimers", statistics.scripts.timers);
        checkBudget(budgets, "scriptBytes", static_cast<double>(statistics.scripts.memoryBytes));
        // What is heard: nothing has to wait for a voice, and somebody listens.
        CHECK(statistics.audio.waiting == 0);
        CHECK(statistics.audio.listeners == 1);
        checkBudget(budgets, "audioVoices", statistics.audio.voices);
        checkBudget(budgets, "soundBytes", static_cast<double>(statistics.audio.soundBytes));
        checkBudget(budgets, "drawCalls", statistics.scene.meshes.drawCalls);
        checkBudget(budgets, "screenPasses", statistics.scene.screenPasses);
        checkBudget(budgets, "triangles", static_cast<double>(statistics.scene.meshes.triangles));
        checkBudget(budgets, "lights", statistics.scene.lights);
        checkBudget(budgets, "lightListEntries", statistics.scene.lightListEntries);
        checkBudget(budgets, "shadowMaps", statistics.scene.shadowMaps);
        checkBudget(budgets, "shadowAtlasBytes", static_cast<double>(statistics.scene.shadowAtlasBytes));
        checkBudget(budgets, "environmentBytes", static_cast<double>(statistics.scene.environmentBytes));
        checkBudget(budgets, "ambientOcclusionBytes", static_cast<double>(statistics.scene.ambientOcclusionBytes));
        checkBudget(budgets, "depthOfFieldBytes", static_cast<double>(statistics.scene.depthOfFieldBytes));
        checkBudget(budgets, "meshBytes", static_cast<double>(statistics.resources.meshBytes));
        checkBudget(budgets, "textureBytes", static_cast<double>(statistics.resources.textureBytes));

        REQUIRE(budgets.contains("milliseconds"));
        const Json& timeBudgets = budgets["milliseconds"];
#if defined(NDEBUG)
        for (const char* name : {"assetIndex", "assetLoad", "coldStart", "warmStart", "sceneLoad", "firstFrame"}) {
            checkBudget(timeBudgets, name, milliseconds[name].get<double>());
        }
        // What the scripts of the showcase take of a frame, at its middle and at its worst.
        checkBudget(timeBudgets, "scriptsMedian", times.scripts.median);
        checkBudget(timeBudgets, "scriptsMaximum", times.scripts.maximum);
#endif
        // Whatever the build, they ran, and it took some time.
        CHECK(times.scripts.median > 0.0);
        test::checkFrameTimeBudgets(timeBudgets, times);
    }
}
