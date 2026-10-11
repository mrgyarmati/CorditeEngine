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
    Json lights = Json::object();
    lights["count"] = statistics.scene.lights;
    lights["listEntries"] = statistics.scene.lightListEntries;
    lights["dropped"] = statistics.scene.droppedLights;
    scene["lights"] = std::move(lights);
    Json passes = Json::object();
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

    Json description = Json::object();
    description["frame"] = statistics.frameIndex;
    description["cpuMilliseconds"] = statistics.cpuMilliseconds;
    description["gpuMilliseconds"] = statistics.gpuMilliseconds ? Json(*statistics.gpuMilliseconds) : Json(nullptr);
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

        const SceneRenderStatistics& scene = statistics.scene;
        row("Views", fmt::format("{}", scene.views));
        row("Draw calls",
            fmt::format("{} ({} depth, {} base, {} forward)", scene.meshes.drawCalls, scene.depthPass.drawCalls,
                        scene.basePass.drawCalls, scene.forwardPass.drawCalls));
        row("Instances", fmt::format("{}", scene.meshes.instances));
        row("Triangles", fmt::format("{}", scene.meshes.triangles));
        row("Mesh renderers", fmt::format("{} ({} out of view)", scene.meshRenderers, scene.culled));
        row("Lights", scene.droppedLights == 0
                          ? fmt::format("{} ({} in cluster lists)", scene.lights, scene.lightListEntries)
                          : fmt::format("{} ({} in cluster lists, {} left out)", scene.lights, scene.lightListEntries,
                                        scene.droppedLights));

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
