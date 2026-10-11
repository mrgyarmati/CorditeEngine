#pragma once

/// @file
/// The editor panel that shows the scene in 3D.
///
/// It renders the world from the editor camera into targets of its own and shows the
/// result as an image. Over the scene it draws what only an editor needs: the ground
/// grid, a marker for every entity that has nothing to be seen by, the view volumes of
/// cameras, and a frame around what is selected. Clicking a mesh or a marker selects its
/// entity; see EditorCamera for how to move around.
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
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"

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
    /// nothing that still refers to their textures may be pending.
    [[nodiscard]] Status render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, EditorContext& context,
                                UVec2 size);

    /// What render() drew into. The display image holds the finished picture.
    [[nodiscard]] const SceneTargets& targets() const { return m_targets; }

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
