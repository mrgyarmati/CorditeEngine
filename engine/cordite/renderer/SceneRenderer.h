#pragma once

/// @file
/// Renders a world as a camera sees it.
///
/// The scene renderer is what turns entities with a MeshRenderer into a picture: it
/// finds the ones in view, makes sure their meshes and materials are on the GPU, draws
/// them into a SceneTargets and encodes the result for display.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto sceneRenderer, SceneRenderer::create(device, shaders, assets));
/// CORDITE_TRY_ASSIGN(SceneTargets targets, SceneTargets::create(device, {1280, 720}));
///
/// world.updateWorldTransforms();
/// const Entity camera = findPrimaryCamera(world);
/// const CameraView view =
///     makeCameraView(world.worldMatrix(camera), *world.tryGet<CameraComponent>(camera), 16.0f / 9.0f);
/// sceneRenderer->render(commandList, world, view, targets);
/// show(targets.display());
/// @endcode
///
/// One scene renderer serves any number of views; give each view its own SceneTargets.
/// Until the engine has lights, every surface is lit by the fixed key light and the
/// ambient light of SceneRenderSettings.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/DisplayEncodePass.h"
#include "cordite/renderer/MeshForwardPass.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace cordite {

/// How a scene is lit and what is behind it. All colors are linear.
struct SceneRenderSettings {
    /// What shows where nothing is drawn.
    Vec3 backgroundColor{0.012f, 0.014f, 0.018f};

    /// The fixed key light: the direction from a surface towards it, and its color and
    /// strength. The default comes from above, over the right shoulder of a viewer who
    /// looks along -Z, and is slightly warm.
    Vec3 keyLightDirection{0.45f, 0.80f, 0.40f};
    Vec3 keyLightColor{2.75f, 2.60f, 2.40f};

    /// Ambient light arriving from above and from below.
    Vec3 skyColor{0.26f, 0.31f, 0.40f};
    Vec3 groundColor{0.10f, 0.09f, 0.08f};
};

/// What SceneRenderer::render() did: in its last call, or in all the calls of a frame.
struct SceneRenderStatistics {
    /// The calls these numbers cover: one view each.
    uint32_t views = 0;
    /// Enabled entities with a mesh that could be loaded.
    uint32_t meshRenderers = 0;
    /// Of those, the ones outside the view, which were not drawn.
    uint32_t culled = 0;
    MeshPassStatistics meshes;
};

class SceneRenderer {
public:
    /// The device, the shader manager and the asset manager must outlive the renderer.
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                                       AssetManager& assets,
                                                                       const RenderResourcesDesc& resources = {});
    ~SceneRenderer();
    CORDITE_NON_MOVABLE(SceneRenderer);

    /// Draws `world` as seen through `view` into `targets`: scene color and depth, and
    /// from those the display image. The command list must be open, the world
    /// transforms up to date (World::updateWorldTransforms), and `view` made for the
    /// aspect ratio of `targets`.
    ///
    /// Assets seen for the first time are loaded and sent to the GPU here, which makes
    /// the first frame of a scene slower than the ones that follow. One that cannot be
    /// loaded is reported in the log and left out; rendering itself does not fail for it.
    void render(nvrhi::ICommandList* commandList, const World& world, const CameraView& view,
                const SceneTargets& targets, const SceneRenderSettings& settings = {});

    /// What the last call to render() did.
    [[nodiscard]] const SceneRenderStatistics& statistics() const { return m_statistics; }

    /// What every call to render() since the last call to this function did, added up,
    /// and starts counting afresh. The engine takes it once per frame: with several
    /// views, a frame costs what they cost together.
    [[nodiscard]] SceneRenderStatistics takeFrameStatistics();

    /// The GPU copies of the assets drawn so far.
    [[nodiscard]] const RenderResources& resources() const { return *m_resources; }

private:
    SceneRenderer(std::unique_ptr<RenderResources> resources, MeshForwardPass forwardPass,
                  DisplayEncodePass encodePass);

    std::unique_ptr<RenderResources> m_resources;
    MeshForwardPass m_forwardPass;
    DisplayEncodePass m_encodePass;
    SceneRenderStatistics m_statistics;
    SceneRenderStatistics m_frameStatistics;
    /// Rebuilt every call; a member so that its memory is reused.
    std::vector<MeshDraw> m_draws;
};

} // namespace cordite
