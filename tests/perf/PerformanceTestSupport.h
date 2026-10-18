#pragma once

/// @file
/// What the performance tests share: measuring, writing down what was measured, and
/// holding it against budgets.
///
/// A performance test runs one scenario - the showcase, a scene full of lights - and
/// measures what the brief of the engine asks to be tracked. Every run writes
/// tests/perf/output/<scenario>.json and adds a line to tests/perf/output/history.jsonl,
/// so that two runs can be compared (`scripts/dev.ps1 perf` shows the comparison). The
/// numbers are then checked against the scenario's entry in tests/perf/budgets.json:
///
/// - Counts and sizes - draw calls, triangles, lights, bytes on the GPU - are the same on
///   every machine. Their budgets are tight, and a change that exceeds one has to raise
///   it on purpose.
/// - Times depend on the machine and on what else it is doing. Their budgets only catch a
///   change that makes something many times slower, and are checked in optimized builds
///   only.
///
/// @code
/// std::unique_ptr<Engine> engine = startEngine(measuredEngineDesc("Showcase", assets, cache.path()));
/// const FrameTimes times = measureFrames(*engine, 240);
///
/// Json report = startReport("showcase", engine->graphics().adapter().name, UVec2(960, 540));
/// report["milliseconds"]["cpuFrame"] = toJson(times.cpu);
/// writeReport(report);
/// checkBudget(loadBudgets("showcase")["milliseconds"], "cpuFrameMedian", times.cpu.median);
/// @endcode

#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Time.h"
#include "cordite/engine/Engine.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/MeshPass.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/shader_compiler/EngineShaderCache.h"

#include "render/RenderTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite::test {

/// How a set of measurements is spread.
struct Distribution {
    double median = 0.0;
    /// 95 of 100 measurements are at or below this.
    double percentile95 = 0.0;
    double maximum = 0.0;
};

inline Distribution distributionOf(std::vector<double> samples) {
    if (samples.empty()) {
        return {};
    }
    std::ranges::sort(samples);
    const auto at = [&samples](double share) {
        return samples[static_cast<size_t>(share * static_cast<double>(samples.size() - 1))];
    };
    return {.median = at(0.5), .percentile95 = at(0.95), .maximum = samples.back()};
}

inline Json toJson(const Distribution& distribution) {
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

/// How an engine is set up to be measured: as a game runs, without a window, at 960 by
/// 540 pixels.
inline EngineDesc measuredEngineDesc(std::string applicationName, const std::filesystem::path& assetDirectory,
                                     const std::filesystem::path& cacheDirectory) {
    EngineDesc desc;
    desc.applicationName = std::move(applicationName);
    desc.headless = true;
    // Validation is for finding mistakes; it would be most of what is measured.
    desc.validation = false;
    desc.renderSize = UVec2(960, 540);
    desc.fixedTimeStep = 1.0 / 60.0;
    desc.shaderDirectory = shaderDirectory();
    desc.shaderCompiler = makeSlangShaderCompiler;
    desc.cacheDirectory = cacheDirectory;
    desc.assetDirectory = assetDirectory;
    return desc;
}

inline std::unique_ptr<Engine> startEngine(const EngineDesc& desc) {
    Result<std::unique_ptr<Engine>> engine = Engine::create(desc);
    REQUIRE_MESSAGE(engine.has_value(), test::errorOf(engine));
    return std::move(*engine);
}

/// What a run of frames cost.
struct FrameTimes {
    Distribution cpu;
    /// The part of `cpu` that the scripts of the world took: nothing where none run.
    Distribution scripts;
    /// All zero when `gpuMeasurements` is: the GPU cannot time its work everywhere.
    Distribution gpu;
    uint64_t gpuMeasurements = 0;
};

/// Runs `frames` frames and collects what the engine says each cost.
inline FrameTimes measureFrames(Engine& engine, uint32_t frames) {
    std::vector<double> cpuSamples;
    std::vector<double> scriptSamples;
    std::vector<double> gpuSamples;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        REQUIRE(unwrap(engine.runFrame()));
        const FrameStatistics& statistics = engine.statistics();
        cpuSamples.push_back(statistics.cpuMilliseconds);
        scriptSamples.push_back(statistics.scriptMilliseconds);
        if (statistics.gpuMilliseconds) {
            gpuSamples.push_back(*statistics.gpuMilliseconds);
        }
    }
    return {.cpu = distributionOf(cpuSamples),
            .scripts = distributionOf(scriptSamples),
            .gpu = distributionOf(gpuSamples),
            .gpuMeasurements = static_cast<uint64_t>(gpuSamples.size())};
}

/// The "milliseconds" members every scenario reports: the frame times.
inline void describeFrameTimes(Json& milliseconds, const FrameTimes& times) {
    milliseconds["cpuFrame"] = toJson(times.cpu);
    milliseconds["scripts"] = toJson(times.scripts);
    milliseconds["gpuFrame"] = times.gpuMeasurements > 0 ? toJson(times.gpu) : Json(nullptr);
}

/// What the scene renderer drew, for a report: all passes together, each on its own, and
/// the lights.
inline Json describeScene(const SceneRenderStatistics& statistics) {
    const auto describePass = [](const MeshPassStatistics& pass) {
        Json description = Json::object();
        description["drawCalls"] = pass.drawCalls;
        description["instances"] = pass.instances;
        description["triangles"] = pass.triangles;
        return description;
    };
    Json scene = describePass(statistics.meshes);
    scene["meshRenderers"] = statistics.meshRenderers;
    scene["culled"] = statistics.culled;
    scene["lods"] = statistics.lodInstances;
    scene["skinnedMeshes"] = statistics.skinnedMeshes;
    scene["jointMatrices"] = statistics.jointMatrices;
    scene["screenPasses"] = statistics.screenPasses;
    scene["shadowPass"] = describePass(statistics.shadowPass);
    scene["depthPass"] = describePass(statistics.depthPass);
    scene["basePass"] = describePass(statistics.basePass);
    scene["forwardPass"] = describePass(statistics.forwardPass);
    Json lights = Json::object();
    lights["count"] = statistics.lights;
    lights["listEntries"] = statistics.lightListEntries;
    lights["dropped"] = statistics.droppedLights;
    scene["lights"] = std::move(lights);
    Json shadows = Json::object();
    shadows["maps"] = statistics.shadowMaps;
    shadows["lightsWithout"] = statistics.lightsWithoutShadows;
    shadows["atlasBytes"] = statistics.shadowAtlasBytes;
    scene["shadows"] = std::move(shadows);
    Json environment = Json::object();
    environment["made"] = statistics.environmentsMade;
    environment["bytes"] = statistics.environmentBytes;
    scene["environment"] = std::move(environment);
    Json probes = Json::object();
    probes["captures"] = statistics.probeCaptures;
    probes["bytes"] = statistics.probeBytes;
    scene["probes"] = std::move(probes);
    Json occlusion = Json::object();
    occlusion["bytes"] = statistics.ambientOcclusionBytes;
    scene["ambientOcclusion"] = std::move(occlusion);
    Json focus = Json::object();
    focus["bytes"] = statistics.depthOfFieldBytes;
    scene["depthOfField"] = std::move(focus);
    Json shafts = Json::object();
    shafts["bytes"] = statistics.lightShaftBytes;
    scene["lightShafts"] = std::move(shafts);
    Json reflections = Json::object();
    reflections["bytes"] = statistics.screenReflectionBytes;
    scene["screenReflections"] = std::move(reflections);
    Json fogLight = Json::object();
    fogLight["bytes"] = statistics.fogVolumeBytes;
    scene["fogLight"] = std::move(fogLight);
    Json blur = Json::object();
    blur["bytes"] = statistics.motionBlurBytes;
    scene["motionBlur"] = std::move(blur);
    Json captures = Json::object();
    captures["count"] = statistics.captures;
    captures["dropped"] = statistics.droppedCaptures;
    scene["captures"] = std::move(captures);
    Json decals = Json::object();
    decals["count"] = statistics.decals;
    decals["dropped"] = statistics.droppedDecals;
    decals["normalCopies"] = statistics.decalNormalCopies;
    decals["bytes"] = statistics.decalBytes;
    scene["decals"] = std::move(decals);
    return scene;
}

/// What the scene renderer holds on the GPU, for a report.
inline Json describeResources(const RenderResourceStatistics& statistics) {
    Json resources = Json::object();
    resources["meshes"] = statistics.meshCount;
    resources["textures"] = statistics.textureCount;
    resources["materials"] = statistics.materialCount;
    resources["meshBytes"] = statistics.meshBytes;
    resources["textureBytes"] = statistics.textureBytes;
    return resources;
}

/// A report with what says where its numbers come from; the scenario adds the numbers.
inline Json startReport(std::string_view scenario, std::string_view adapter, UVec2 resolution, uint32_t frames) {
    Json report = Json::object();
    report["scenario"] = std::string(scenario);
    // Set by `scripts/dev.ps1 perf`, so that a line of the history can be traced back.
    report["commit"] = environmentVariable("CORDITE_COMMIT").value_or("unknown");
    report["adapter"] = std::string(adapter);
#if defined(NDEBUG)
    report["optimized"] = true;
#else
    report["optimized"] = false;
#endif
    report["resolution"] = {resolution.x, resolution.y};
    report["frames"] = frames;
    return report;
}

inline std::filesystem::path performanceDirectory() {
    return sourceDirectory() / "tests" / "perf";
}

/// Writes tests/perf/output/<scenario>.json and adds the report to the history.
inline void writeReport(const Json& report) {
    const std::filesystem::path outputDirectory = performanceDirectory() / "output";
    REQUIRE(ensureDirectory(outputDirectory).has_value());
    const std::string scenario = report["scenario"].get<std::string>();
    REQUIRE(writeJsonFile(outputDirectory / pathFromUtf8(scenario + ".json"), report).has_value());
    const std::filesystem::path historyFile = outputDirectory / "history.jsonl";
    const std::string history = fileExists(historyFile) ? unwrap(readTextFile(historyFile)) : std::string();
    REQUIRE(writeTextFile(historyFile, history + dumpJson(report, -1) + "\n").has_value());
}

/// The budgets of a scenario, from tests/perf/budgets.json.
inline Json loadBudgets(std::string_view scenario) {
    const Json allBudgets = unwrap(readJsonFile(performanceDirectory() / "budgets.json"));
    const auto budgets = allBudgets.find(std::string(scenario));
    REQUIRE_MESSAGE(budgets != allBudgets.end(),
                    "tests/perf/budgets.json has no budgets for the scenario " << std::string(scenario));
    return *budgets;
}

/// Checks `value` against the budget of the same name, which must exist.
inline void checkBudget(const Json& budgets, const char* name, double value) {
    const auto budget = budgets.find(name);
    // As a string: doctest shows a `const char*` as a number.
    const std::string budgetName(name);
    REQUIRE_MESSAGE(budget != budgets.end(), "tests/perf/budgets.json has no budget for " << budgetName);
    CAPTURE(budgetName);
    CHECK(value <= budget->get<double>());
}

/// Checks the frame times against the budgets "cpuFrameMedian" and "gpuFrameMedian".
/// Does nothing in builds that are not optimized: their times say nothing.
inline void checkFrameTimeBudgets([[maybe_unused]] const Json& timeBudgets, [[maybe_unused]] const FrameTimes& times) {
#if defined(NDEBUG)
    checkBudget(timeBudgets, "cpuFrameMedian", times.cpu.median);
    if (times.gpuMeasurements > 0) {
        checkBudget(timeBudgets, "gpuFrameMedian", times.gpu.median);
    }
#endif
}

} // namespace cordite::test
