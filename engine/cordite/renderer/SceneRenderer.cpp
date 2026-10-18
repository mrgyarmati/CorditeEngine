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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

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
    // The same code as the base pass, whose depth test asks for these positions exactly.
    desc.motion = true;
    return desc;
}

/// Fills the G-buffer, only where a surface is the one the depth pre-pass left in front.
MeshPassDesc basePassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Base pass";
    desc.pixelShader = pixelShader("passes/MeshGBuffer.slang", "meshGBufferPixel");
    desc.depth = MeshPassDepth::Equal;
    // It writes the motion vectors, from where each vertex was and is.
    desc.motion = true;
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

/// Each mesh in the color of the level of detail it is drawn at.
MeshPassDesc lodPassDesc() {
    MeshPassDesc desc;
    desc.debugName = "Levels of detail view";
    desc.pixelShader = pixelShader("passes/MeshDebug.slang", "meshLodPixel");
    desc.depth = MeshPassDepth::TestAndWrite;
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
    if (debugView == RenderDebugView::FogLight) {
        return DeferredLightingMode::FogLight;
    }
    return DeferredLightingMode::Lit;
}

/// Whether a view shows the light the lights give - the lit picture, the light alone, or
/// what they throw into the fog - and so needs their shadow maps.
bool showsLightOfLights(RenderDebugView debugView) {
    return debugView == RenderDebugView::None || debugView == RenderDebugView::LightingOnly ||
           debugView == RenderDebugView::FogLight;
}

/// Whether any of the lights of a view gives the fog something.
bool anyLightLightsFog(const LightClusters& clusters) {
    return std::ranges::any_of(
        clusters.lights, [](const GpuLight& light) { return glm::any(glm::greaterThan(light.fogColor, Vec3(0.0f))); });
}

/// Where the entity `id` placed its mesh in the picture `before` - what a view remembers
/// of its last one - or none: an entity that has no past did not move. A placement that
/// is not made of numbers is none, too: what moves from nowhere has no motion to tell.
std::optional<Mat4> placementBefore(const MotionHistory* before, const Uuid& id) {
    const Mat4* found = before != nullptr ? before->placementOf(id) : nullptr;
    if (found == nullptr) {
        return std::nullopt;
    }
    const Mat4& placement = *found;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(placement[column][row])) {
                return std::nullopt;
            }
        }
    }
    return placement;
}

/// What the targets of a view remember of its last picture, where `view` drawn with
/// `settings` goes on from it; null where nothing in the picture can have moved - where it
/// does not go on from one before (MotionVectors.h), and where no time has passed since,
/// which makes a change a jump, not a motion: an edit, a second picture of one moment.
const MotionHistory* historyToGoOn(const SceneTargets& targets, const CameraView& view,
                                   const SceneRenderSettings& settings) {
    const MotionHistory& history = targets.motionHistory();
    return settings.deltaSeconds > 0.0f && history.continuesWith(view, settings.viewEntity) ? &history : nullptr;
}

/// The world to clip space of the picture `before`, or none for a picture that goes on
/// from none.
std::optional<Mat4> viewProjectionBefore(const MotionHistory* before) {
    return before != nullptr ? std::optional<Mat4>(before->view.viewProjection) : std::nullopt;
}

/// The joint matrices of the pose of the entity `owner` in the picture `before`, where they
/// are another pose than `skinning` with as many joints - a skeleton that was replaced in
/// between has no motion to tell of - or null.
const std::vector<Mat4>* changedPoseOf(const MotionHistory* before, const Uuid& owner, std::span<const Mat4> skinning) {
    const std::vector<Mat4>* found = before != nullptr ? before->poseOf(owner) : nullptr;
    if (found == nullptr || found->size() != skinning.size() || std::ranges::equal(*found, skinning)) {
        return nullptr;
    }
    return found;
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
            settings.lodErrorPixels = 2.0f;
            break;
        case RenderQuality::Medium:
            settings.shadowQuality = ShadowQuality::Medium;
            settings.screenEffectsQuality = ScreenEffectsQuality::Medium;
            settings.postProcessQuality = PostProcessQuality::Medium;
            settings.lodErrorPixels = 1.0f;
            break;
        case RenderQuality::High:
            settings.shadowQuality = ShadowQuality::High;
            settings.screenEffectsQuality = ScreenEffectsQuality::High;
            settings.postProcessQuality = PostProcessQuality::High;
            settings.lodErrorPixels = 0.5f;
            break;
    }
}

std::optional<RenderQuality> renderQualityOf(const SceneRenderSettings& settings) {
    for (const RenderQuality quality : kRenderQualities) {
        SceneRenderSettings preset;
        applyRenderQuality(quality, preset);
        if (preset.shadowQuality == settings.shadowQuality &&
            preset.screenEffectsQuality == settings.screenEffectsQuality &&
            preset.postProcessQuality == settings.postProcessQuality &&
            preset.lodErrorPixels == settings.lodErrorPixels) {
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
    captures += other.captures;
    droppedCaptures += other.droppedCaptures;
    droppedDecals += other.droppedDecals;
    decalNormalCopies += other.decalNormalCopies;
    for (size_t level = 0; level < lodInstances.size(); ++level) {
        lodInstances[level] += other.lodInstances[level];
    }
    decalBytes = std::max(decalBytes, other.decalBytes);
    shadowMaps += other.shadowMaps;
    lightsWithoutShadows += other.lightsWithoutShadows;
    shadowAtlasBytes = std::max(shadowAtlasBytes, other.shadowAtlasBytes);
    environmentsMade += other.environmentsMade;
    environmentBytes = std::max(environmentBytes, other.environmentBytes);
    ambientOcclusionBytes = std::max(ambientOcclusionBytes, other.ambientOcclusionBytes);
    depthOfFieldBytes = std::max(depthOfFieldBytes, other.depthOfFieldBytes);
    lightShaftBytes = std::max(lightShaftBytes, other.lightShaftBytes);
    fogVolumeBytes = std::max(fogVolumeBytes, other.fogVolumeBytes);
    motionBlurBytes = std::max(motionBlurBytes, other.motionBlurBytes);
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
    CORDITE_TRY_ASSIGN(MeshPass lod, meshPass(lodPassDesc()));
    CORDITE_TRY_ASSIGN(DeferredLightingPass lighting, DeferredLightingPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DebugViewPass debugView, DebugViewPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DisplayEncodePass encode, DisplayEncodePass::create(device, shaders));
    CORDITE_TRY_ASSIGN(ShadowAtlas shadows, ShadowAtlas::create(device, shaders, materialLayout, jointLayout));
    CORDITE_TRY_ASSIGN(EnvironmentMaps environments, EnvironmentMaps::create(device, shaders, assets));
    CORDITE_TRY_ASSIGN(DecalPass decals, DecalPass::create(device, shaders, viewLayout, materialLayout));
    CORDITE_TRY_ASSIGN(AmbientOcclusionPass occlusion, AmbientOcclusionPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(VolumetricFogPass fogLight, VolumetricFogPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(LightShaftPass lightShafts, LightShaftPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(DepthOfFieldPass depthOfField, DepthOfFieldPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(MotionBlurPass motionBlur, MotionBlurPass::create(device, shaders, viewLayout));
    CORDITE_TRY_ASSIGN(PostProcessPass postProcess, PostProcessPass::create(device, shaders));

    Passes passes{.depth = std::move(depth),
                  .base = std::move(base),
                  .forward = std::move(forward),
                  .wireframe = std::move(wireframe),
                  .overdraw = std::move(overdraw),
                  .lod = std::move(lod),
                  .lighting = std::move(lighting),
                  .debugView = std::move(debugView),
                  .encode = std::move(encode),
                  .shadows = std::move(shadows),
                  .environments = std::move(environments),
                  .decals = std::move(decals),
                  .occlusion = std::move(occlusion),
                  .fogLight = std::move(fogLight),
                  .lightShafts = std::move(lightShafts),
                  .depthOfField = std::move(depthOfField),
                  .motionBlur = std::move(motionBlur),
                  .postProcess = std::move(postProcess)};
    return std::unique_ptr<SceneRenderer>(new SceneRenderer(device, std::move(resources), std::move(viewBindings),
                                                            std::move(palettes), std::move(passes),
                                                            desc.supportsWireframe));
}

SceneRenderer::MeshPose SceneRenderer::poseFor(const World& world, Entity entity, const GpuMesh& mesh,
                                               const Uuid& meshId, const MotionHistory* before) {
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
    const auto [found, isNew] = m_paletteOfPose.try_emplace(owner);
    PosePalettes& palettes = found->second;
    if (isNew) {
        palettes.current = m_palettes.add(pose->skinning);
        if (palettes.current != kNoPalette) {
            m_statistics.jointMatrices += static_cast<uint32_t>(pose->skinning.size());
            const Uuid ownerId = world.id(owner);
            m_poses.emplace_back(ownerId, std::vector<Mat4>(pose->skinning.begin(), pose->skinning.end()));
            if (const std::vector<Mat4>* was = changedPoseOf(before, ownerId, pose->skinning)) {
                palettes.previous = m_palettes.add(*was);
            }
        }
    }
    if (palettes.current == kNoPalette) {
        return {};
    }
    return {.palette = palettes.current, .previousPalette = palettes.previous, .skinning = pose->skinning};
}

void SceneRenderer::collectDraws(const World& world, const CameraView& view, const LodView& lodView,
                                 const MotionHistory* before) {
    m_sceneMeshes.clear();
    m_sceneDraws.clear();
    m_draws.clear();
    m_palettes.clear();
    m_paletteOfPose.clear();
    m_placements.clear();
    m_poses.clear();
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
        // Where it is, for the next picture to tell how far it went; and where it was.
        const Uuid id = world.id(Entity{handle});
        m_placements.emplace_back(id, placement.matrix);
        const std::optional<Mat4> previousToWorld = placementBefore(before, id);
        const MeshPose pose =
            mesh->isSkinned() ? poseFor(world, Entity{handle}, *mesh, renderer.mesh, before) : MeshPose();
        if (pose.palette != kNoPalette) {
            ++m_statistics.skinnedMeshes;
        }

        // Where a pose puts a mesh is not where it was modelled: an arm that is raised
        // is seen, and casts its shadow, where it is now.
        const Aabb inMesh = pose.palette != kNoPalette ? skinnedBounds(mesh->jointBounds, pose.skinning) : mesh->bounds;
        const Aabb bounds = inMesh.transformed(placement.matrix);
        // One level of detail for all of the mesh, as this view sees it - also for its
        // shadows: a mesh casts the shadow of what is drawn.
        const uint32_t lod = selectLod(mesh->lodErrors, bounds, scaleOf(placement.matrix), lodView);

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
                                    .palette = pose.palette,
                                    .lod = lod,
                                    .previousToWorld = previousToWorld,
                                    .previousPalette = pose.previousPalette});
        }
        m_sceneMeshes.push_back({.bounds = bounds,
                                 .firstDraw = static_cast<uint32_t>(firstDraw),
                                 .drawCount = static_cast<uint32_t>(m_sceneDraws.size() - firstDraw),
                                 .lightChannels = renderer.lightChannels & kAllLightChannels,
                                 .castsShadows = renderer.castsShadows});
        if (frustum.intersects(bounds)) {
            m_draws.insert(m_draws.end(), m_sceneDraws.begin() + static_cast<std::ptrdiff_t>(firstDraw),
                           m_sceneDraws.end());
            ++m_statistics.lodInstances[std::min<size_t>(lod, kMaxMeshLods)];
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
            draw.previousPalette = kNoPalette;
        }
    }
    m_statistics.skinnedMeshes = 0;
    m_statistics.jointMatrices = 0;
}

FogVolumeGrid SceneRenderer::prepareFogLight(SceneTargets& targets, const SceneRenderSettings& settings,
                                             RenderDebugView debugView) {
    const FogVolumeGrid grid = fogVolumeGridFor(targets.size(), settings.screenEffectsQuality);
    if (!showsLightOfLights(debugView) || grid.isEmpty() || !m_lights.fog().isLitByLights() ||
        !anyLightLightsFog(m_clusters)) {
        // A view that has none gives back what it was worked out in.
        targets.releaseFogVolume();
        return {};
    }
    if (const Status prepared = targets.prepareFogVolume(m_device, UVec3(grid.columns, grid.rows, grid.slices));
        !prepared) {
        if (!m_reportedFogLight) {
            CORDITE_LOG_ERROR("Renderer", "the fog is lit by no light: {}", prepared.error().describe());
            m_reportedFogLight = true;
        }
        return {};
    }
    return grid;
}

void SceneRenderer::assignCookies() {
    for (size_t index = 0; index < m_clusters.lights.size(); ++index) {
        GpuLight& light = m_clusters.lights[index];
        light.cookie = light.cookieRight == Vec3(0.0f)
                           ? -1
                           : m_resources->cookieLayer(m_lights.lights()[m_clusters.sources[index]].cookie);
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
    const MotionHistory* before = historyToGoOn(targets, view, settings);
    collectDraws(world, view, lodViewOf(view, targets.size().y, settings.lodErrorPixels), before);
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
    // Their cookies, put into the cookie array by uploads that are sent below, before
    // anything is drawn.
    assignCookies();
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
    if (showsLightOfLights(debugView)) {
        drawShadows(commandList, shadowSettings);
    }
    // What lights throw into the fog is worked out over a grid of the view, before
    // anything is lit, in volumes of its targets that the view's bindings hold.
    const FogVolumeGrid fogGrid = prepareFogLight(targets, settings, debugView);

    m_viewBindings.update(
        commandList,
        {.camera = view,
         .previousViewProjection = viewProjectionBefore(before),
         .targetSize = targets.size(),
         .skyColor = m_lights.skyColor(),
         .groundColor = m_lights.groundColor(),
         .ambientOcclusion = drawsOcclusion,
         .fog = m_lights.fog(),
         .fogVolume = fogGrid,
         .backgroundColor = settings.backgroundColor},
        m_clusters, m_resources->cookies(),
        {.plan = &m_shadowPlan, .atlas = m_passes.shadows.texture(), .filterRadius = shadowSettings.filterRadius},
        // An environment whose image cannot be used lights nothing.
        {.environment = environment != nullptr ? environment : &m_passes.environments.none(),
         .intensity = environment != nullptr ? surroundings.intensity : 0.0f,
         .rotation = surroundings.rotation,
         .visible = surroundings.visible},
        fogGrid.isEmpty() ? nullptr : targets.fogVolume());
    m_drawList.build(commandList, m_draws, view.position, view.forward);
    nvrhi::IBindingSet* viewBindings = m_viewBindings.bindings();
    const MeshPassBindings meshBindings{
        .view = viewBindings, .instances = m_drawList.instanceBuffer(), .joints = m_palettes.bindings()};

    targets.clear(commandList, settings.backgroundColor);
    if (debugView == RenderDebugView::Wireframe || debugView == RenderDebugView::Overdraw ||
        debugView == RenderDebugView::Lod) {
        // What draws every mesh itself shows no G-buffer, and nothing laid onto one.
        targets.releaseDecalNormals();
    }
    if (debugView == RenderDebugView::Wireframe) {
        m_statistics.forwardPass =
            m_passes.wireframe.draw(commandList, targets.sceneFramebuffer(), m_drawList.batches(), meshBindings);
    } else if (debugView == RenderDebugView::Overdraw) {
        // Counted from black, whatever the background is. Into scene color and depth,
        // though no depth is tested: what the command list draws into, it keeps alive.
        commandList->clearTextureFloat(targets.sceneColor(), nvrhi::AllSubresources,
                                       nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
        m_statistics.forwardPass =
            m_passes.overdraw.draw(commandList, targets.sceneFramebuffer(), m_drawList.batches(), meshBindings);
    } else if (debugView == RenderDebugView::Lod) {
        m_statistics.forwardPass =
            m_passes.lod.draw(commandList, targets.sceneFramebuffer(), m_drawList.batches(), meshBindings);
    } else {
        // Only here: a view that draws no G-buffer must not clear it either (clear()).
        targets.clearGBuffer(commandList);
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
        // Nothing, unless prepareFogLight() made the targets volumes for it.
        m_statistics.screenPasses +=
            m_passes.fogLight.draw(commandList, targets, viewBindings, m_viewBindings.constants());
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
    finishPicture(commandList, targets, viewBindings, view, look, settings, debugView, before != nullptr);

    // What the next picture of the view goes on from. The lists change places, so that
    // their memory is used again.
    MotionHistory& history = targets.motionHistory();
    std::swap(history.placements, m_placements);
    std::swap(history.poses, m_poses);
    history.remember(view, settings.viewEntity);

    m_statistics.views = 1;
    m_statistics.shadowAtlasBytes = m_passes.shadows.byteSize();
    m_statistics.ambientOcclusionBytes = targets.tracedOcclusionBytes();
    m_statistics.depthOfFieldBytes = targets.depthOfFieldBytes();
    m_statistics.lightShaftBytes = targets.lightShaftBytes();
    m_statistics.fogVolumeBytes = targets.fogVolumeBytes();
    m_statistics.motionBlurBytes = targets.motionBlurBytes();
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
                                  RenderDebugView debugView, bool continues) {
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
    // Motion blur smears what moved, after the lens has blurred what is out of focus and
    // before the rest, so that bloom and exposure see the blur. In a picture that goes
    // on from none - the first of a view, the first after a cut - nothing moved.
    if (isFinished && settings.motionBlur && hasMotionBlur(look)) {
        if (continues) {
            m_statistics.screenPasses += m_passes.motionBlur.draw(commandList, targets, viewBindings, look,
                                                                  {.quality = settings.postProcessQuality});
        }
    } else {
        // A view that has none gives back what it was worked out in.
        targets.releaseMotionBlur();
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

void SceneRenderer::renderCaptures(nvrhi::ICommandList* commandList, const World& world,
                                   const SceneRenderSettings& settings) {
    CORDITE_ASSERT(commandList != nullptr, "renderCaptures needs a command list");
    if (commandList == nullptr) {
        return;
    }
    ++m_captureCalls;
    // Before a target is asked for: the resources of this frame.
    m_resources->beginFrame();

    // By the ids of their entities: the same captures are left out on every run.
    std::vector<std::pair<Uuid, Entity>> captures;
    for (const auto handle : world.registry().view<const SceneCaptureComponent>()) {
        const Entity entity{handle};
        if (world.isEnabledInHierarchy(entity)) {
            captures.emplace_back(world.id(entity), entity);
        }
    }
    std::ranges::sort(captures, {}, &std::pair<Uuid, Entity>::first);
    if (captures.size() > kMaxSceneCaptures) {
        const auto dropped = static_cast<uint32_t>(captures.size() - kMaxSceneCaptures);
        m_frameStatistics.droppedCaptures += dropped;
        if (!m_reportedDroppedCaptures) {
            CORDITE_LOG_WARN("Renderer", "too many scene captures: {} draw nothing (a frame draws {})", dropped,
                             kMaxSceneCaptures);
            m_reportedDroppedCaptures = true;
        }
        captures.resize(kMaxSceneCaptures);
    }

    const float passed =
        std::isfinite(settings.deltaSeconds) && settings.deltaSeconds > 0.0f ? settings.deltaSeconds : 0.0f;
    for (const auto& [id, entity] : captures) {
        const SceneCaptureComponent& capture = *world.tryGet<SceneCaptureComponent>(entity);
        const auto* camera = world.tryGet<CameraComponent>(entity);
        if (camera == nullptr) {
            if (m_reportedCaptures.insert(id).second) {
                CORDITE_LOG_WARN("Renderer", "the scene capture of '{}' draws nothing: its entity has no Camera",
                                 world.name(entity));
            }
            continue;
        }
        // One that names nothing draws nothing; one that cannot be used is reported by the
        // resources.
        nvrhi::ITexture* target = m_resources->renderTarget(capture.target);
        if (target == nullptr) {
            continue;
        }
        const UVec2 size(target->getDesc().width, target->getDesc().height);
        CaptureView& state = m_captureViews[id];
        state.seen = m_captureCalls;
        state.sinceLastPicture += passed;
        const float interval = std::isfinite(capture.interval) ? std::max(capture.interval, 0.0f) : 0.0f;
        if (state.hasPicture && state.sinceLastPicture < interval) {
            continue;
        }
        Status prepared;
        if (state.targets.isValid()) {
            prepared = state.targets.resize(m_device, size);
        } else if (Result<SceneTargets> made = SceneTargets::create(m_device, size, "Capture")) {
            state.targets = std::move(*made);
        } else {
            prepared = tl::unexpected(made.error());
        }
        if (!prepared) {
            if (m_reportedCaptures.insert(id).second) {
                CORDITE_LOG_ERROR("Renderer", "the scene capture of '{}' draws nothing: {}", world.name(entity),
                                  prepared.error().message);
            }
            continue;
        }

        // A game's view of the world, whatever the view that shows it is looking at.
        SceneRenderSettings captured = settings;
        captured.debugView = RenderDebugView::None;
        captured.defaultLighting = false;
        captured.motionBlur = true;
        captured.viewEntity = id;
        captured.deltaSeconds = state.hasPicture ? state.sinceLastPicture : passed;
        // Shaken where its camera shakes, as a game's view is.
        const CameraView view = makeCameraView(world, entity, static_cast<float>(size.x) / static_cast<float>(size.y));
        render(commandList, world, view, state.targets, captured);
        // Into the target only now that the picture is done: a capture that sees its own
        // target sees the picture before.
        commandList->copyTexture(target, nvrhi::TextureSlice(), state.targets.display(), nvrhi::TextureSlice());
        state.sinceLastPicture = 0.0f;
        state.hasPicture = true;
        ++m_frameStatistics.captures;
    }
    // A capture that is gone, switched off or left out lets go of its targets.
    std::erase_if(m_captureViews, [this](const auto& entry) { return entry.second.seen != m_captureCalls; });
}

SceneRenderStatistics SceneRenderer::takeFrameStatistics() {
    return std::exchange(m_frameStatistics, {});
}

} // namespace cordite
