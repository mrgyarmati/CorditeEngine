#include "cordite/renderer/SceneRenderer.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Frustum.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

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

} // namespace

SceneRenderStatistics& SceneRenderStatistics::operator+=(const SceneRenderStatistics& other) {
    views += other.views;
    meshRenderers += other.meshRenderers;
    culled += other.culled;
    depthPass += other.depthPass;
    basePass += other.basePass;
    forwardPass += other.forwardPass;
    meshes += other.meshes;
    return *this;
}

SceneRenderer::SceneRenderer(nvrhi::IDevice* device, std::unique_ptr<RenderResources> resources,
                             ViewBindings viewBindings, Passes passes, bool supportsWireframe)
    : m_resources(std::move(resources)),
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

    Passes passes{.depth = std::move(depth),
                  .base = std::move(base),
                  .forward = std::move(forward),
                  .wireframe = std::move(wireframe),
                  .overdraw = std::move(overdraw),
                  .lighting = std::move(lighting),
                  .debugView = std::move(debugView),
                  .encode = std::move(encode)};
    return std::unique_ptr<SceneRenderer>(new SceneRenderer(device, std::move(resources), std::move(viewBindings),
                                                            std::move(passes), desc.supportsWireframe));
}

void SceneRenderer::collectDraws(const World& world, const CameraView& view) {
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
        // turning the camera does not stall on loading what comes into sight.
        const size_t firstDraw = m_draws.size();
        for (uint32_t submesh = 0; submesh < mesh->submeshes.size(); ++submesh) {
            const Uuid material = submesh < renderer.materials.size() ? renderer.materials[submesh] : Uuid();
            m_draws.push_back({mesh, submesh, &m_resources->material(material), placement.matrix});
        }
        if (!frustum.intersects(mesh->bounds.transformed(placement.matrix))) {
            ++m_statistics.culled;
            m_draws.resize(firstDraw);
        }
    }
}

void SceneRenderer::render(nvrhi::ICommandList* commandList, const World& world, const CameraView& view,
                           const SceneTargets& targets, const SceneRenderSettings& settings) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid(), "render needs a command list and valid targets");
    m_statistics = {};
    if (commandList == nullptr || !targets.isValid()) {
        return;
    }

    m_resources->beginFrame();
    collectDraws(world, view);
    // Before anything that uses the new resources is recorded behind them.
    m_resources->flushUploads();

    m_viewBindings.update(commandList, {.camera = view,
                                        .targetSize = targets.size(),
                                        .keyLightDirection = settings.keyLightDirection,
                                        .keyLightColor = settings.keyLightColor,
                                        .skyColor = settings.skyColor,
                                        .groundColor = settings.groundColor});
    m_drawList.build(commandList, m_draws, view.position, view.forward);
    nvrhi::IBindingSet* viewBindings = m_viewBindings.bindings();
    nvrhi::IBuffer* instances = m_drawList.instanceBuffer();

    RenderDebugView debugView = settings.debugView;
    if (debugView == RenderDebugView::Wireframe && !m_supportsWireframe) {
        if (!m_reportedWireframe) {
            CORDITE_LOG_WARN("Renderer", "this adapter cannot draw wireframes; the lit picture is shown instead");
            m_reportedWireframe = true;
        }
        debugView = RenderDebugView::None;
    }

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
        if (showsGBufferChannel(debugView)) {
            m_passes.debugView.draw(commandList, targets, viewBindings, debugView);
        } else {
            m_passes.lighting.draw(commandList, targets, viewBindings, debugView == RenderDebugView::LightingOnly);
        }
        if (debugView == RenderDebugView::None) {
            m_statistics.forwardPass = m_passes.forward.draw(commandList, targets.sceneFramebuffer(),
                                                             m_drawList.blendedBatches(), viewBindings, instances);
        }
    }
    m_passes.encode.draw(commandList, targets.sceneColor(), targets.displayFramebuffer());

    m_statistics.views = 1;
    m_statistics.meshes = m_statistics.depthPass;
    m_statistics.meshes += m_statistics.basePass;
    m_statistics.meshes += m_statistics.forwardPass;
    m_frameStatistics += m_statistics;
}

SceneRenderStatistics SceneRenderer::takeFrameStatistics() {
    return std::exchange(m_frameStatistics, {});
}

} // namespace cordite
