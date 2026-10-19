#pragma once

/// @file
/// The editor panel that shows the scene in 3D.
///
/// It renders the world from the editor camera into targets of its own and shows the
/// result as an image. Over the scene it draws what only an editor needs: the ground
/// grid, a marker for every entity that has nothing to be seen by, the view volumes of
/// cameras, and a frame around what is selected. Clicking a mesh or a marker selects its
/// entity, and an asset dragged onto the picture from the Assets panel is placed where it
/// is dropped (AssetDrop.h); see EditorCamera for how to move around. It can show any of
/// the renderer's debug views in place of the lit picture (setDebugView).
///
/// Asked to, it shows the scene through the camera of the game instead
/// (ViewportCamera::Game): what a player sees, and nothing an editor adds.
///
/// An editor has up to kMaxViewports of them, each with a number, a window and a camera of
/// its own. What a picture may cost and what the gizmo does are theirs together
/// (ViewportSettings), and so are the pictures of the scene captures, which the first of
/// them that draws in a frame draws for all (ViewportFrame).
///
/// @code
/// ViewportSettings settings;                       // outlives the panels
/// CORDITE_TRY_ASSIGN(auto viewport, ViewportPanel::create(engine, settings));
/// CORDITE_TRY_ASSIGN(auto second, ViewportPanel::create(engine, settings, 2));
///
/// // Once per frame, each as a window:
/// ViewportFrame frame{.clock = WorldClock::Stopped};
/// viewport->draw(engine, context, frame, submittedLines);
/// second->draw(engine, context, frame, submittedLines);
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
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// How many viewports an editor has open at the most. Each is a view of the scene
/// renderer, which has room for so many in a frame (kMaxProgramViews).
inline constexpr uint32_t kMaxViewports = 4;
static_assert(kMaxViewports + 1 <= kMaxProgramViews,
              "the viewports of an editor and the preview of an asset are views of a frame of its program");

/// What every viewport of an editor shares: how much its picture may cost - the renderer
/// has one shadow atlas, which it makes anew whenever its size changes, and one set of
/// textures - and what the gizmo of the selection does.
struct ViewportSettings {
    /// How much the shadows may cost.
    ShadowQuality shadowQuality = ShadowQuality::Medium;
    /// How much the effects that are worked out from the screen may cost: ambient
    /// occlusion and the reflections from the screen. Off draws none.
    ScreenEffectsQuality screenEffectsQuality = ScreenEffectsQuality::Medium;
    /// How much finishing the picture may cost; Off shows the light as the renderer
    /// worked it out.
    PostProcessQuality postProcessQuality = PostProcessQuality::Medium;
    /// How large the error of a level of detail may look, in pixels
    /// (SceneRenderSettings::lodErrorPixels): part of the quality, like the above.
    float lodErrorPixels = 1.0f;
    /// How the edges of the finished picture are smoothed.
    AntiAliasing antiAliasing = AntiAliasing::Fxaa;
    /// What the gizmo of the selection does - move, turn or scale it - and along whose
    /// axes. It is drawn on the entity that was selected last, and moves every selected
    /// entity that does not hang under another selected one.
    GizmoMode gizmoMode = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;
};

/// What the viewports of an editor share in a frame, handed from one ViewportPanel::draw
/// to the next.
struct ViewportFrame {
    /// Whether time passes for the world, as Engine::updateWorld was told (see
    /// ViewportPanel::render()).
    WorldClock clock = WorldClock::Stopped;
    /// Whether one of them has drawn the scene captures and taken the pictures of the probes
    /// this frame: the first that draws its picture does, for all of them. Drawn by each,
    /// they would be drawn as often as there are viewports, and their intervals would pass
    /// as many times as fast.
    bool capturesDrawn = false;
};

/// Whether ViewportPanel::render() draws what the cameras of the scene see into their
/// render targets, and takes the pictures of the probes that wait, before its own picture.
enum class SceneCaptures : uint8_t {
    Draw,
    /// Another view of the frame has drawn them.
    AlreadyDrawn,
};

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

/// The camera of that name (nameOf), or nothing when there is none of it.
[[nodiscard]] std::optional<ViewportCamera> viewportCameraFromName(std::string_view name);

class ViewportPanel {
public:
    /// What the window of the first viewport is known by: its place in a layout is
    /// remembered by it, whatever its tab shows (windowName() in UiWidgets.h). The others
    /// are known by their titles, "###Viewport 2" and on (windowId()).
    static constexpr const char* kWindowId = "###Viewport";

    /// The engine, whose scene renderer the panel draws with, and the settings, which every
    /// viewport of an editor shares and which say what its pictures may cost, must outlive
    /// the panel. `number` is which viewport it is, from 1 to kMaxViewports: what its
    /// window is called and known by.
    [[nodiscard]] static Result<std::unique_ptr<ViewportPanel>> create(Engine& engine, const ViewportSettings& settings,
                                                                       uint32_t number = 1);

    /// Which viewport it is: 1 for the first.
    [[nodiscard]] uint32_t number() const { return m_number; }

    /// What its tab says: "Viewport" for the first, "Viewport 2" and on for the others.
    [[nodiscard]] const std::string& title() const { return m_title; }

    /// What its window is known by: kWindowId for the first, "###Viewport 2" and on for the
    /// others.
    [[nodiscard]] const std::string& windowId() const { return m_windowId; }

    /// Draws the panel as its own window, renders the scene into it and handles the
    /// pointer. Only during a frame. The first viewport of `frame` that draws its picture
    /// draws the scene captures before it, for all of them. A viewport other than the first
    /// has a button that closes its window, which asks for that with a command line added
    /// to `submittedLines`: closing a viewport is the editor's (viewport.close).
    void draw(Engine& engine, EditorContext& context, ViewportFrame& frame, std::vector<std::string>& submittedLines);

    /// Whether its window had the focus of the interface when it was last drawn: it was
    /// clicked into with any button, or brought to the front.
    [[nodiscard]] bool isFocused() const { return m_focused; }

    /// Renders the scene into targets() at the given size in pixels, without any
    /// interface around it. The command list must be open, and the world transforms up
    /// to date. The targets are replaced when the size differs from the last call, so
    /// nothing that still refers to their textures may be pending. `deltaSeconds` is
    /// how long ago the viewport was last drawn - nothing, for a second picture of the
    /// same moment - and `timeSeconds` the time: see SceneRenderSettings. `clock` says
    /// whether time passes for the world: what moves is blurred, through the camera of
    /// the game, only while it does - an edit is no motion. The editor's own camera never
    /// blurs. `captures` says whether what the cameras of the scene see is drawn first, as
    /// a game draws it - with the game's background, whoever's camera the viewport looks
    /// through - which the picture may show.
    [[nodiscard]] Status render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, EditorContext& context,
                                UVec2 size, float deltaSeconds = 0.0f, double timeSeconds = 0.0,
                                WorldClock clock = WorldClock::Stopped, SceneCaptures captures = SceneCaptures::Draw);

    /// What render() drew into. The display image holds the finished picture.
    [[nodiscard]] const SceneTargets& targets() const { return m_targets; }

    /// What the viewport shows instead of the finished picture: a debug view of the
    /// renderer, or RenderDebugView::None for the picture itself.
    [[nodiscard]] RenderDebugView debugView() const { return m_debugView; }

    void setDebugView(RenderDebugView view) { m_debugView = view; }

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

    /// Moves the camera so that what is selected fills the view - its meshes as they are
    /// posed, and where the rest is - keeping the direction it looks in, and makes the middle
    /// of it what the camera orbits. Returns false, and moves nothing, when nothing is
    /// selected. F does this in the viewport that was used last.
    bool frameSelection(EditorContext& context);

    /// Whether a handle of the gizmo is held: a drag is under way.
    [[nodiscard]] bool isDraggingGizmo() const { return m_held.has_value(); }

    /// Lets go of what the viewport holds of the interface, before it is closed: a handle of
    /// the gizmo - what the drag changed becomes one step of the history, where it was let
    /// go of - and the pointer, which steering the camera hides and locks to `window` (null
    /// without one).
    void release(EditorContext& context, Window* window);

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
                  const ViewportSettings& settings, uint32_t number, DebugLinePass linePass);

    void collectLines(EditorContext& context, const CameraView& view);
    /// What every view the viewport draws starts from, whoever's camera it is: the quality
    /// all the viewports share, and the time it is drawn at.
    [[nodiscard]] SceneRenderSettings qualitySettings(float deltaSeconds, double timeSeconds) const;

    /// Finds the handle of the gizmo under the pointer, takes hold of one that is pressed,
    /// and moves, turns or scales what is selected while it is held. Returns whether the
    /// pointer is the gizmo's in this frame - over a handle, holding one or letting one go -
    /// so that a click does not select what is under it. `pixel` is in pixels of the picture.
    bool updateGizmo(EditorContext& context, Vec2 pixel, bool hovered);
    /// Makes the picture, which must be the item submitted last, a place to drop assets
    /// from the Assets panel (AssetDrop.h), and queues what a drop at `pixel` - in pixels
    /// of the picture - does.
    void acceptDrop(EditorContext& context, Vec2 pixel, std::vector<std::string>& submittedLines) const;
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
    /// How the game draws its world: what the game's camera, and the scene captures, are
    /// shown with.
    const SceneRenderSettings* m_gameSettings;
    /// What every viewport of the editor shares.
    const ViewportSettings* m_settings;
    uint32_t m_number;
    std::string m_title;
    /// What its window is called - its icon and its title - and known by.
    std::string m_windowName;
    std::string m_windowId;
    bool m_focused = false;
    DebugLinePass m_linePass;
    SceneTargets m_targets;
    EditorCamera m_camera;
    RenderDebugView m_debugView = RenderDebugView::None;
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
    /// The handle under the pointer in the last frame, which is drawn in the color of what
    /// is selected; and the one that is held.
    GizmoHandle m_hoveredHandle = GizmoHandle::None;
    std::optional<HeldGizmo> m_held;
    /// Drawn over everything, after what is selected.
    DebugDraw m_gizmoLines;
    Vec2 m_imageOrigin{0.0f};
};

} // namespace cordite
