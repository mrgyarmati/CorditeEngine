#include "cordite/editor/ViewportPanel.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/PosedMesh.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/world/Components.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace cordite {
namespace {

/// What is behind the scene, as a display color.
constexpr Vec3 kBackgroundColor(0.085f, 0.090f, 0.105f);
constexpr Vec4 kSelectionColor(1.0f, 0.78f, 0.20f, 1.0f);
constexpr Vec4 kCameraColor(0.75f, 0.78f, 0.85f, 1.0f);
/// A light is not seen itself, only what it lights; its lines say where it is.
constexpr Vec4 kLightColor(1.0f, 0.86f, 0.38f, 1.0f);
/// The box in which a PostProcess component applies.
constexpr Vec4 kVolumeColor(0.55f, 0.85f, 0.80f, 1.0f);
/// The bones of a skeleton: joints are not entities, and are seen only like this.
constexpr Vec4 kSkeletonColor(0.95f, 0.45f, 0.85f, 1.0f);

/// The ground grid follows the camera and fades out before its edge comes into view.
/// Its lines are faint: far away they crowd together, and at full strength they would
/// add up to a bright band along the horizon.
constexpr float kGridCellSize = 1.0f;
constexpr uint32_t kGridHalfCellCount = 60;
constexpr float kGridFadeStart = 10.0f;
constexpr float kGridFadeEnd = 50.0f;
constexpr Vec4 kGridColor(0.42f, 0.44f, 0.50f, 0.35f);
constexpr Vec4 kGridMajorColor(0.55f, 0.57f, 0.64f, 0.75f);
/// How much nearer the grid and the markers are drawn than they are, as a fraction of
/// their distance, so that a floor at height zero does not swallow the grid.
constexpr float kLineDepthBias = 0.0005f;

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
/// A light that is not selected shows what kind it is and where it points, this
/// large; how far it reaches is drawn only for a selected one.
constexpr float kLightMarkerRadius = 0.15f;
constexpr float kLightMarkerLength = 0.75f;
constexpr uint32_t kLightMarkerSegments = 16;
/// The cross that marks a joint of a selected skeleton.
constexpr float kJointMarkerSize = 0.03f;

/// How close, in pixels, a click has to be to a marker to select its entity, and how
/// close two markers have to be on screen to count as being in the same spot.
constexpr float kPickRadius = 14.0f;
constexpr float kSameSpotTolerance = 1.0f;
/// A press and release further apart than this many pixels is a drag, not a click.
constexpr float kClickTolerance = 4.0f;

/// Whether an entity draws a mesh that can be loaded. A mesh that cannot was reported
/// by the renderer, and is simply not there for the editor either.
bool drawsMesh(EditorContext& context, Entity entity) {
    const auto* renderer = context.world().tryGet<MeshRendererComponent>(entity);
    return renderer != nullptr && !renderer->mesh.isNil() && context.assets().loadMesh(renderer->mesh).has_value();
}

/// The skeleton an animator has posed, as lines: from every joint to the joint it hangs
/// on, and a cross at each. `placement` is where the entity of the animator is, which
/// is where the meshes that hang under it unmoved are drawn in the pose.
void drawSkeleton(const SkeletonData& skeleton, const SkeletonPoseComponent& pose, const Mat4& placement,
                  DebugDraw& lines) {
    const size_t count = std::min(skeleton.joints.size(), pose.joints.size());
    const auto positionOf = [&](size_t joint) { return Vec3(placement * pose.joints[joint][3]); };
    const auto isFinite = [](const Vec3& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    for (size_t joint = 0; joint < count; ++joint) {
        const Vec3 position = positionOf(joint);
        if (!isFinite(position)) {
            continue;
        }
        lines.cross(position, kJointMarkerSize, kSkeletonColor);
        const int32_t parent = skeleton.joints[joint].parent;
        if (parent < 0 || static_cast<size_t>(parent) >= count) {
            continue;
        }
        if (const Vec3 above = positionOf(static_cast<size_t>(parent)); isFinite(above)) {
            lines.line(above, position, kSkeletonColor);
        }
    }
}

/// What a selected entity shows beside what every entity shows: the skeleton its
/// animator has posed, and a box - around its mesh, or around where it is.
void drawSelected(EditorContext& context, Entity entity, const Mat4& placement, DebugDraw& lines) {
    const World& world = context.world();
    // The joints an animator moves are no entities; this is where they are seen.
    const auto* animator = world.tryGet<AnimatorComponent>(entity);
    const auto* pose = world.tryGet<SkeletonPoseComponent>(entity);
    if (animator != nullptr && pose != nullptr) {
        // Loaded already: the pose was made of it.
        if (const auto skeleton = context.assets().loadSkeleton(animator->skeleton)) {
            drawSkeleton(**skeleton, *pose, placement, lines);
        }
    }
    if (const std::optional<PosedMesh> mesh = posedMeshOf(world, context.assets(), entity)) {
        // The box the mesh fills - in the pose it is drawn in, for one that joints
        // move - turned and scaled with the entity.
        lines.box(mesh->bounds, placement, kSelectionColor);
    } else {
        const Mat4 atEntity = glm::translate(Mat4(1.0f), Vec3(placement[3]));
        lines.box(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(kSelectionBoxHalfSize)), atEntity, kSelectionColor);
    }
}

/// A light as lines: what kind it is and where it shines into `marker`, and how far
/// it reaches into `reach`. Drawn from the light as the renderer sees it, so the
/// lines are where the light is.
void drawLight(const SceneLight& light, const Vec4& color, DebugDraw& marker, DebugDraw* reach) {
    switch (light.type) {
        case GpuLightType::Directional:
            // The same everywhere: a disc the light leaves, and the way it goes.
            marker.circle(light.position, light.direction, kLightMarkerRadius, color, kLightMarkerSegments);
            marker.arrow(light.position, light.position + light.direction * kLightMarkerLength, color);
            break;
        case GpuLightType::Point:
            marker.sphere(light.position, kLightMarkerRadius, color, kLightMarkerSegments);
            if (reach != nullptr) {
                reach->sphere(light.position, light.range, color);
            }
            break;
        case GpuLightType::Spot: {
            // The edge of the cone is as long as the light reaches, so its rim lies on
            // the sphere of the range.
            const auto cone = [&light, &color](DebugDraw& lines, float angle, float length) {
                lines.cone(light.position, light.direction, std::cos(angle) * length, std::sin(angle) * length, color);
            };
            cone(marker, light.outerConeAngle, std::min(light.range, kLightMarkerLength));
            if (reach != nullptr) {
                cone(*reach, light.outerConeAngle, light.range);
                // Where the light starts to fade towards the edge.
                reach->circle(light.position + light.direction * (std::cos(light.innerConeAngle) * light.range),
                              light.direction, std::sin(light.innerConeAngle) * light.range, color);
            }
            break;
        }
    }
}

} // namespace

ViewportPanel::ViewportPanel(SceneRenderer& sceneRenderer, DebugLinePass linePass)
    : m_sceneRenderer(&sceneRenderer), m_linePass(std::move(linePass)) {}

Result<std::unique_ptr<ViewportPanel>> ViewportPanel::create(Engine& engine) {
    CORDITE_TRY_ASSIGN(DebugLinePass linePass,
                       DebugLinePass::create(engine.graphics().device(), engine.shaderManager()));
    return std::unique_ptr<ViewportPanel>(new ViewportPanel(engine.sceneRenderer(), std::move(linePass)));
}

void ViewportPanel::collectLines(EditorContext& context, const CameraView& view) {
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
    m_selectionLines.clear();
    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        const bool selected = context.isSelected(entity);
        const Mat4& placement = world.worldMatrix(entity);
        const bool hasMesh = drawsMesh(context, entity);

        // An entity that draws a mesh is seen by its mesh; anything else gets a marker.
        if (!hasMesh) {
            m_sceneLines.axes(placement, selected ? kSelectedMarkerSize : kMarkerSize);
        }
        if (const auto* camera = world.tryGet<CameraComponent>(entity)) {
            CameraComponent shown = *camera;
            shown.farPlane = std::min(shown.farPlane, shown.nearPlane + kCameraFrustumDepth);
            // The shape of the volume does not depend on the target it is shown in; 16:9
            // is what a game most likely runs at.
            const CameraView cameraView = makeCameraView(placement, shown, 16.0f / 9.0f);
            m_sceneLines.frustum(cameraView.inverseViewProjection, selected ? kSelectionColor : kCameraColor);
        }
        if (const auto* light = world.tryGet<LightComponent>(entity)) {
            drawLight(makeSceneLight(*light, placement), selected ? kSelectionColor : kLightColor, m_sceneLines,
                      selected ? &m_selectionLines : nullptr);
        }
        if (const auto* postProcess = world.tryGet<PostProcessComponent>(entity);
            postProcess != nullptr && !postProcess->global && world.tryGet<CameraComponent>(entity) == nullptr) {
            // The box its settings apply in: a cube of one metre, as the entity places
            // it. Selected, also how far outside it they fade in - in metres of the
            // world, whatever the box is scaled by.
            m_sceneLines.box(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f)), placement,
                             selected ? kSelectionColor : kVolumeColor);
            const Vec3 axes(glm::length(Vec3(placement[0])), glm::length(Vec3(placement[1])),
                            glm::length(Vec3(placement[2])));
            if (selected && postProcess->blendDistance > 0.0f && axes.x > 0.0f && axes.y > 0.0f && axes.z > 0.0f) {
                m_selectionLines.box(
                    Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f) + Vec3(postProcess->blendDistance) / axes),
                    placement, kVolumeColor);
            }
        }
        if (selected) {
            drawSelected(context, entity, placement, m_selectionLines);
        }
    });
}

Status ViewportPanel::render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, EditorContext& context,
                             UVec2 size, float deltaSeconds, double timeSeconds) {
    if (!m_targets.isValid()) {
        CORDITE_TRY_ASSIGN(m_targets, SceneTargets::create(device, size, "Viewport"));
    } else {
        CORDITE_TRY(m_targets.resize(device, size));
    }

    m_view = m_camera.view(static_cast<float>(size.x) / static_cast<float>(size.y));
    collectLines(context, m_view);

    SceneRenderSettings settings;
    settings.backgroundColor = srgbToLinear(kBackgroundColor);
    settings.debugView = m_debugView;
    settings.shadowQuality = m_shadowQuality;
    settings.screenEffectsQuality = m_screenEffectsQuality;
    settings.postProcessQuality = m_postProcessQuality;
    settings.antiAliasing = m_antiAliasing;
    settings.deltaSeconds = deltaSeconds;
    settings.timeSeconds = timeSeconds;
    // A scene that is being started has no lights yet, and would be black.
    settings.defaultLighting = true;
    m_sceneRenderer->render(commandList, context.world(), m_view, m_targets, settings);

    // Over the finished image, in display colors, but still behind what is nearer.
    m_linePass.draw(commandList, m_targets.overlayFramebuffer(), m_gridLines.vertices(),
                    {.viewProjection = m_view.viewProjection,
                     .depthBias = kLineDepthBias,
                     .fadeOrigin = m_view.position,
                     .fadeStart = kGridFadeStart,
                     .fadeEnd = kGridFadeEnd});
    m_linePass.draw(commandList, m_targets.overlayFramebuffer(), m_sceneLines.vertices(),
                    {.viewProjection = m_view.viewProjection, .depthBias = kLineDepthBias});
    // What is selected shows through everything.
    m_linePass.draw(commandList, m_targets.overlayFramebuffer(), m_selectionLines.vertices(),
                    {.viewProjection = m_view.viewProjection, .depthTest = false});
    return {};
}

Entity ViewportPanel::pick(EditorContext& context, Vec2 pixel) const {
    if (!m_targets.isValid()) {
        return Entity{};
    }
    const World& world = context.world();
    const Vec2 targetSize(m_targets.size());
    const Ray ray = screenRay(m_view, pixel, targetSize);

    // The mesh the ray hits first.
    Entity hitEntity;
    float hitDistance = std::numeric_limits<float>::max();
    // The marker nearest to the pixel, for entities without a mesh.
    Entity markerEntity;
    float markerOffset = std::numeric_limits<float>::max();
    float markerDepth = std::numeric_limits<float>::max();

    world.traverse([&](Entity entity, uint32_t /*depth*/) {
        if (!world.isEnabledInHierarchy(entity)) {
            return;
        }
        const Mat4& placement = world.worldMatrix(entity);
        // A mesh is hit where it is drawn: one that joints move, in its pose.
        if (const std::optional<PosedMesh> mesh = posedMeshOf(world, context.assets(), entity)) {
            const std::optional<float> distance = intersect(ray, *mesh, placement);
            if (distance && *distance < hitDistance) {
                hitEntity = entity;
                hitDistance = *distance;
            }
            return;
        }

        const Vec3 position(placement[3]);
        const std::optional<Vec2> onScreen = worldToScreen(m_view, position, targetSize);
        if (!onScreen) {
            return;
        }
        const float offset = glm::distance(*onScreen, pixel);
        if (offset > kPickRadius) {
            return;
        }
        // Markers within a pixel of each other count as being in the same spot, and of
        // those the one nearest to the camera is the one that was clicked.
        const float depth = glm::dot(position - m_view.position, m_view.forward);
        const bool clearlyNearer = offset < markerOffset - kSameSpotTolerance;
        const bool sameSpotInFront = offset <= markerOffset + kSameSpotTolerance && depth < markerDepth;
        if (clearlyNearer || sameSpotInFront) {
            markerEntity = entity;
            markerOffset = offset;
            markerDepth = depth;
        }
    });

    // A marker is drawn behind meshes that are nearer, and cannot be clicked through them.
    if (!markerEntity.isNull()) {
        const float markerDistance = glm::distance(Vec3(world.worldMatrix(markerEntity)[3]), ray.origin);
        if (hitEntity.isNull() || markerDistance < hitDistance) {
            return markerEntity;
        }
    }
    return hitEntity;
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
            reportIfFailed(render(engine.graphics().device(), engine.commandList(), context, size,
                                  engine.deltaSeconds(), engine.timeSeconds()))) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(UiSystem::textureId(m_targets.display()), available);
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
