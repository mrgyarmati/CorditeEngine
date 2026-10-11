#include "cordite/editor/ViewportPanel.h"

#include "cordite/editor/EditorFeedback.h"
#include "cordite/world/Components.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace cordite {
namespace {

constexpr Vec4 kBackgroundColor(0.085f, 0.090f, 0.105f, 1.0f);
constexpr Vec4 kSelectionColor(1.0f, 0.78f, 0.20f, 1.0f);
constexpr Vec4 kCameraColor(0.75f, 0.78f, 0.85f, 1.0f);

/// The ground grid follows the camera and fades out before its edge comes into view.
/// Its lines are faint: far away they crowd together, and at full strength they would
/// add up to a bright band along the horizon.
constexpr float kGridCellSize = 1.0f;
constexpr uint32_t kGridHalfCellCount = 60;
constexpr float kGridFadeStart = 10.0f;
constexpr float kGridFadeEnd = 50.0f;
constexpr Vec4 kGridColor(0.42f, 0.44f, 0.50f, 0.35f);
constexpr Vec4 kGridMajorColor(0.55f, 0.57f, 0.64f, 0.75f);

/// The world's X and Z axes are drawn on the grid, as far as anyone can see.
constexpr float kWorldAxisLength = 10000.0f;
constexpr Vec4 kWorldXAxisColor(0.80f, 0.27f, 0.27f, 0.9f);
constexpr Vec4 kWorldZAxisColor(0.27f, 0.45f, 0.85f, 0.9f);

/// Sizes of the editor-only shapes, in metres.
constexpr float kMarkerSize = 0.25f;
constexpr float kSelectedMarkerSize = 0.6f;
constexpr float kSelectionBoxHalfSize = 0.2f;
/// A camera's view volume is drawn only this deep, or it would fill the scene.
constexpr float kCameraFrustumDepth = 2.0f;

/// How close, in pixels, a click has to be to a marker to select its entity, and how
/// close two markers have to be on screen to count as being in the same spot.
constexpr float kPickRadius = 14.0f;
constexpr float kSameSpotTolerance = 1.0f;
/// A press and release further apart than this many pixels is a drag, not a click.
constexpr float kClickTolerance = 4.0f;

} // namespace

ViewportPanel::ViewportPanel(DebugLinePass linePass) : m_linePass(std::move(linePass)) {}

Result<std::unique_ptr<ViewportPanel>> ViewportPanel::create(Engine& engine) {
    CORDITE_TRY_ASSIGN(DebugLinePass linePass,
                       DebugLinePass::create(engine.graphics().device(), engine.shaderManager()));
    return std::unique_ptr<ViewportPanel>(new ViewportPanel(std::move(linePass)));
}

void ViewportPanel::collectLines(const EditorContext& context, const CameraView& view) {
    const World& world = context.world();

    // Centered on the cell under the camera, so that the grid never visibly moves.
    m_gridLines.clear();
    m_gridLines.grid({.center = Vec3(std::round(view.position.x / kGridCellSize) * kGridCellSize, 0.0f,
                                     std::round(view.position.z / kGridCellSize) * kGridCellSize),
                      .cellSize = kGridCellSize,
                      .halfCellCount = kGridHalfCellCount,
                      .color = kGridColor,
                      .majorColor = kGridMajorColor});
    m_gridLines.line(Vec3(-kWorldAxisLength, 0.0f, 0.0f), Vec3(kWorldAxisLength, 0.0f, 0.0f), kWorldXAxisColor);
    m_gridLines.line(Vec3(0.0f, 0.0f, -kWorldAxisLength), Vec3(0.0f, 0.0f, kWorldAxisLength), kWorldZAxisColor);

    m_sceneLines.clear();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        const bool selected = context.isSelected(entity);
        const Mat4& placement = world.worldMatrix(entity);
        m_sceneLines.axes(placement, selected ? kSelectedMarkerSize : kMarkerSize);

        if (const auto* camera = world.tryGet<CameraComponent>(entity)) {
            CameraComponent shown = *camera;
            shown.farPlane = std::min(shown.farPlane, shown.nearPlane + kCameraFrustumDepth);
            // The shape of the volume does not depend on the target it is shown in; 16:9
            // is what a game most likely runs at.
            const CameraView cameraView = makeCameraView(placement, shown, 16.0f / 9.0f);
            m_sceneLines.frustum(cameraView.inverseViewProjection, selected ? kSelectionColor : kCameraColor);
        }
        if (selected) {
            const Mat4 atEntity = glm::translate(Mat4(1.0f), Vec3(placement[3]));
            m_sceneLines.box(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(kSelectionBoxHalfSize)), atEntity,
                             kSelectionColor);
        }
    });
}

Status ViewportPanel::render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, const EditorContext& context,
                             UVec2 size) {
    if (!m_target.isValid()) {
        CORDITE_TRY_ASSIGN(
            m_target,
            RenderTarget::create(
                device, {.size = size, .withDepth = true, .clearColor = kBackgroundColor, .debugName = "Viewport"}));
    } else {
        CORDITE_TRY(m_target.resize(device, size));
    }

    m_view = m_camera.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    collectLines(context, m_view);

    m_target.clear(commandList);
    m_linePass.draw(commandList, m_target.framebuffer(), m_gridLines.vertices(),
                    {.viewProjection = m_view.viewProjection,
                     .fadeOrigin = m_view.position,
                     .fadeStart = kGridFadeStart,
                     .fadeEnd = kGridFadeEnd});
    m_linePass.draw(commandList, m_target.framebuffer(), m_sceneLines.vertices(),
                    {.viewProjection = m_view.viewProjection});
    return {};
}

Entity ViewportPanel::pick(const EditorContext& context, Vec2 pixel) const {
    if (!m_target.isValid()) {
        return Entity{};
    }
    const World& world = context.world();
    const Vec2 targetSize(m_target.size());

    Entity nearest;
    float nearestDistance = std::numeric_limits<float>::max();
    float nearestDepth = std::numeric_limits<float>::max();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        const Vec3 position(world.worldMatrix(entity)[3]);
        const std::optional<Vec2> onScreen = worldToScreen(m_view, position, targetSize);
        if (!onScreen) {
            return;
        }
        const float distance = glm::distance(*onScreen, pixel);
        if (distance > kPickRadius) {
            return;
        }

        // Markers within a pixel of each other count as being in the same spot, and of
        // those the one nearest to the camera is the one that was clicked.
        const float depth = glm::dot(position - m_view.position, m_view.forward);
        const bool clearlyNearer = distance < nearestDistance - kSameSpotTolerance;
        const bool sameSpotInFront = distance <= nearestDistance + kSameSpotTolerance && depth < nearestDepth;
        if (clearlyNearer || sameSpotInFront) {
            nearest = entity;
            nearestDistance = distance;
            nearestDepth = depth;
        }
    });
    return nearest;
}

void ViewportPanel::draw(Engine& engine, EditorContext& context) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin("Viewport");
    ImGui::PopStyleVar();

    bool hovered = false;
    if (visible) {
        // The panel's size is in interface units; the image is rendered in pixels.
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        const UVec2 size(static_cast<uint32_t>(std::max(available.x * scale.x, 1.0f)),
                         static_cast<uint32_t>(std::max(available.y * scale.y, 1.0f)));

        if (available.x >= 1.0f && available.y >= 1.0f &&
            reportIfFailed(render(engine.graphics().device(), engine.commandList(), context, size))) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(UiSystem::textureId(m_target.color()), available);
            hovered = ImGui::IsItemHovered();

            // A click that was not the end of a drag selects what is under the pointer.
            const ImVec2 dragged = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                std::abs(dragged.x) + std::abs(dragged.y) <= kClickTolerance) {
                const ImVec2 pointer = ImGui::GetMousePos();
                const Entity picked =
                    pick(context, Vec2((pointer.x - origin.x) * scale.x, (pointer.y - origin.y) * scale.y));
                if (picked.isNull()) {
                    context.clearSelection();
                } else if (ImGui::GetIO().KeyCtrl) {
                    context.toggleSelection(picked);
                } else {
                    context.setSelection(std::span(&picked, 1));
                }
            }

            const Vec3& position = m_camera.position();
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + 6.0f));
            ImGui::TextDisabled("%.1f, %.1f, %.1f", position.x, position.y, position.z);
        }
    }
    ImGui::End();

    m_camera.update(engine.input(), engine.deltaSeconds(), hovered);

    // While the camera is being steered the pointer is hidden and held in place, so that
    // it cannot run into the edge of the screen.
    if (Window* window = engine.window(); window != nullptr && m_camera.isNavigating() != m_pointerCaptured) {
        m_pointerCaptured = m_camera.isNavigating();
        window->setCursorMode(m_pointerCaptured ? CursorMode::Captured : CursorMode::Normal);
    }
}

} // namespace cordite
