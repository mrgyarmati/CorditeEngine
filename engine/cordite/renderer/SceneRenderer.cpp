#include "cordite/renderer/SceneRenderer.h"

#include "cordite/core/Assert.h"
#include "cordite/math/Frustum.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

#include <utility>

namespace cordite {

SceneRenderer::SceneRenderer(std::unique_ptr<RenderResources> resources, MeshForwardPass forwardPass,
                             DisplayEncodePass encodePass)
    : m_resources(std::move(resources)), m_forwardPass(std::move(forwardPass)), m_encodePass(std::move(encodePass)) {}

SceneRenderer::~SceneRenderer() = default;

Result<std::unique_ptr<SceneRenderer>> SceneRenderer::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             AssetManager& assets,
                                                             const RenderResourcesDesc& resources) {
    CORDITE_TRY_ASSIGN(std::unique_ptr<RenderResources> renderResources,
                       RenderResources::create(device, assets, resources));
    CORDITE_TRY_ASSIGN(MeshForwardPass forwardPass,
                       MeshForwardPass::create(device, shaders, renderResources->materialLayout()));
    CORDITE_TRY_ASSIGN(DisplayEncodePass encodePass, DisplayEncodePass::create(device, shaders));
    return std::unique_ptr<SceneRenderer>(
        new SceneRenderer(std::move(renderResources), std::move(forwardPass), std::move(encodePass)));
}

void SceneRenderer::render(nvrhi::ICommandList* commandList, const World& world, const CameraView& view,
                           const SceneTargets& targets, const SceneRenderSettings& settings) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid(), "render needs a command list and valid targets");
    m_statistics = {};
    if (commandList == nullptr || !targets.isValid()) {
        return;
    }

    m_resources->beginFrame();
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
    // Before anything that uses the new resources is recorded behind them.
    m_resources->flushUploads();

    MeshPassView passView;
    passView.viewProjection = view.viewProjection;
    passView.cameraPosition = view.position;
    passView.cameraForward = view.forward;
    passView.orthographic = view.orthographic;
    passView.keyLightDirection = settings.keyLightDirection;
    passView.keyLightColor = settings.keyLightColor;
    passView.skyColor = settings.skyColor;
    passView.groundColor = settings.groundColor;

    targets.clear(commandList, settings.backgroundColor);
    m_statistics.meshes = m_forwardPass.draw(commandList, targets.sceneFramebuffer(), m_draws, passView);
    m_encodePass.draw(commandList, targets.sceneColor(), targets.displayFramebuffer());
}

} // namespace cordite
