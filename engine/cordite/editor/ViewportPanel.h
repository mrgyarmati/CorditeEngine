#pragma once

/// @file
/// The editor panel that shows the scene in 3D.
///
/// It renders the world from the editor camera into a target of its own and shows that
/// as an image. Besides the scene itself it draws what only an editor needs: the ground
/// grid, a marker for every entity, the view volumes of cameras, and a highlight around
/// what is selected. Clicking an entity's marker selects it; see EditorCamera for how to
/// move around.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto viewport, ViewportPanel::create(engine));
///
/// viewport->draw(engine, context);                 // once per frame, as a window
///
/// // Or without any interface, into a command list of the caller's:
/// CORDITE_TRY(viewport->render(device, commandList, context, {1920, 1080}));
/// showOrSave(viewport->target().color());
/// @endcode
///
/// Until the engine can load meshes, the markers and the grid are all there is to see.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorCamera.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/Engine.h"
#include "cordite/renderer/DebugDraw.h"
#include "cordite/renderer/DebugLinePass.h"
#include "cordite/renderer/RenderTarget.h"

#include <memory>

namespace cordite {

class ViewportPanel {
public:
    [[nodiscard]] static Result<std::unique_ptr<ViewportPanel>> create(Engine& engine);

    /// Draws the panel as its own window, renders the scene into it and handles the
    /// pointer and the keyboard. Only during a frame.
    void draw(Engine& engine, EditorContext& context);

    /// Renders the scene into target() at the given size in pixels, without any
    /// interface around it. The command list must be open, and the world transforms up
    /// to date. The target is replaced when the size differs from the last call, so
    /// nothing that still refers to its textures may be pending.
    [[nodiscard]] Status render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const EditorContext& context,
                                UVec2 size);

    /// What render() drew into. Its color texture holds display-ready colors.
    [[nodiscard]] const RenderTarget& target() const { return m_target; }

    [[nodiscard]] EditorCamera& camera() { return m_camera; }

    [[nodiscard]] const EditorCamera& camera() const { return m_camera; }

    /// The entity whose marker is nearest to a pixel of the last rendered image, or null
    /// when none is close enough to count as clicked.
    [[nodiscard]] Entity pick(const EditorContext& context, Vec2 pixel) const;

private:
    explicit ViewportPanel(DebugLinePass linePass);

    void collectLines(const EditorContext& context, const CameraView& view);

    DebugLinePass m_linePass;
    RenderTarget m_target;
    EditorCamera m_camera;
    /// The view render() last used, for pick().
    CameraView m_view;
    /// Rebuilt every frame; members so that their memory is reused.
    DebugDraw m_gridLines;
    DebugDraw m_sceneLines;
    /// True while the pointer is locked to the window for steering the camera.
    bool m_pointerCaptured = false;
};

} // namespace cordite
