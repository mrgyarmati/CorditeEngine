#pragma once

/// @file
/// Renders a world as a camera sees it.
///
/// The scene renderer is what turns entities with a MeshRenderer into a picture: it
/// finds the ones in view, makes sure their meshes and materials are on the GPU, and
/// takes them through its passes into a SceneTargets.
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
/// The passes of a view, in order:
///
/// | Pass              | Draws                                    | Into                 |
/// |-------------------|------------------------------------------|----------------------|
/// | Depth pre-pass    | Solid surfaces: opaque and masked        | depth                |
/// | Base pass         | The same, where they are the nearest     | the G-buffer         |
/// | Deferred lighting | One triangle over the whole target       | scene color          |
/// | Forward pass      | Translucent surfaces, far to near        | scene color, over it |
/// | Display encoding  | One triangle                             | the display image    |
///
/// A debug view (RenderDebugView) replaces the lighting, or the passes altogether, by a
/// picture of something the renderer works with.
///
/// One scene renderer serves any number of views; give each view its own SceneTargets.
/// A scene is lit by its Light and AmbientLight components (RenderComponents.h): the
/// directional lights everywhere, the point and spot lights where they reach, found
/// through a grid of clusters (LightClusters.h).

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/DebugViewPass.h"
#include "cordite/renderer/DeferredLightingPass.h"
#include "cordite/renderer/DisplayEncodePass.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/MeshPass.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/renderer/ViewBindings.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace cordite {

struct SceneRendererDesc {
    RenderResourcesDesc resources;

    /// Whether the adapter can draw the edges of triangles
    /// (AdapterInfo::supportsWireframe). Where it cannot, the wireframe debug view shows
    /// the lit picture instead, and says so in the log.
    bool supportsWireframe = true;
};

/// What a view shows of a scene besides the scene itself.
struct SceneRenderSettings {
    /// What shows where nothing is drawn. Linear color.
    Vec3 backgroundColor{0.012f, 0.014f, 0.018f};

    /// Light a world that has no light components at all with a key light and soft
    /// ambient light, so that what is in it can be seen (SceneLightsDesc). An editor
    /// does, for a scene that is being started; a game shows what its scene says.
    bool defaultLighting = false;

    /// What to show instead of the finished picture; None shows the picture.
    RenderDebugView debugView = RenderDebugView::None;
};

/// What SceneRenderer::render() did: in its last call, or in all the calls of a frame.
struct SceneRenderStatistics {
    /// The calls these numbers cover: one view each.
    uint32_t views = 0;
    /// Enabled entities with a mesh that could be loaded.
    uint32_t meshRenderers = 0;
    /// Of those, the ones outside the view, which were not drawn.
    uint32_t culled = 0;

    /// The lights that reach something in view: directional ones, and point and spot
    /// lights that touch at least one cluster.
    uint32_t lights = 0;
    /// The entries of all the clusters' lists together: how much the lights overlap.
    uint32_t lightListEntries = 0;
    /// Lights left out because a view can only hold so many (kMaxDirectionalLights,
    /// kMaxLocalLights).
    uint32_t droppedLights = 0;

    /// What each of the passes that draw meshes did. A debug view that draws the meshes
    /// itself - wireframe, overdraw - counts as the forward pass.
    MeshPassStatistics depthPass;
    MeshPassStatistics basePass;
    MeshPassStatistics forwardPass;
    /// The three together: what the GPU was asked to draw.
    MeshPassStatistics meshes;

    SceneRenderStatistics& operator+=(const SceneRenderStatistics& other);
};

class SceneRenderer {
public:
    /// The device, the shader manager and the asset manager must outlive the renderer.
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>>
    create(nvrhi::IDevice* device, ShaderManager& shaders, AssetManager& assets, const SceneRendererDesc& desc = {});
    ~SceneRenderer();
    CORDITE_NON_MOVABLE(SceneRenderer);

    /// Draws `world` as seen through `view` into `targets`: the G-buffer and depth, from
    /// those the scene color, and from that the display image. The command list must be
    /// open, the world transforms up to date (World::updateWorldTransforms), and `view`
    /// made for the aspect ratio of `targets`.
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
    /// The passes of the renderer.
    struct Passes {
        MeshPass depth;
        MeshPass base;
        MeshPass forward;
        MeshPass wireframe;
        MeshPass overdraw;
        DeferredLightingPass lighting;
        DebugViewPass debugView;
        DisplayEncodePass encode;
    };

    SceneRenderer(nvrhi::IDevice* device, std::unique_ptr<RenderResources> resources, ViewBindings viewBindings,
                  Passes passes, bool supportsWireframe);

    /// Fills m_draws with what `world` shows through `view`, and counts.
    void collectDraws(const World& world, const CameraView& view);

    std::unique_ptr<RenderResources> m_resources;
    ViewBindings m_viewBindings;
    MeshDrawList m_drawList;
    Passes m_passes;
    bool m_supportsWireframe;
    /// So that the missing wireframe view is reported once, not every frame.
    bool m_reportedWireframe = false;

    /// The lights of the view being drawn. Members so that their memory is reused.
    SceneLights m_lights;
    LightClusterBuilder m_clusterBuilder;
    LightClusters m_clusters;
    /// So that lights that were left out are reported once, not every frame.
    bool m_reportedDroppedLights = false;

    SceneRenderStatistics m_statistics;
    SceneRenderStatistics m_frameStatistics;
    /// Rebuilt every call; a member so that its memory is reused.
    std::vector<MeshDraw> m_draws;
};

} // namespace cordite
