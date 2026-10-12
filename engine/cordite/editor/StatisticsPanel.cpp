#include "cordite/editor/StatisticsPanel.h"

#include <fmt/format.h>
#include <imgui.h>

#include <string>
#include <utility>

namespace cordite {
namespace {

/// How far a shown time moves towards the newest frame's: small enough to steady the
/// digits, large enough to follow a change within a second.
constexpr double kSmoothing = 0.1;

constexpr double kBytesPerMebibyte = 1024.0 * 1024.0;

double smoothed(double shown, double latest) {
    // The first value is taken as it is, rather than creeping up from zero.
    return shown <= 0.0 ? latest : shown + (latest - shown) * kSmoothing;
}

void row(const char* label, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

} // namespace

Json describeFrameStatistics(const FrameStatistics& statistics) {
    const auto describePass = [](const MeshPassStatistics& pass) {
        Json description = Json::object();
        description["drawCalls"] = pass.drawCalls;
        description["instances"] = pass.instances;
        description["triangles"] = pass.triangles;
        return description;
    };

    Json scene = describePass(statistics.scene.meshes);
    scene["views"] = statistics.scene.views;
    scene["meshRenderers"] = statistics.scene.meshRenderers;
    scene["culled"] = statistics.scene.culled;
    Json skinned = Json::object();
    skinned["meshes"] = statistics.scene.skinnedMeshes;
    skinned["jointMatrices"] = statistics.scene.jointMatrices;
    scene["skinned"] = std::move(skinned);
    scene["screenPasses"] = statistics.scene.screenPasses;
    Json lights = Json::object();
    lights["count"] = statistics.scene.lights;
    lights["listEntries"] = statistics.scene.lightListEntries;
    lights["dropped"] = statistics.scene.droppedLights;
    scene["lights"] = std::move(lights);
    Json shadows = Json::object();
    shadows["maps"] = statistics.scene.shadowMaps;
    shadows["lightsWithout"] = statistics.scene.lightsWithoutShadows;
    shadows["atlasBytes"] = statistics.scene.shadowAtlasBytes;
    scene["shadows"] = std::move(shadows);
    Json environment = Json::object();
    environment["made"] = statistics.scene.environmentsMade;
    environment["bytes"] = statistics.scene.environmentBytes;
    scene["environment"] = std::move(environment);
    Json occlusion = Json::object();
    occlusion["bytes"] = statistics.scene.ambientOcclusionBytes;
    scene["ambientOcclusion"] = std::move(occlusion);
    Json passes = Json::object();
    passes["shadow"] = describePass(statistics.scene.shadowPass);
    passes["depth"] = describePass(statistics.scene.depthPass);
    passes["base"] = describePass(statistics.scene.basePass);
    passes["forward"] = describePass(statistics.scene.forwardPass);
    scene["passes"] = std::move(passes);

    Json resources = Json::object();
    resources["meshes"] = statistics.resources.meshCount;
    resources["textures"] = statistics.resources.textureCount;
    resources["materials"] = statistics.resources.materialCount;
    resources["meshBytes"] = statistics.resources.meshBytes;
    resources["textureBytes"] = statistics.resources.textureBytes;

    Json physics = Json::object();
    physics["steps"] = statistics.physics.steps;
    physics["bodies"] = statistics.physics.bodies;
    physics["activeBodies"] = statistics.physics.activeBodies;

    Json animation = Json::object();
    animation["animators"] = statistics.animation.animators;
    animation["joints"] = statistics.animation.joints;

    Json description = Json::object();
    description["frame"] = statistics.frameIndex;
    description["cpuMilliseconds"] = statistics.cpuMilliseconds;
    description["gpuMilliseconds"] = statistics.gpuMilliseconds ? Json(*statistics.gpuMilliseconds) : Json(nullptr);
    description["physics"] = std::move(physics);
    description["animation"] = std::move(animation);
    description["scene"] = std::move(scene);
    description["resources"] = std::move(resources);
    return description;
}

void StatisticsPanel::draw(const FrameStatistics& statistics, bool& open) {
    m_cpuMilliseconds = smoothed(m_cpuMilliseconds, statistics.cpuMilliseconds);
    if (statistics.gpuMilliseconds) {
        m_gpuMilliseconds = smoothed(m_gpuMilliseconds.value_or(0.0), *statistics.gpuMilliseconds);
    }

    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Statistics", &open) && ImGui::BeginTable("##statistics", 2, ImGuiTableFlags_SizingStretchProp)) {
        row("CPU", fmt::format("{:.2f} ms", m_cpuMilliseconds));
        row("GPU", m_gpuMilliseconds ? fmt::format("{:.2f} ms", *m_gpuMilliseconds) : std::string("not measured"));

        row("Bodies", fmt::format("{} ({} awake, {} steps)", statistics.physics.bodies, statistics.physics.activeBodies,
                                  statistics.physics.steps));
        row("Animators", fmt::format("{} ({} joints)", statistics.animation.animators, statistics.animation.joints));

        const SceneRenderStatistics& scene = statistics.scene;
        row("Views", fmt::format("{}", scene.views));
        row("Draw calls", fmt::format("{} ({} shadow, {} depth, {} base, {} forward)", scene.meshes.drawCalls,
                                      scene.shadowPass.drawCalls, scene.depthPass.drawCalls, scene.basePass.drawCalls,
                                      scene.forwardPass.drawCalls));
        row("Screen passes", fmt::format("{}", scene.screenPasses));
        row("Instances", fmt::format("{}", scene.meshes.instances));
        row("Triangles", fmt::format("{}", scene.meshes.triangles));
        row("Mesh renderers", fmt::format("{} ({} out of view)", scene.meshRenderers, scene.culled));
        row("Skinned meshes", fmt::format("{} ({} joint matrices)", scene.skinnedMeshes, scene.jointMatrices));
        row("Lights", scene.droppedLights == 0
                          ? fmt::format("{} ({} in cluster lists)", scene.lights, scene.lightListEntries)
                          : fmt::format("{} ({} in cluster lists, {} left out)", scene.lights, scene.lightListEntries,
                                        scene.droppedLights));
        const double atlasMebibytes = static_cast<double>(scene.shadowAtlasBytes) / kBytesPerMebibyte;
        row("Shadow maps", scene.lightsWithoutShadows == 0
                               ? fmt::format("{} (atlas of {:.1f} MiB)", scene.shadowMaps, atlasMebibytes)
                               : fmt::format("{} (atlas of {:.1f} MiB, {} lights without)", scene.shadowMaps,
                                             atlasMebibytes, scene.lightsWithoutShadows));

        row("Environment", scene.environmentBytes == 0
                               ? std::string("none")
                               : fmt::format("{:.1f} MiB of cube maps",
                                             static_cast<double>(scene.environmentBytes) / kBytesPerMebibyte));

        const RenderResourceStatistics& resources = statistics.resources;
        row("Meshes", fmt::format("{} ({:.1f} MiB)", resources.meshCount,
                                  static_cast<double>(resources.meshBytes) / kBytesPerMebibyte));
        row("Textures", fmt::format("{} ({:.1f} MiB)", resources.textureCount,
                                    static_cast<double>(resources.textureBytes) / kBytesPerMebibyte));
        row("Materials", fmt::format("{}", resources.materialCount));
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace cordite
