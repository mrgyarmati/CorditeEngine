#pragma once

/// @file
/// The editor panel that shows the scene in 3D.
///
/// It renders the world from the editor camera into targets of its own and shows the
/// result as an image. Over the scene it draws what only an editor needs: the ground
/// grid, a marker for every entity that has nothing to be seen by, the view volumes of
/// cameras, and a frame around what is selected. Clicking a mesh or a marker selects its
/// entity; see EditorCamera for how to move around. It can show any of the renderer's
/// debug views in place of the lit picture (setDebugView).
///
/// @code
/// CORDITE_TRY_ASSIGN(auto viewport, ViewportPanel::create(engine));
///
/// viewport->draw(engine, context);                 // once per frame, as a window
///
/// // Or without any interface, into a command list of the caller's:
/// CORDITE_TRY(viewport->render(device, commandList, context, {1920, 1080}));
/// showOrSave(viewport->targets().display());
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorCamera.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/Engine.h"
#include "cordite/renderer/DebugDraw.h"
#include "cordite/renderer/DebugLinePass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/renderer/ShadowPlan.h"

#include <memory>

namespace cordite {

class ViewportPanel {
public:
    /// The engine, whose scene renderer the panel draws with, must outlive the panel.
    [[nodiscard]] static Result<std::unique_ptr<ViewportPanel>> create(Engine& engine);

    /// Draws the panel as its own window, renders the scene into it and handles the
    /// pointer and the keyboard. Only during a frame.
    void draw(Engine& engine, EditorContext& context);

    /// Renders the scene into targets() at the given size in pixels, without any
    /// interface around it. The command list must be open, and the world transforms up
    /// to date. The targets are replaced when the size differs from the last call, so
    /// nothing that still refers to their textures may be pending. `deltaSeconds` is
    /// how long ago the viewport was last drawn - nothing, for a second picture of the
    /// same moment - and `timeSeconds` the time: see SceneRenderSettings.
    [[nodiscard]] Status render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, EditorContext& context,
                                UVec2 size, float deltaSeconds = 0.0f, double timeSeconds = 0.0);

    /// What render() drew into. The display image holds the finished picture.
    [[nodiscard]] const SceneTargets& targets() const { return m_targets; }

    /// What the viewport shows instead of the finished picture: a debug view of the
    /// renderer, or RenderDebugView::None for the picture itself.
    [[nodiscard]] RenderDebugView debugView() const { return m_debugView; }

    void setDebugView(RenderDebugView view) { m_debugView = view; }

    /// How much the shadows the viewport shows may cost.
    [[nodiscard]] ShadowQuality shadowQuality() const { return m_shadowQuality; }

    void setShadowQuality(ShadowQuality quality) { m_shadowQuality = quality; }

    /// How much the effects that are worked out from the screen may cost in the
    /// viewport: ambient occlusion, so far. Off draws none.
    [[nodiscard]] ScreenEffectsQuality screenEffectsQuality() const { return m_screenEffectsQuality; }

    void setScreenEffectsQuality(ScreenEffectsQuality quality) { m_screenEffectsQuality = quality; }

    /// How much finishing the picture the viewport shows may cost; Off shows the light
    /// as the renderer worked it out.
    [[nodiscard]] PostProcessQuality postProcessQuality() const { return m_postProcessQuality; }

    void setPostProcessQuality(PostProcessQuality quality) { m_postProcessQuality = quality; }

    /// How the edges of the finished picture are smoothed in the viewport.
    [[nodiscard]] AntiAliasing antiAliasing() const { return m_antiAliasing; }

    void setAntiAliasing(AntiAliasing antiAliasing) { m_antiAliasing = antiAliasing; }

    /// Whether the collider of every entity that has one is drawn, as lines. Those of
    /// selected entities, and of what hangs below them, always are.
    [[nodiscard]] bool showsColliders() const { return m_showColliders; }

    void setShowColliders(bool show) { m_showColliders = show; }

    [[nodiscard]] EditorCamera& camera() { return m_camera; }

    [[nodiscard]] const EditorCamera& camera() const { return m_camera; }

    /// The entity under a pixel of the last rendered image: the one whose mesh is hit
    /// first by the ray through the pixel, or whose marker is nearest to it when that
    /// marker is in front of every mesh there. Null when there is neither.
    [[nodiscard]] Entity pick(EditorContext& context, Vec2 pixel) const;

private:
    ViewportPanel(SceneRenderer& sceneRenderer, DebugLinePass linePass);

    void collectLines(EditorContext& context, const CameraView& view);

    SceneRenderer* m_sceneRenderer;
    DebugLinePass m_linePass;
    SceneTargets m_targets;
    EditorCamera m_camera;
    RenderDebugView m_debugView = RenderDebugView::None;
    ShadowQuality m_shadowQuality = ShadowQuality::Medium;
    ScreenEffectsQuality m_screenEffectsQuality = ScreenEffectsQuality::Medium;
    PostProcessQuality m_postProcessQuality = PostProcessQuality::Medium;
    AntiAliasing m_antiAliasing = AntiAliasing::Fxaa;
    bool m_showColliders = false;
    /// The view render() last used, for pick().
    CameraView m_view;
    /// Rebuilt every frame; members so that their memory is reused. Grid and scene
    /// lines are hidden by what is in front of them; selection lines never are.
    DebugDraw m_gridLines;
    DebugDraw m_sceneLines;
    DebugDraw m_selectionLines;
    /// True while the pointer is locked to the window for steering the camera.
    bool m_pointerCaptured = false;
};

} // namespace cordite
