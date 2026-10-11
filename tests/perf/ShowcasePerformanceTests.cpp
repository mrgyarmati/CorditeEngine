/// @file
/// What the showcase costs: measured, written down, and held against budgets.
///
/// The test starts the engine on the showcase project the way a game would, and measures
/// what the brief of the engine asks to be tracked: frame time, draw calls, GPU memory
/// and asset load time. (Script execution time joins them when there are scripts.)
///
/// Every run writes tests/perf/output/showcase.json and adds a line to
/// tests/perf/output/history.jsonl, so that two runs can be compared. The numbers are
/// then checked against tests/perf/budgets.json:
///
/// - Counts and sizes - draw calls, triangles, bytes on the GPU - are the same on every
///   machine. Their budgets are tight, and a change that exceeds one has to raise it on
///   purpose.
/// - Times depend on the machine and on what else it is doing. Their budgets only catch a
///   change that makes something many times slower, and are checked in optimized builds
///   only.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Time.h"
#include "cordite/engine/Engine.h"
#include "cordite/engine/Project.h"
#include "cordite/world/SceneSerializer.h"

#include "render/RenderTestSupport.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Frames measured once everything is loaded.
constexpr uint32_t kSteadyFrames = 240;

/// How a set of measurements is spread.
struct Distribution {
    double median = 0.0;
    /// 95 of 100 measurements are at or below this.
    double percentile95 = 0.0;
    double maximum = 0.0;
};

Distribution describe(std::vector<double> samples) {
    if (samples.empty()) {
        return {};
    }
    std::ranges::sort(samples);
    const auto at = [&samples](double share) {
        return samples[static_cast<size_t>(share * static_cast<double>(samples.size() - 1))];
    };
    return {.median = at(0.5), .percentile95 = at(0.95), .maximum = samples.back()};
}

Json toJson(const Distribution& distribution) {
    Json json = Json::object();
    json["median"] = distribution.median;
    json["percentile95"] = distribution.percentile95;
    json["maximum"] = distribution.maximum;
    return json;
}

/// The milliseconds `work` takes.
template <typename Work>
double millisecondsFor(Work&& work) {
    const Stopwatch stopwatch;
    std::forward<Work>(work)();
    return stopwatch.elapsedSeconds() * 1000.0;
}

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
            case AssetType::Scene:
                continue;
        }
        ++loaded;
    }
    return loaded;
}

EngineDesc gameDesc(const Project& project, const std::filesystem::path& cacheDirectory) {
    EngineDesc desc;
    desc.applicationName = project.settings.name;
    desc.headless = true;
    // Validation is for finding mistakes; it would be most of what is measured.
    desc.validation = false;
    desc.renderSize = UVec2(960, 540);
    desc.fixedTimeStep = 1.0 / 60.0;
    desc.shaderDirectory = test::shaderDirectory();
    desc.cacheDirectory = cacheDirectory;
    desc.assetDirectory = project.assetDirectory();
    return desc;
}

std::unique_ptr<Engine> startEngine(const EngineDesc& desc) {
    Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
    REQUIRE_MESSAGE(engine.has_value(), engine.error().describe());
    return std::move(*engine);
}

/// Checks `value` against the budget of the same name, if there is one.
void checkBudget(const Json& budgets, const char* name, double value) {
    const auto budget = budgets.find(name);
    REQUIRE_MESSAGE(budget != budgets.end(), "tests/perf/budgets.json has no budget for " << name);
    CAPTURE(name);
    CHECK(value <= budget->get<double>());
}

} // namespace

TEST_SUITE("Performance") {
    TEST_CASE("the showcase stays within its budgets") {
        const std::filesystem::path perfDirectory = test::sourceDirectory() / "tests" / "perf";
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
        const EngineDesc desc = gameDesc(project, cache.path());
        std::unique_ptr<Engine> engine;
        milliseconds["coldStart"] = millisecondsFor([&] { engine = startEngine(desc); });
        const std::string adapter = engine->graphics().adapter().name;
        engine.reset();
        milliseconds["warmStart"] = millisecondsFor([&] { engine = startEngine(desc); });

        // --- The scene, and the frame that loads what it shows -------------------------------
        milliseconds["sceneLoad"] = millisecondsFor(
            [&] { REQUIRE(loadScene(project.startScenePath(), engine->world(), engine->components()).has_value()); });
        milliseconds["firstFrame"] = millisecondsFor([&] { REQUIRE(unwrap(engine->runFrame())); });

        // --- Frames like any other -----------------------------------------------------------
        std::vector<double> cpuSamples;
        std::vector<double> gpuSamples;
        uint64_t measurements = 0;
        for (uint32_t frame = 0; frame < kSteadyFrames; ++frame) {
            REQUIRE(unwrap(engine->runFrame()));
            const FrameStatistics& statistics = engine->statistics();
            cpuSamples.push_back(statistics.cpuMilliseconds);
            if (statistics.gpuMilliseconds) {
                gpuSamples.push_back(*statistics.gpuMilliseconds);
                ++measurements;
            }
        }
        const Distribution cpuFrame = describe(cpuSamples);
        const Distribution gpuFrame = describe(gpuSamples);
        milliseconds["cpuFrame"] = toJson(cpuFrame);
        milliseconds["gpuFrame"] = measurements > 0 ? toJson(gpuFrame) : Json(nullptr);

        const FrameStatistics statistics = engine->statistics();
        // Validation is off, but the device still reports what it cannot do - running
        // out of versions of a buffer because frames pile up, for one.
        const std::vector<std::string> deviceIssues = engine->graphics().validationIssues();
        engine.reset();

        // --- The report -------------------------------------------------------------------------
        const auto describePass = [](const MeshPassStatistics& pass) {
            Json description = Json::object();
            description["drawCalls"] = pass.drawCalls;
            description["instances"] = pass.instances;
            description["triangles"] = pass.triangles;
            return description;
        };
        // All passes together, and each on its own.
        Json scene = describePass(statistics.scene.meshes);
        scene["meshRenderers"] = statistics.scene.meshRenderers;
        scene["culled"] = statistics.scene.culled;
        scene["depthPass"] = describePass(statistics.scene.depthPass);
        scene["basePass"] = describePass(statistics.scene.basePass);
        scene["forwardPass"] = describePass(statistics.scene.forwardPass);

        Json resources = Json::object();
        resources["meshes"] = statistics.resources.meshCount;
        resources["textures"] = statistics.resources.textureCount;
        resources["materials"] = statistics.resources.materialCount;
        resources["meshBytes"] = statistics.resources.meshBytes;
        resources["textureBytes"] = statistics.resources.textureBytes;

        Json report = Json::object();
        report["scenario"] = "showcase";
        // Set by `scripts/dev.ps1 perf`, so that a line of the history can be traced back.
        report["commit"] = environmentVariable("CORDITE_COMMIT").value_or("unknown");
        report["adapter"] = adapter;
#if defined(NDEBUG)
        report["optimized"] = true;
#else
        report["optimized"] = false;
#endif
        report["resolution"] = {desc.renderSize.x, desc.renderSize.y};
        report["frames"] = kSteadyFrames;
        report["assets"] = assetCount;
        report["milliseconds"] = milliseconds;
        report["scene"] = scene;
        report["resources"] = resources;

        const std::filesystem::path outputDirectory = perfDirectory / "output";
        REQUIRE(ensureDirectory(outputDirectory).has_value());
        REQUIRE(writeJsonFile(outputDirectory / "showcase.json", report).has_value());
        const std::filesystem::path historyFile = outputDirectory / "history.jsonl";
        const std::string history = fileExists(historyFile) ? unwrap(readTextFile(historyFile)) : std::string();
        REQUIRE(writeTextFile(historyFile, history + dumpJson(report, -1) + "\n").has_value());

        MESSAGE("showcase on " << adapter << ": CPU " << cpuFrame.median << " ms, GPU " << gpuFrame.median << " ms, "
                               << statistics.scene.meshes.drawCalls << " draw calls, "
                               << statistics.scene.meshes.triangles << " triangles, "
                               << (statistics.resources.meshBytes + statistics.resources.textureBytes) / 1024
                               << " KiB on the GPU; first frame " << milliseconds["firstFrame"].get<double>() << " ms");

        // --- The budgets ------------------------------------------------------------------------
        const Json allBudgets = unwrap(readJsonFile(perfDirectory / "budgets.json"));
        REQUIRE(allBudgets.contains("showcase"));
        const Json& budgets = allBudgets["showcase"];

        for (const std::string& issue : deviceIssues) {
            FAIL_CHECK("the graphics device reported: " << issue);
        }

        // The showcase is drawn completely: nothing failed to load, nothing was left out.
        CHECK(statistics.scene.views == 1);
        CHECK(statistics.scene.meshRenderers > 0);
        CHECK(assetCount > 0);

        checkBudget(budgets, "drawCalls", statistics.scene.meshes.drawCalls);
        checkBudget(budgets, "triangles", static_cast<double>(statistics.scene.meshes.triangles));
        checkBudget(budgets, "meshBytes", static_cast<double>(statistics.resources.meshBytes));
        checkBudget(budgets, "textureBytes", static_cast<double>(statistics.resources.textureBytes));

#if defined(NDEBUG)
        REQUIRE(budgets.contains("milliseconds"));
        const Json& timeBudgets = budgets["milliseconds"];
        for (const char* name : {"assetIndex", "assetLoad", "coldStart", "warmStart", "sceneLoad", "firstFrame"}) {
            checkBudget(timeBudgets, name, milliseconds[name].get<double>());
        }
        checkBudget(timeBudgets, "cpuFrameMedian", cpuFrame.median);
        if (measurements > 0) {
            checkBudget(timeBudgets, "gpuFrameMedian", gpuFrame.median);
        }
#endif
    }
}
