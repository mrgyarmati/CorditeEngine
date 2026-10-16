#include "cordite/renderer/SceneRenderer.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/Pose.h"
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

/// Whether the picture of a view is finished (PostProcessPass) or only encoded: a debug
/// view is not, nor is a view that asks for no finishing.
bool isFinishedPicture(const SceneRenderSettings& settings, RenderDebugView debugView) {
    return settings.postProcessQuality != PostProcessQuality::Off && debugView == RenderDebugView::None;
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
            settings.screenEffectsQuality = ScreenEffectsQuality::Low;
            settings.postProcessQuality = PostProcessQuality::Low;
            break;
        case RenderQuality::Medium:
            settings.shadowQuality = ShadowQuality::Medium;
            settings.screenEffectsQuality = ScreenEffectsQuality::Medium;
            settings.postProcessQuality = PostProcessQuality::Medium;
            break;
        case RenderQuality::High:
            settings.shadowQuality = ShadowQuality::High;
            settings.screenEffectsQuality = ScreenEffectsQuality::High;
            settings.postProcessQuality = PostProcessQuality::High;
            break;
    }
}

std::optional<RenderQuality> renderQualityOf(const SceneRenderSettings& settings) {
    for (const RenderQuality quality : kRenderQualities) {
        SceneRenderSettings preset;
        applyRenderQuality(quality, preset);
        if (preset.shadowQuality == settings.shadowQuality &&
            preset.screenEffectsQuality == settings.screenEffectsQuality &&
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
    skinnedMeshes += other.skinnedMeshes;
    jointMatrices += other.jointMatrices;
    screenPasses += other.screenPasses;
    lights += other.lights;
    lightListEntries += other.lightListEntries;
    droppedLights += other.droppedLights;
    decals += other.decals;
    droppedDecals += other.droppedDecals;
    decalNormalCopies += other.decalNormalCopies;
    decalBytes = std::max(decalBytes, other.decalBytes);
    shadowMaps += other.shadowMaps;
    lightsWithoutShadows += other.lightsWithoutShadows;
    shadowAtlasBytes = std::max(shadowAtlasBytes, other.shadowAtlasBytes);
    environmentsMade += other.environmentsMade;
    environmentBytes = std::max(environmentBytes, other.environmentBytes);
    ambientOcclusionBytes = std::max(ambientOcclusionBytes, other.ambientOcclusionBytes);
    depthOfFieldBytes = std::max(depthOfFieldBytes, other.depthOfFieldBytes);
    lightShaftBytes = std::max(lightShaftBytes, other.lightShaftBytes);
    shadowPass += other.shadowPass;
    depthPass += other.depthPass;
    basePass += other.basePass;
    forwardPass += other.forwardPass;
    meshes += other.meshes;
    return *this;
}

SceneRenderer::SceneRenderer(nvrhi::IDevice* device, std::unique_ptr<RenderResources> resources,
                             ViewBindings viewBindings, SkinPalettes palettes, Passes passes, bool supportsWireframe)
    : m_device(device),
      m_resources(std::move(resources)),
      m_viewBindings(std::move(viewBindings)),
      m_palettes(std::move(palettes)),
      m_drawList(device),
      m_passes(std::move(passes)),
      m_supportsWireframe(supportsWireframe) {}

SceneRenderer::~SceneRenderer() = default;

Result<std::unique_ptr<SceneRenderer>> SceneRenderer::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             AssetManager& assets, const SceneRendererDesc& desc) {
    CORDITE_TRY_ASSIGN(std::unique_ptr<RenderResources> resources,
                       RenderResources::create(device, assets, desc.resources));
    CORDITE_TRY_ASSIGN(ViewBindings viewBindings, ViewBindings::create(device));
    CORDITE_TRY_ASSIGN(SkinPalettes palettes, SkinPalettes::create(device));

    nvrhi::IBindingLayout* viewLayout = viewBindings.layout();
    nvrhi::IBindingLayout* materialLayout = resources->materialLayout();
    nvrhi::IBindingLayout* jointLayout = palettes.layout();
    const auto meshPass = [&](MeshPassDesc passDesc) {
        return MeshPass::create(device, shaders,
                                {.view = viewLayout, .material = materialLayout, .joints = jointLayout},
                                std::move(passDesc));
    };
    CORDITE_TRY_ASSIGN(MeshPass depth, meshPass(depthPassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass base, meshPass(basePassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass forward, meshPass(forwardPassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass wireframe, meshPass(wireframePassDesc()));
    CORDITE_TRY_ASSIGN(MeshPass overdraw, meshPass(overdrawPassDesc()));
    CORDITE_TRY_ASSIGN(DeferredLightingPass lighting, DeferredLightingPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DebugViewPass debugView, DebugViewPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DisplayEncodePass encode, DisplayEncodePass::create(device, shaders));
    CORDITE_TRY_ASSIGN(ShadowAtlas shadows, ShadowAtlas::create(device, shaders, materialLayout, jointLayout));
    CORDITE_TRY_ASSIGN(EnvironmentMaps environments, EnvironmentMaps::create(device, shaders, assets));
    CORDITE_TRY_ASSIGN(DecalPass decals, DecalPass::create(device, shaders, viewLayout, materialLayout));
    CORDITE_TRY_ASSIGN(AmbientOcclusionPass occlusion, AmbientOcclusionPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(LightShaftPass lightShafts, LightShaftPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DepthOfFieldPass depthOfField, DepthOfFieldPass::create(device, shaders, viewLayout));
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
                  .decals = std::move(decals),
                  .occlusion = std::move(occlusion),
                  .lightShafts = std::move(lightShafts),
                  .depthOfField = std::move(depthOfField),
                  .postProcess = std::move(postProcess)};
    return std::unique_ptr<SceneRenderer>(new SceneRenderer(device, std::move(resources), std::move(viewBindings),
                                                            std::move(palettes), std::move(passes),
                                                            desc.supportsWireframe));
}

SceneRenderer::MeshPose SceneRenderer::poseFor(const World& world, Entity entity, const GpuMesh& mesh,
                                               const Uuid& meshId) {
    Entity owner;
    const SkeletonPoseComponent* pose = findSkeletonPose(world, entity, &owner);
    if (pose == nullptr) {
        return {};
    }
    // A vertex must never name a joint the pose has no matrix for: the shader would
    // read past the end of the pose, into the next one or into nothing.
    if (pose->skinning.size() < mesh.jointBounds.size()) {
        if (m_reportedPoses.insert(meshId).second) {
            CORDITE_LOG_WARN("Renderer",
                             "the mesh {} is drawn as it was modelled: its vertices follow {} joints, and the "
                             "skeleton that '{}' is posed with has {}",
                             meshId.toString(), mesh.jointBounds.size(), world.name(owner), pose->skinning.size());
        }
        return {};
    }
    const auto [found, isNew] = m_paletteOfPose.try_emplace(owner, kNoPalette);
    if (isNew) {
        found->second = m_palettes.add(pose->skinning);
        if (found->second != kNoPalette) {
            m_statistics.jointMatrices += static_cast<uint32_t>(pose->skinning.size());
        }
    }
    if (found->second == kNoPalette) {
        return {};
    }
    return {.palette = found->second, .skinning = pose->skinning};
}

void SceneRenderer::collectDraws(const World& world, const CameraView& view) {
    m_sceneMeshes.clear();
    m_sceneDraws.clear();
    m_draws.clear();
    m_palettes.clear();
    m_paletteOfPose.clear();
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
        const MeshPose pose = mesh->isSkinned() ? poseFor(world, Entity{handle}, *mesh, renderer.mesh) : MeshPose();
        if (pose.palette != kNoPalette) {
            ++m_statistics.skinnedMeshes;
        }

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
                                    .lightChannels = renderer.lightChannels,
                                    .palette = pose.palette});
        }
        // Where a pose puts a mesh is not where it was modelled: an arm that is raised
        // is seen, and casts its shadow, where it is now.
        const Aabb inMesh = pose.palette != kNoPalette ? skinnedBounds(mesh->jointBounds, pose.skinning) : mesh->bounds;
        const Aabb bounds = inMesh.transformed(placement.matrix);
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

void SceneRenderer::uploadPoses(nvrhi::ICommandList* commandList) {
    if (m_palettes.upload(commandList)) {
        return;
    }
    // Without the poses on the GPU everything is drawn as it was modelled, which is
    // wrong but shows the scene; the failure has been reported.
    for (std::vector<MeshDraw>* draws : {&m_sceneDraws, &m_draws}) {
        for (MeshDraw& draw : *draws) {
            draw.palette = kNoPalette;
        }
    }
    m_statistics.skinnedMeshes = 0;
    m_statistics.jointMatrices = 0;
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
        m_statistics.shadowPass += m_passes.shadows.draw(commandList, shadow, list.solidBatches(),
                                                         list.instanceBuffer(), m_palettes.bindings());
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
    // The decals, and what they are drawn with, which goes to the GPU with the rest.
    m_decals.collect(world);
    DecalPass::prepare(*m_resources, m_decals.decals());
    // Before anything that uses the new resources is recorded behind them.
    m_resources->flushUploads();
    // The poses, before anything is drawn in them.
    uploadPoses(commandList);

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

    // The look the scene asks for where the view is from: what its picture is finished
    // with, and what its screen effects are like.
    const bool needsLook = isFinishedPicture(settings, debugView) || debugView == RenderDebugView::Focus ||
                           debugView == RenderDebugView::LightShafts ||
                           settings.screenEffectsQuality != ScreenEffectsQuality::Off;
    PostProcessSettings look;
    if (settings.postProcess) {
        look = sanitized(*settings.postProcess);
    } else if (needsLook) {
        look = sanitized(resolvePostProcess(world, {.position = view.position, .camera = settings.viewEntity}));
    }

    // Ambient occlusion keeps light from all around away: where there is none of that,
    // there is nothing to work out - unless it is itself what the view shows.
    const AmbientOcclusionSettings occlusion = ambientOcclusionSettingsFor(look, settings.screenEffectsQuality);
    const bool hasLightFromAround = environment != nullptr && surroundings.intensity > 0.0f;
    const bool hasAmbientLight = m_lights.skyColor() != Vec3(0.0f) || m_lights.groundColor() != Vec3(0.0f);
    const bool isLit = debugView == RenderDebugView::None || debugView == RenderDebugView::LightingOnly;
    const bool drawsOcclusion = occlusion.isEnabled() && (debugView == RenderDebugView::AmbientOcclusion ||
                                                          (isLit && (hasLightFromAround || hasAmbientLight)));

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
         .groundColor = m_lights.groundColor(),
         .ambientOcclusion = drawsOcclusion,
         .fog = m_lights.fog(),
         .backgroundColor = settings.backgroundColor},
        m_clusters,
        {.plan = &m_shadowPlan, .atlas = m_passes.shadows.texture(), .filterRadius = shadowSettings.filterRadius},
        // An environment whose image cannot be used lights nothing.
        {.environment = environment != nullptr ? environment : &m_passes.environments.none(),
         .intensity = environment != nullptr ? surroundings.intensity : 0.0f,
         .rotation = surroundings.rotation,
         .visible = surroundings.visible});
    m_drawList.build(commandList, m_draws, view.position, view.forward);
    nvrhi::IBindingSet* viewBindings = m_viewBindings.bindings();
    const MeshPassBindings meshBindings{
        .view = viewBindings, .instances = m_drawList.instanceBuffer(), .joints = m_palettes.bindings()};

    targets.clear(commandList, settings.backgroundColor);
    if (debugView == RenderDebugView::Wireframe || debugView == RenderDebugView::Overdraw) {
        // What draws every mesh itself shows no G-buffer, and nothing laid onto one.
        targets.releaseDecalNormals();
    }
    if (debugView == RenderDebugView::Wireframe) {
        m_statistics.forwardPass =
            m_passes.wireframe.draw(commandList, targets.sceneFramebuffer(), m_drawList.batches(), meshBindings);
    } else if (debugView == RenderDebugView::Overdraw) {
        // Counted from black, whatever the background is.
        commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        m_statistics.forwardPass =
            m_passes.overdraw.draw(commandList, targets.lightingFramebuffer(), m_drawList.batches(), meshBindings);
    } else {
        m_statistics.depthPass =
            m_passes.depth.draw(commandList, targets.depthFramebuffer(), m_drawList.solidBatches(), meshBindings);
        m_statistics.basePass =
            m_passes.base.draw(commandList, targets.gbufferFramebuffer(), m_drawList.solidBatches(), meshBindings);
        // Laid onto what the base pass left, before anything reads it.
        drawDecals(commandList, targets, viewBindings, view);
        if (!occlusion.isEnabled()) {
            // A view that is to have none gives back what it was worked out in.
            targets.releaseTracedOcclusion();
        }
        if (drawsOcclusion) {
            const uint32_t passes = m_passes.occlusion.draw(commandList, targets, viewBindings, occlusion);
            if (passes == 0) {
                // It could not be drawn, and has said so; the lighting reads it all
                // the same, and is to find nothing in the light's way.
                commandList->clearTextureFloat(targets.ambientOcclusion(), nvrhi::AllSubresources, nvrhi::Color(1.0f));
            }
            m_statistics.screenPasses += passes;
        }
        if (isDrawnFromGBuffer(debugView)) {
            m_passes.debugView.draw(commandList, targets, viewBindings, debugView);
        } else {
            m_passes.lighting.draw(commandList, targets, viewBindings, lightingModeOf(debugView));
        }
        ++m_statistics.screenPasses;
        if (debugView == RenderDebugView::None) {
            m_statistics.forwardPass = m_passes.forward.draw(commandList, targets.sceneFramebuffer(),
                                                             m_drawList.blendedBatches(), meshBindings);
        }
    }
    finishPicture(commandList, targets, viewBindings, view, look, settings, debugView);

    m_statistics.views = 1;
    m_statistics.shadowAtlasBytes = m_passes.shadows.byteSize();
    m_statistics.ambientOcclusionBytes = targets.tracedOcclusionBytes();
    m_statistics.depthOfFieldBytes = targets.depthOfFieldBytes();
    m_statistics.lightShaftBytes = targets.lightShaftBytes();
    m_statistics.decalBytes = targets.decalNormalBytes();
    m_statistics.meshes = m_statistics.shadowPass;
    m_statistics.meshes += m_statistics.depthPass;
    m_statistics.meshes += m_statistics.basePass;
    m_statistics.meshes += m_statistics.forwardPass;
    m_frameStatistics += m_statistics;
}

void SceneRenderer::finishPicture(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                  nvrhi::IBindingSet* viewBindings, const CameraView& view,
                                  const PostProcessSettings& look, const SceneRenderSettings& settings,
                                  RenderDebugView debugView) {
    const bool isFinished = isFinishedPicture(settings, debugView);
    const bool showsFocus = debugView == RenderDebugView::Focus;
    // The light of the sun scattered in the air is part of the light of the scene: the
    // depth of field blurs it as it blurs the rest.
    drawLightShafts(commandList, targets, viewBindings, view, look, isFinished, debugView);
    // Depth of field blurs the light of the scene before it is finished, so that bloom
    // and exposure see the blur - or shows where it does, in a debug view of its own.
    if ((isFinished && hasDepthOfField(look)) || showsFocus) {
        m_statistics.screenPasses +=
            m_passes.depthOfField.draw(commandList, targets, viewBindings, look,
                                       {.quality = settings.postProcessQuality, .showFocus = showsFocus});
    } else {
        // A view that has none gives back what it was worked out in.
        targets.releaseDepthOfField();
    }
    if (isFinished) {
        // A lookup table that is used for the first time goes to the GPU now.
        nvrhi::ITexture* colorLut = m_resources->dataTexture(look.colorLut);
        m_resources->flushUploads();
        m_statistics.screenPasses += m_passes.postProcess.draw(commandList, targets, look,
                                                               {.quality = settings.postProcessQuality,
                                                                .antiAliasing = settings.antiAliasing,
                                                                .deltaSeconds = settings.deltaSeconds,
                                                                .timeSeconds = settings.timeSeconds,
                                                                .colorLut = colorLut});
    } else {
        // A picture that is not finished is not anti-aliased, and needs no room for it.
        targets.releaseAliased();
        m_passes.encode.draw(commandList, targets.sceneColor(), targets.displayFramebuffer());
        ++m_statistics.screenPasses;
    }
}

void SceneRenderer::drawDecals(nvrhi::ICommandList* commandList, SceneTargets& targets,
                               nvrhi::IBindingSet* viewBindings, const CameraView& view) {
    m_statistics.droppedDecals = m_decals.dropped();
    if (m_decals.dropped() > 0 && !m_reportedDroppedDecals) {
        CORDITE_LOG_WARN("Renderer", "too many decals for one view: {} left out (a view holds {})", m_decals.dropped(),
                         kMaxDecals);
        m_reportedDroppedDecals = true;
    }
    const DecalDrawStatistics drawn =
        m_passes.decals.draw(commandList, targets, viewBindings, view, *m_resources, m_decals.decals());
    m_statistics.decals = drawn.decals;
    m_statistics.decalNormalCopies = drawn.normalCopies;
    if (m_statistics.decals == 0) {
        // A view that has none gives back what they read.
        targets.releaseDecalNormals();
    }
}

void SceneRenderer::drawLightShafts(nvrhi::ICommandList* commandList, SceneTargets& targets,
                                    nvrhi::IBindingSet* viewBindings, const CameraView& view,
                                    const PostProcessSettings& look, bool isFinished, RenderDebugView debugView) {
    const bool showsShafts = debugView == RenderDebugView::LightShafts;
    const std::optional<SceneSun>& sun = m_lights.sun();
    uint32_t passes = 0;
    if (sun && ((isFinished && hasLightShafts(look)) || showsShafts)) {
        passes = m_passes.lightShafts.draw(commandList, targets, viewBindings, look,
                                           {.view = view, .sun = *sun, .showShafts = showsShafts});
        m_statistics.screenPasses += passes;
    }
    if (passes == 0) {
        // A view that has none gives back what they were worked out in - and shows none
        // in their debug view.
        targets.releaseLightShafts();
        if (showsShafts) {
            commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                           nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        }
    }
}

SceneRenderStatistics SceneRenderer::takeFrameStatistics() {
    return std::exchange(m_frameStatistics, {});
}

} // namespace cordite
