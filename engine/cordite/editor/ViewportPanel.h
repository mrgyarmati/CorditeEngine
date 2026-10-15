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
/// Asked to, it shows the scene through the camera of the game instead
/// (ViewportCamera::Game): what a player sees, and nothing an editor adds.
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
#include "cordite/editor/ColliderOutlines.h"
#include "cordite/editor/EditorCamera.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/editor/Gizmo.h"
#include "cordite/engine/Engine.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/renderer/DebugDraw.h"
#include "cordite/renderer/DebugLinePass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/renderer/ShadowPlan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace cordite {

/// Through whose eyes the viewport shows the scene.
enum class ViewportCamera : uint8_t {
    /// The editor's own camera, which is flown around the scene, with what only an
    /// editor draws: the grid, the markers, the frames around what is selected.
    Editor,
    /// The camera the game renders from - the first enabled primary camera of the
    /// scene - lit and cleared as the game is, and with nothing drawn over it. Nothing
    /// is picked and nothing is flown. A scene that has no such camera is shown through
    /// the editor's.
    Game,
};

/// The lowercase name: "editor", "game".
std::string_view nameOf(ViewportCamera camera);

class ViewportPanel {
public:
    /// What the window is known by: its place in a layout is remembered by it, whatever its
    /// tab shows (windowName() in UiWidgets.h).
    static constexpr const char* kWindowId = "###Viewport";

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

    /// Through whose eyes the scene is shown. What is asked for, and what the last
    /// render() came to: the game is shown only where the scene has a camera for it.
    [[nodiscard]] ViewportCamera viewCamera() const { return m_viewCamera; }

    void setViewCamera(ViewportCamera camera) { m_viewCamera = camera; }

    [[nodiscard]] bool showsGame() const { return m_showsGame; }

    /// Whether the collider of every entity that has one is drawn, as lines, and its
    /// joint, and the capsule of a character. Those of selected entities, and of what
    /// hangs below them, always are.
    [[nodiscard]] bool showsColliders() const { return m_showColliders; }

    void setShowColliders(bool show) { m_showColliders = show; }

    /// What the gizmo of the selection does - move, turn or scale it - and along whose
    /// axes. It is drawn on the entity that was selected last, and moves every selected
    /// entity that does not hang under another selected one.
    [[nodiscard]] GizmoMode gizmoMode() const { return m_gizmoMode; }

    void setGizmoMode(GizmoMode mode) { m_gizmoMode = mode; }

    [[nodiscard]] GizmoSpace gizmoSpace() const { return m_gizmoSpace; }

    void setGizmoSpace(GizmoSpace space) { m_gizmoSpace = space; }

    /// Whether a handle of the gizmo is held: a drag is under way.
    [[nodiscard]] bool isDraggingGizmo() const { return m_held.has_value(); }

    /// The view the scene was last drawn from, and where the top-left corner of its picture
    /// was on the screen, in interface pixels: for what has to point at something in it.
    [[nodiscard]] const CameraView& view() const { return m_view; }

    [[nodiscard]] Vec2 imageOrigin() const { return m_imageOrigin; }

    [[nodiscard]] EditorCamera& camera() { return m_camera; }

    [[nodiscard]] const EditorCamera& camera() const { return m_camera; }

    /// The entity under a pixel of the last rendered image: the one whose mesh is hit
    /// first by the ray through the pixel, or whose marker is nearest to it when that
    /// marker is in front of every mesh there. Null when there is neither.
    [[nodiscard]] Entity pick(EditorContext& context, Vec2 pixel) const;

private:
    ViewportPanel(SceneRenderer& sceneRenderer, const PhysicsSystem& physics, const SceneRenderSettings& gameSettings,
                  DebugLinePass linePass);

    void collectLines(EditorContext& context, const CameraView& view);

    /// Finds the handle of the gizmo under the pointer, takes hold of one that is pressed,
    /// and moves, turns or scales what is selected while it is held. Returns whether the
    /// pointer is the gizmo's in this frame - over a handle, holding one or letting one go -
    /// so that a click does not select what is under it. `pixel` is in pixels of the picture.
    bool updateGizmo(EditorContext& context, Vec2 pixel, bool hovered);
    /// Moves, turns or scales what is held to where the pointer is now.
    void dragHeld(World& world, Vec2 pixel);
    /// Ends a drag: what it changed becomes one step of the history.
    void releaseGizmo(EditorContext& context);
    /// How large the gizmo is drawn: the scale of the interface, in pixels of the picture.
    [[nodiscard]] static float gizmoPixelScale();

    /// An entity that a held gizmo moves, as it was when the handle was taken hold of.
    struct HeldEntity {
        Uuid id;
        Transform start;
        Mat4 startWorld{1.0f};
        /// Where what it hangs under is, which a drag does not move.
        Mat4 parentWorld{1.0f};
        /// Its transform component, for the history.
        Json before;
    };

    struct HeldGizmo {
        GizmoDrag drag;
        std::vector<HeldEntity> entities;
    };

    SceneRenderer* m_sceneRenderer;
    /// What the colliders that are made of meshes are made of.
    const PhysicsSystem* m_physics;
    ColliderOutlines m_colliderOutlines;
    /// How the game draws its world: what the game's camera is shown with.
    const SceneRenderSettings* m_gameSettings;
    DebugLinePass m_linePass;
    SceneTargets m_targets;
    EditorCamera m_camera;
    RenderDebugView m_debugView = RenderDebugView::None;
    ShadowQuality m_shadowQuality = ShadowQuality::Medium;
    ScreenEffectsQuality m_screenEffectsQuality = ScreenEffectsQuality::Medium;
    PostProcessQuality m_postProcessQuality = PostProcessQuality::Medium;
    AntiAliasing m_antiAliasing = AntiAliasing::Fxaa;
    bool m_showColliders = false;
    ViewportCamera m_viewCamera = ViewportCamera::Editor;
    /// Whether the last render() was through the camera of the game.
    bool m_showsGame = false;
    /// The view render() last used, for pick().
    CameraView m_view;
    /// Rebuilt every frame; members so that their memory is reused. Grid and scene
    /// lines are hidden by what is in front of them; selection lines never are.
    DebugDraw m_gridLines;
    DebugDraw m_sceneLines;
    DebugDraw m_selectionLines;
    /// True while the pointer is locked to the window for steering the camera.
    bool m_pointerCaptured = false;
    GizmoMode m_gizmoMode = GizmoMode::Translate;
    GizmoSpace m_gizmoSpace = GizmoSpace::World;
    /// The handle under the pointer in the last frame, which is drawn in the color of what
    /// is selected; and the one that is held.
    GizmoHandle m_hoveredHandle = GizmoHandle::None;
    std::optional<HeldGizmo> m_held;
    /// Drawn over everything, after what is selected.
    DebugDraw m_gizmoLines;
    Vec2 m_imageOrigin{0.0f};
};

} // namespace cordite
