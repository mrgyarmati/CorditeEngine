#include "cordite/renderer/SceneRenderer.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/math/Frustum.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace cordite {
namespace {

ShaderRequest pixelShader(const char* path, const char* entryPoint) {
    return {.path = path, .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
}

/// Draws the depth of solid surfaces. Opaque ones need no pixel shader for that; masked
/// ones need one that cuts their holes.
MeshPassDesc depthPassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Depth pre-pass";
    desc.maskedPixelShader = pixelShader("passes/MeshDepth.slang", "meshDepthMaskedPixel");
    desc.depth = MeshPassDepth::TestAndWrite;
    return desc;
}

/// Fills the G-buffer, only where a surface is the one the depth pre-pass left in front.
MeshPassDesc basePassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Base pass";
    desc.pixelShader = pixelShader("passes/MeshGBuffer.slang", "meshGBufferPixel");
    desc.depth = MeshPassDepth::Equal;
    return desc;
}

/// Shades translucent surfaces and blends them over what is there, behind what is solid.
MeshPassDesc forwardPassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Forward pass";
    desc.pixelShader = pixelShader("passes/MeshForward.slang", "meshForwardPixel");
    desc.depth = MeshPassDepth::TestOnly;
    desc.blend = MeshPassBlend::Alpha;
    return desc;
}

MeshPassDesc wireframePassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Wireframe view";
    desc.pixelShader = pixelShader("passes/MeshDebug.slang", "meshWireframePixel");
    desc.depth = MeshPassDepth::TestAndWrite;
    desc.wireframe = true;
    return desc;
}

/// Every surface adds to the picture, hidden or not.
MeshPassDesc overdrawPassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Overdraw view";
    desc.pixelShader = pixelShader("passes/MeshDebug.slang", "meshOverdrawPixel");
    desc.depth = MeshPassDepth::None;
    desc.blend = MeshPassBlend::Additive;
    return desc;
}

/// What the lighting pass makes of the G-buffer for a view that shows `debugView`.
DeferredLightingMode lightingModeOf(RenderDebugView debugView) {
    if (debugView == RenderDebugView::LightingOnly) {
        return DeferredLightingMode::LightingOnly;
    }
    if (debugView == RenderDebugView::Reflections) {
        return DeferredLightingMode::Reflections;
    }
    return DeferredLightingMode::Lit;
}

} // namespace

std::string_view nameOf(RenderQuality quality) {
    switch (quality) {
        case RenderQuality::Low:
            return "low";
        case RenderQuality::Medium:
            return "medium";
        case RenderQuality::High:
            return "high";
    }
    return "unknown";
}

std::string_view displayNameOf(RenderQuality quality) {
    switch (quality) {
        case RenderQuality::Low:
            return "Low";
        case RenderQuality::Medium:
            return "Medium";
        case RenderQuality::High:
            return "High";
    }
    return "Unknown";
}

std::optional<RenderQuality> renderQualityFromName(std::string_view name) {
    for (const RenderQuality quality : kRenderQualities) {
        if (equalsIgnoreCase(name, nameOf(quality))) {
            return quality;
        }
    }
    return std::nullopt;
}

void applyRenderQuality(RenderQuality quality, SceneRenderSettings& settings) {
    switch (quality) {
        case RenderQuality::Low:
            settings.shadowQuality = ShadowQuality::Low;
            settings.postProcessQuality = PostProcessQuality::Low;
            break;
        case RenderQuality::Medium:
            settings.shadowQuality = ShadowQuality::Medium;
            settings.postProcessQuality = PostProcessQuality::Medium;
            break;
        case RenderQuality::High:
            settings.shadowQuality = ShadowQuality::High;
            settings.postProcessQuality = PostProcessQuality::High;
            break;
    }
}

std::optional<RenderQuality> renderQualityOf(const SceneRenderSettings& settings) {
    for (const RenderQuality quality : kRenderQualities) {
        SceneRenderSettings preset;
        applyRenderQuality(quality, preset);
        if (preset.shadowQuality == settings.shadowQuality &&
            preset.postProcessQuality == settings.postProcessQuality) {
            return quality;
        }
    }
    return std::nullopt;
}

SceneRenderStatistics& SceneRenderStatistics::operator+=(const SceneRenderStatistics& other) {
    views += other.views;
    meshRenderers += other.meshRenderers;
    culled += other.culled;
    screenPasses += other.screenPasses;
    lights += other.lights;
    lightListEntries += other.lightListEntries;
    droppedLights += other.droppedLights;
    shadowMaps += other.shadowMaps;
    lightsWithoutShadows += other.lightsWithoutShadows;
    shadowAtlasBytes = std::max(shadowAtlasBytes, other.shadowAtlasBytes);
    environmentsMade += other.environmentsMade;
    environmentBytes = std::max(environmentBytes, other.environmentBytes);
    shadowPass += other.shadowPass;
    depthPass += other.depthPass;
    basePass += other.basePass;
    forwardPass += other.forwardPass;
    meshes += other.meshes;
    return *this;
}

SceneRenderer::SceneRenderer(nvrhi::IDevice* device, std::unique_ptr<RenderResources> resources,
                             ViewBindings viewBindings, Passes passes, bool supportsWireframe)
    : m_device(device),
      m_resources(std::move(resources)),
      m_viewBindings(std::move(viewBindings)),
      m_drawList(device),
      m_passes(std::move(passes)),
      m_supportsWireframe(supportsWireframe) {}

SceneRenderer::~SceneRenderer() = default;

Result<std::unique_ptr<SceneRenderer>> SceneRenderer::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             AssetManager& assets, const SceneRendererDesc& desc) {
    CORDITE_TRY_ASSIGN(std::unique_ptr<RenderResources> resources,
                       RenderResources::create(device, assets, desc.resources));
    CORDITE_TRY_ASSIGN(ViewBindings viewBindings, ViewBindings::create(device));

    nvrhi::IBindingLayout* viewLayout = viewBindings.layout();
    nvrhi::IBindingLayout* materialLayout = resources->materialLayout();
    const auto meshPass = [&](MeshPassDesc passDesc) {
        return MeshPass::create(device, shaders, viewLayout, materialLayout, std::move(passDesc));
    };
    CORDITE_TRY_ASSIGN(MeshPass depth, meshPass(depthPassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass base, meshPass(basePassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass forward, meshPass(forwardPassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass wireframe, meshPass(wireframePassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass overdraw, meshPass(overdrawPassDesc()));
    CORDITE_TRY_ASSIGN(DeferredLightingPass lighting, DeferredLightingPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DebugViewPass debugView, DebugViewPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DisplayEncodePass encode, DisplayEncodePass::create(device, shaders));
    CORDITE_TRY_ASSIGN(ShadowAtlas shadows, ShadowAtlas::create(device, shaders, materialLayout));
    CORDITE_TRY_ASSIGN(EnvironmentMaps environments, EnvironmentMaps::create(device, shaders, assets));
    CORDITE_TRY_ASSIGN(PostProcessPass postProcess, PostProcessPass::create(device, shaders));

    Passes passes{.depth = std::move(depth),
                  .base = std::move(base),
                  .forward = std::move(forward),
                  .wireframe = std::move(wireframe),
                  .overdraw = std::move(overdraw),
                  .lighting = std::move(lighting),
                  .debugView = std::move(debugView),
                  .encode = std::move(encode),
                  .shadows = std::move(shadows),
                  .environments = std::move(environments),
                  .postProcess = std::move(postProcess)};
    return std::unique_ptr<SceneRenderer>(new SceneRenderer(device, std::move(resources), std::move(viewBindings),
                                                            std::move(passes), desc.supportsWireframe));
}

void SceneRenderer::collectDraws(const World& world, const CameraView& view) {
    m_sceneMeshes.clear();
    m_sceneDraws.clear();
    m_draws.clear();
    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection);
    const auto renderers = world.registry().view<const MeshRendererComponent, const WorldTransform>();
    for (const auto [handle, renderer, placement] : renderers.each()) {
        if (!world.isEnabledInHierarchy(Entity{handle})) {
            continue;
        }
        const GpuMesh* mesh = m_resources->mesh(renderer.mesh);
        if (mesh == nullptr) {
            continue;
        }
        ++m_statistics.meshRenderers;

        // Materials are resolved for everything in the scene, in view or not, so that
        // turning the camera does not stall on loading what comes into sight. What the
        // camera does not see may also throw a shadow into what it sees.
        const size_t firstDraw = m_sceneDraws.size();
        for (uint32_t submesh = 0; submesh < mesh->submeshes.size(); ++submesh) {
            const Uuid material = submesh < renderer.materials.size() ? renderer.materials[submesh] : Uuid();
            m_sceneDraws.push_back({.mesh = mesh,
                                    .submesh = submesh,
                                    .material = &m_resources->material(material),
                                    .localToWorld = placement.matrix,
                                    .lightChannels = renderer.lightChannels});
        }
        const Aabb bounds = mesh->bounds.transformed(placement.matrix);
        m_sceneMeshes.push_back({.bounds = bounds,
                                 .firstDraw = static_cast<uint32_t>(firstDraw),
                                 .drawCount = static_cast<uint32_t>(m_sceneDraws.size() - firstDraw),
                                 .lightChannels = renderer.lightChannels & kAllLightChannels,
                                 .castsShadows = renderer.castsShadows});
        if (frustum.intersects(bounds)) {
            m_draws.insert(m_draws.end(), m_sceneDraws.begin() + static_cast<std::ptrdiff_t>(firstDraw),
                           m_sceneDraws.end());
        } else {
            ++m_statistics.culled;
        }
    }
}

void SceneRenderer::drawShadows(nvrhi::ICommandList* commandList, const ShadowSettings& settings) {
    m_statistics.lightsWithoutShadows = m_shadowPlan.lightsWithoutRoom;
    if (m_shadowPlan.lightsWithoutRoom > 0 && !m_reportedShadowRoom) {
        CORDITE_LOG_WARN("Renderer",
                         "{} lights that are to cast shadows cast none: the shadow atlas of {} texels a side "
                         "is full, a view has its {} shadow maps, or they are directional lights other than "
                         "the brightest",
                         m_shadowPlan.lightsWithoutRoom, settings.atlasSize, kMaxShadowViews);
        m_reportedShadowRoom = true;
    }
    // With shadows switched off, the atlas gives its memory back: what stays is as
    // small as an atlas can be, to have something to bind. A view that merely has no
    // shadow to draw leaves the atlas as it is, for the next view that has.
    const bool switchedOff = settings.atlasSize < kMinShadowTileSize;
    if (m_shadowPlan.views.empty() && !switchedOff) {
        return;
    }

    const uint32_t atlasSize = switchedOff ? kMinShadowTileSize : settings.atlasSize;
    if (const Status resized = m_passes.shadows.resize(atlasSize); !resized) {
        // Without an atlas there are no shadows; the lights shine as if nothing stood
        // in them, which is wrong but shows the scene.
        if (!m_reportedShadowAtlas) {
            CORDITE_LOG_ERROR("Renderer", "no shadows are drawn: {}", resized.error().describe());
            m_reportedShadowAtlas = true;
        }
        for (GpuLight& light : m_clusters.lights) {
            light.shadowView = -1;
        }
        m_shadowPlan.views.clear();
        m_shadowPlan.cascades = {};
        return;
    }
    if (m_shadowPlan.views.empty()) {
        return;
    }

    m_passes.shadows.clear(commandList);
    while (m_shadowDrawLists.size() < m_shadowPlan.views.size()) {
        m_shadowDrawLists.emplace_back(m_device);
    }
    for (size_t index = 0; index < m_shadowPlan.views.size(); ++index) {
        const ShadowView& shadow = m_shadowPlan.views[index];
        // What the light sees, of what stands in its light at all: a mesh that shares
        // no lighting channel with a light is not lit by it and blocks none of it.
        const Frustum frustum = Frustum::fromViewProjection(shadow.viewProjection);
        const uint32_t channels = (m_clusters.lights[shadow.light].typeAndChannels >> 8u) & kAllLightChannels;
        m_shadowDraws.clear();
        for (const SceneMesh& mesh : m_sceneMeshes) {
            if (mesh.castsShadows && (mesh.lightChannels & channels) != 0 && frustum.intersects(mesh.bounds)) {
                const auto first = m_sceneDraws.begin() + static_cast<std::ptrdiff_t>(mesh.firstDraw);
                m_shadowDraws.insert(m_shadowDraws.end(), first, first + static_cast<std::ptrdiff_t>(mesh.drawCount));
            }
        }
        // Only solid surfaces are drawn: what lets light through casts no shadow. Where
        // the list is seen from matters for blended surfaces alone.
        MeshDrawList& list = m_shadowDrawLists[index];
        list.build(commandList, m_shadowDraws, Vec3(0.0f), kWorldForward);
        m_statistics.shadowPass +=
            m_passes.shadows.draw(commandList, shadow, list.solidBatches(), list.instanceBuffer());
    }
    m_statistics.shadowMaps = static_cast<uint32_t>(m_shadowPlan.views.size());
}

void SceneRenderer::render(nvrhi::ICommandList* commandList, const World& world, const CameraView& view,
                           SceneTargets& targets, const SceneRenderSettings& settings) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid(), "render needs a command list and valid targets");
    m_statistics = {};
    if (commandList == nullptr || !targets.isValid()) {
        return;
    }

    m_resources->beginFrame();
    collectDraws(world, view);
    // Before anything that uses the new resources is recorded behind them.
    m_resources->flushUploads();

    // The lights: gathered from the world, sorted into the clusters of this view.
    m_lights.collect(world, {.defaultLighting = settings.defaultLighting});
    m_clusterBuilder.build(view, m_lights.lights(), m_clusters);
    m_statistics.lights = static_cast<uint32_t>(m_clusters.lights.size());
    m_statistics.lightListEntries = static_cast<uint32_t>(m_clusters.indices.size());
    m_statistics.droppedLights = m_clusters.droppedLights;
    if (m_clusters.droppedLights > 0 && !m_reportedDroppedLights) {
        CORDITE_LOG_WARN("Renderer",
                         "too many lights for one view: {} left out (a view holds {} directional lights and {} "
                         "point and spot lights)",
                         m_clusters.droppedLights, kMaxDirectionalLights, kMaxLocalLights);
        m_reportedDroppedLights = true;
    }

    // What surrounds the scene. Its environment is made ready the first time it is
    // used, by passes recorded here, ahead of the ones it lights.
    const SceneEnvironment& surroundings = m_lights.environment();
    const uint32_t environmentsBefore = m_passes.environments.bakeCount();
    const GpuEnvironment* environment =
        m_passes.environments.environment(commandList, surroundings.texture, *m_resources);
    m_resources->flushUploads();
    m_statistics.environmentsMade = m_passes.environments.bakeCount() - environmentsBefore;
    m_statistics.environmentBytes = m_passes.environments.byteSize();

    RenderDebugView debugView = settings.debugView;
    if (debugView == RenderDebugView::Wireframe && !m_supportsWireframe) {
        if (!m_reportedWireframe) {
            CORDITE_LOG_WARN("Renderer", "this adapter cannot draw wireframes; the lit picture is shown instead");
            m_reportedWireframe = true;
        }
        debugView = RenderDebugView::None;
    }

    // The shadows of those lights: planned for every view, drawn for the views that
    // are lit - the others show something else in place of the light.
    const ShadowSettings shadowSettings = shadowSettingsFor(settings.shadowQuality);
    planShadows(view, m_lights.lights(), m_clusters, shadowSettings, m_shadowPlan);
    if (debugView == RenderDebugView::None || debugView == RenderDebugView::LightingOnly) {
        drawShadows(commandList, shadowSettings);
    }

    m_viewBindings.update(
        commandList,
        {.camera = view,
         .targetSize = targets.size(),
         .skyColor = m_lights.skyColor(),
         .groundColor = m_lights.groundColor()},
        m_clusters,
        {.plan = &m_shadowPlan, .atlas = m_passes.shadows.texture(), .filterRadius = shadowSettings.filterRadius},
        // An environment whose image cannot be used lights nothing.
        {.environment = environment != nullptr ? environment : &m_passes.environments.none(),
         .intensity = environment != nullptr ? surroundings.intensity : 0.0f,
         .rotation = surroundings.rotation,
         .visible = surroundings.visible});
    m_drawList.build(commandList, m_draws, view.position, view.forward);
    nvrhi::IBindingSet* viewBindings = m_viewBindings.bindings();
    nvrhi::IBuffer* instances = m_drawList.instanceBuffer();

    targets.clear(commandList, settings.backgroundColor);
    if (debugView == RenderDebugView::Wireframe) {
        m_statistics.forwardPass = m_passes.wireframe.draw(commandList, targets.sceneFramebuffer(),
                                                           m_drawList.batches(), viewBindings, instances);
    } else if (debugView == RenderDebugView::Overdraw) {
        // Counted from black, whatever the background is.
        commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        m_statistics.forwardPass = m_passes.overdraw.draw(commandList, targets.lightingFramebuffer(),
                                                          m_drawList.batches(), viewBindings, instances);
    } else {
        m_statistics.depthPass = m_passes.depth.draw(commandList, targets.depthFramebuffer(), m_drawList.solidBatches(),
                                                     viewBindings, instances);
        m_statistics.basePass = m_passes.base.draw(commandList, targets.gbufferFramebuffer(), m_drawList.solidBatches(),
                                                   viewBindings, instances);
        if (isDrawnFromGBuffer(debugView)) {
            m_passes.debugView.draw(commandList, targets, viewBindings, debugView);
        } else {
            m_passes.lighting.draw(commandList, targets, viewBindings, lightingModeOf(debugView));
        }
        ++m_statistics.screenPasses;
        if (debugView == RenderDebugView::None) {
            m_statistics.forwardPass = m_passes.forward.draw(commandList, targets.sceneFramebuffer(),
                                                             m_drawList.blendedBatches(), viewBindings, instances);
        }
    }
    if (settings.postProcessQuality != PostProcessQuality::Off && debugView == RenderDebugView::None) {
        const PostProcessSettings look =
            settings.postProcess
                ? *settings.postProcess
                : resolvePostProcess(world, {.position = view.position, .camera = settings.viewEntity});
        // A lookup table that is used for the first time goes to the GPU now.
        nvrhi::ITexture* colorLut = m_resources->dataTexture(look.colorLut);
        m_resources->flushUploads();
        m_statistics.screenPasses += m_passes.postProcess.draw(commandList, targets, look,
                                                               {.quality = settings.postProcessQuality,
                                                                .deltaSeconds = settings.deltaSeconds,
                                                                .timeSeconds = settings.timeSeconds,
                                                                .colorLut = colorLut});
    } else {
        m_passes.encode.draw(commandList, targets.sceneColor(), targets.displayFramebuffer());
        ++m_statistics.screenPasses;
    }

    m_statistics.views = 1;
    m_statistics.shadowAtlasBytes = m_passes.shadows.byteSize();
    m_statistics.meshes = m_statistics.shadowPass;
    m_statistics.meshes += m_statistics.depthPass;
    m_statistics.meshes += m_statistics.basePass;
    m_statistics.meshes += m_statistics.forwardPass;
    m_frameStatistics += m_statistics;
}

SceneRenderStatistics SceneRenderer::takeFrameStatistics() {
    return std::exchange(m_frameStatistics, {});
}

} // namespace cordite
