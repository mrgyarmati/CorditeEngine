#include "cordite/editor/ViewportPanel.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/PosedMesh.h"
#include "cordite/math/Color.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/ui/UiWidgets.h"
#include "cordite/world/Components.h"

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
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
/// The box of a decal, and the way it is projected: rose, which no other marker is.
constexpr Vec4 kDecalColor(1.00f, 0.45f, 0.55f, 1.0f);
/// The shape something collides with: it is not drawn, and is seen only like this.
constexpr Vec4 kColliderColor(0.40f, 0.95f, 0.50f, 1.0f);
/// The shape of a trigger, which stops nothing and notices what moves into it.
constexpr Vec4 kTriggerColor(1.00f, 0.55f, 0.20f, 1.0f);
/// A joint between two bodies: where it is, about or along which axis, and to what.
constexpr Vec4 kPhysicsJointColor(0.45f, 0.75f, 1.00f, 1.0f);
/// The least radius that F frames: what is selected is never closer than this fills the view.
constexpr float kFramedRadius = 0.5f;

/// What a gizmo moves: the selection, without what hangs under something else that is
/// selected - which its parent carries along already.
std::vector<Entity> movedTogether(const EditorContext& context) {
    const World& world = context.world();
    std::vector<Entity> moved;
    for (const Entity entity : context.selection()) {
        bool carried = false;
        for (Entity above = world.parent(entity); !above.isNull() && !carried; above = world.parent(above)) {
            carried = context.isSelected(above);
        }
        if (!carried) {
            moved.push_back(entity);
        }
    }
    return moved;
}

constexpr float kPhysicsJointMarkerSize = 0.12f;
constexpr float kPhysicsJointAxisLength = 0.35f;
/// The capsule that a character is.
constexpr Vec4 kCharacterColor(0.75f, 0.60f, 1.00f, 1.0f);
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

/// What drawing the colliders that are made of meshes takes: the physics that has
/// made their shapes, and the outlines of those that were asked for before.
struct ColliderShapes {
    const PhysicsSystem& physics;
    ColliderOutlines& outlines;
};

/// A collider that is made of a mesh, as lines: the edges of the hull around the mesh,
/// or of its triangles - of what things collide with, as the physics has made it. A
/// shape of too many edges to draw is the box around it; one that the physics has not
/// made, or could not make, is nothing.
void drawMeshCollider(const ColliderComponent& collider, const Mat4& placement, ColliderShapes& shapes,
                      const Vec4& color, DebugDraw& lines) {
    const ColliderOutline* outline = shapes.outlines.find(shapes.physics, collider.mesh, collider.shape);
    if (outline == nullptr) {
        return;
    }
    // The middle of the collider is where the origin of its mesh goes.
    const Mat4 meshToWorld = placement * glm::translate(Mat4(1.0f), collider.center);
    if (outline->edges.empty()) {
        lines.box(outline->bounds, meshToWorld, color);
        return;
    }
    for (size_t edge = 0; edge + 1 < outline->edges.size(); edge += 2) {
        lines.line(Vec3(meshToWorld * Vec4(outline->edges[edge], 1.0f)),
                   Vec3(meshToWorld * Vec4(outline->edges[edge + 1], 1.0f)), color);
    }
}

/// The collider of an entity as lines, where the entity has it and as large as its
/// scale makes it for the physics: a box by each axis, a ball by the largest of the
/// three, a capsule by Y in height and by the larger of X and Z across, what is made
/// of a mesh by each axis. Nothing for an entity without a collider.
void drawCollider(const World& world, Entity entity, const Mat4& placement, ColliderShapes& shapes, DebugDraw& lines) {
    const auto* collider = world.tryGet<ColliderComponent>(entity);
    if (collider == nullptr) {
        return;
    }
    const Vec4& color = collider->trigger ? kTriggerColor : kColliderColor;
    const Transform placed = Transform::fromMatrix(placement);
    const Vec3 scale = glm::abs(placed.scale);
    const Vec3 center(placement * Vec4(collider->center, 1.0f));
    switch (collider->shape) {
        case ShapeType::Box:
            lines.box(Aabb::fromCenterExtents(collider->center, 0.5f * collider->size), placement, color);
            break;
        case ShapeType::Sphere:
            lines.sphere(center, collider->radius * std::max({scale.x, scale.y, scale.z}), color);
            break;
        case ShapeType::Capsule: {
            const float radius = collider->radius * std::max(scale.x, scale.z);
            const float halfCylinder = std::max(0.5f * collider->height * scale.y - radius, 0.0f);
            const Vec3 axis = placed.rotation * kWorldUp;
            lines.capsule(center - axis * halfCylinder, center + axis * halfCylinder, radius, color);
            break;
        }
        case ShapeType::ConvexHull:
        case ShapeType::Mesh:
            drawMeshCollider(*collider, placement, shapes, color, lines);
            break;
    }
}

/// The joint of an entity as lines: a mark where it is, the axis of a hinge or of a
/// slider through it, and a line to what the entity is held to - to the other end, for
/// a distance. Nothing for an entity without a joint.
void drawJoint(const World& world, Entity entity, const Mat4& placement, DebugDraw& lines) {
    const auto* joint = world.tryGet<JointComponent>(entity);
    if (joint == nullptr) {
        return;
    }
    const Vec3 anchor(placement * Vec4(joint->anchor, 1.0f));
    lines.cross(anchor, kPhysicsJointMarkerSize, kPhysicsJointColor);
    if (joint->type == JointType::Hinge || joint->type == JointType::Slider) {
        const Vec3 axis = normalizeOr(Vec3(placement * Vec4(joint->axis, 0.0f)), kWorldUp);
        lines.line(anchor - axis * kPhysicsJointAxisLength, anchor + axis * kPhysicsJointAxisLength,
                   kPhysicsJointColor);
    }

    const Entity connected = world.findById(joint->connectedTo);
    const bool isDistance = joint->type == JointType::Distance;
    if (connected.isNull() && !isDistance) {
        // Held to the world where it is: there is no other end to draw a line to.
        return;
    }
    // The other end of a distance is a point of what it is held to, or of the world;
    // any other joint is held to the entity itself.
    const Mat4 connectedToWorld = connected.isNull() ? Mat4(1.0f) : world.worldMatrix(connected);
    const Vec3 end(connectedToWorld * Vec4(isDistance ? joint->connectedAnchor : Vec3(0.0f), 1.0f));
    lines.line(anchor, end, kPhysicsJointColor);
    if (isDistance) {
        lines.cross(end, kPhysicsJointMarkerSize, kPhysicsJointColor);
    }
}

/// The character that an entity is, as lines: its capsule - upright in the world
/// however the entity is turned, with its soles where the entity is, and as large as
/// the entity's scale makes it for the physics, by Y in height and by the larger of X
/// and Z around. Nothing for an entity without a character controller.
void drawCharacter(const World& world, Entity entity, const Mat4& placement, DebugDraw& lines) {
    const auto* controller = world.tryGet<CharacterControllerComponent>(entity);
    if (controller == nullptr) {
        return;
    }
    const Vec3 scale = glm::abs(Transform::fromMatrix(placement).scale);
    const float radius = controller->radius * std::max(scale.x, scale.z);
    // One that is no taller than it is wide is a ball.
    const float height = std::max(controller->height * scale.y, 2.0f * radius);
    const Vec3 soles(placement[3]);
    lines.capsule(soles + kWorldUp * radius, soles + kWorldUp * (height - radius), radius, kCharacterColor);
}

/// What a selected entity shows beside what every entity shows: its collider, its
/// joint and the character it is, and those of what hangs below it, the skeleton its
/// animator has posed, and a box - around its mesh, or around where it is.
void drawSelected(EditorContext& context, Entity entity, const Mat4& placement, ColliderShapes& shapes,
                  DebugDraw& lines) {
    const World& world = context.world();
    // A body is often made of several colliders: its entity's own, and those below it.
    // What holds it is shown with it.
    world.traverseSubtree(entity, [&](Entity part, uint32_t /*depth*/) {
        if (world.isEnabledInHierarchy(part)) {
            drawCollider(world, part, world.worldMatrix(part), shapes, lines);
            drawJoint(world, part, world.worldMatrix(part), lines);
            drawCharacter(world, part, world.worldMatrix(part), lines);
        }
    });
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

std::string_view nameOf(ViewportCamera camera) {
    switch (camera) {
        case ViewportCamera::Editor:
            return "editor";
        case ViewportCamera::Game:
            return "game";
    }
    return "unknown";
}

ViewportPanel::ViewportPanel(SceneRenderer& sceneRenderer, const PhysicsSystem& physics,
                             const SceneRenderSettings& gameSettings, DebugLinePass linePass)
    : m_sceneRenderer(&sceneRenderer),
      m_physics(&physics),
      m_gameSettings(&gameSettings),
      m_linePass(std::move(linePass)) {}

Result<std::unique_ptr<ViewportPanel>> ViewportPanel::create(Engine& engine) {
    CORDITE_TRY_ASSIGN(DebugLinePass linePass,
                       DebugLinePass::create(engine.graphics().device(), engine.shaderManager()));
    return std::unique_ptr<ViewportPanel>(
        new ViewportPanel(engine.sceneRenderer(), engine.physics(), engine.renderSettings(), std::move(linePass)));
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
    ColliderShapes shapes{.physics = *m_physics, .outlines = m_colliderOutlines};
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
        if (world.tryGet<DecalComponent>(entity) != nullptr) {
            // The box it lands in, and the way it is projected: from the middle of the face
            // it sets out from to the middle of the far one.
            const Vec4 color = selected ? kSelectionColor : kDecalColor;
            m_sceneLines.box(Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f)), placement, color);
            m_sceneLines.line(Vec3(placement * Vec4(0.0f, 0.0f, 0.5f, 1.0f)),
                              Vec3(placement * Vec4(0.0f, 0.0f, -0.5f, 1.0f)), color);
        }
        if (selected) {
            drawSelected(context, entity, placement, shapes, m_selectionLines);
        } else if (m_showColliders) {
            drawCollider(world, entity, placement, shapes, m_sceneLines);
            drawJoint(world, entity, placement, m_sceneLines);
            drawCharacter(world, entity, placement, m_sceneLines);
        }
    });
}

Status ViewportPanel::render(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, EditorContext& context,
                             UVec2 size, float deltaSeconds, double timeSeconds, WorldClock clock) {
    if (!m_targets.isValid()) {
        CORDITE_TRY_ASSIGN(m_targets, SceneTargets::create(device, size, "Viewport"));
    } else {
        CORDITE_TRY(m_targets.resize(device, size));
    }

    // Through the camera of the game where that is asked for and the scene has one:
    // what a player sees. Otherwise through the editor's, with what an editor adds.
    const World& world = context.world();
    const float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
    const Entity gameCamera = m_viewCamera == ViewportCamera::Game ? findPrimaryCamera(world) : Entity{};
    const auto* gameLens = world.tryGet<CameraComponent>(gameCamera);
    m_showsGame = gameLens != nullptr;

    SceneRenderSettings settings;
    if (m_showsGame) {
        m_view = makeCameraView(world.worldMatrix(gameCamera), *gameLens, aspect);
        m_gridLines.clear();
        m_sceneLines.clear();
        m_selectionLines.clear();
        // Cleared and lit as the game is, by what the game says and by nothing else -
        // and blurred where it moves while its time passes: an edit is no motion.
        settings.backgroundColor = m_gameSettings->backgroundColor;
        settings.viewEntity = world.id(gameCamera);
        settings.motionBlur = clock == WorldClock::Running;
    } else {
        m_view = m_camera.view(aspect);
        collectLines(context, m_view);
        settings.backgroundColor = srgbToLinear(kBackgroundColor);
        // The editor's camera moves to look around, not as part of the scene: what it
        // passes is not blurred.
        settings.motionBlur = false;
    }
    settings.debugView = m_debugView;
    settings.shadowQuality = m_shadowQuality;
    settings.screenEffectsQuality = m_screenEffectsQuality;
    settings.postProcessQuality = m_postProcessQuality;
    settings.lodErrorPixels = m_lodErrorPixels;
    settings.antiAliasing = m_antiAliasing;
    settings.deltaSeconds = deltaSeconds;
    settings.timeSeconds = timeSeconds;
    // A scene that is being started has no lights yet, and would be black - which is
    // what a game that has none is.
    settings.defaultLighting = m_showsGame ? m_gameSettings->defaultLighting : true;
    // What the cameras of the scene see goes into their render targets first: the view
    // may show it.
    m_sceneRenderer->renderCaptures(commandList, context.world(), settings);
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

    // The gizmo of the selection, over all of it: on the entity that was selected last.
    m_gizmoLines.clear();
    if (const Entity primary = context.primarySelection(); !m_showsGame && !primary.isNull()) {
        const GizmoFrame frame =
            gizmoFrame(world.worldMatrix(primary), m_gizmoSpace, m_gizmoMode, m_view, Vec2(size), gizmoPixelScale());
        drawGizmo(m_gizmoLines, m_gizmoMode, frame, m_held ? m_held->drag.handle : m_hoveredHandle);
        m_linePass.draw(commandList, m_targets.overlayFramebuffer(), m_gizmoLines.vertices(),
                        {.viewProjection = m_view.viewProjection, .depthTest = false});
    }
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

void ViewportPanel::draw(Engine& engine, EditorContext& context, WorldClock clock) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    static const std::string kName = windowName(icons::kViewport, "Viewport");
    const bool visible = ImGui::Begin(kName.c_str());
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
                                  engine.deltaSeconds(), engine.timeSeconds(), clock))) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Image(UiSystem::textureId(m_targets.display()), available);
            hovered = ImGui::IsItemHovered();
            m_imageOrigin = Vec2(origin.x, origin.y);
            const ImVec2 pointer = ImGui::GetMousePos();
            const Vec2 pixel((pointer.x - origin.x) * scale.x, (pointer.y - origin.y) * scale.y);

            // The gizmo has the pointer first: a click on one of its handles selects nothing.
            // With Alt held, the left button is the camera's, which orbits.
            const Input& input = engine.input();
            const bool orbits = input.isKeyDown(Key::LeftAlt) || input.isKeyDown(Key::RightAlt);
            const bool gizmoHasPointer = !m_showsGame && updateGizmo(context, pixel, hovered && !orbits);

            // A click that was not the end of a drag selects what is under the pointer -
            // in the editor's view. In the game's, a click is the game's.
            const ImVec2 dragged = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (hovered && !m_showsGame && !gizmoHasPointer && !orbits &&
                ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                std::abs(dragged.x) + std::abs(dragged.y) <= kClickTolerance) {
                const Entity picked = pick(context, pixel);
                if (picked.isNull()) {
                    context.clearSelection();
                } else if (ImGui::GetIO().KeyCtrl) {
                    context.toggleSelection(picked);
                } else {
                    context.setSelection(std::span(&picked, 1));
                }
            }

            // Where the editor's camera is - when it is the one that is looked through.
            if (!m_showsGame) {
                const Vec3& position = m_camera.position();
                overlayText(*ImGui::GetWindowDrawList(), ImVec2(origin.x + 8.0f, origin.y + 6.0f),
                            fmt::format("{:.1f}, {:.1f}, {:.1f}", position.x, position.y, position.z));
            }
        }
    }
    ImGui::End();

    // F frames what is selected, wherever the pointer is - but not into a text being typed,
    // nor while the camera is being steered.
    if (!m_showsGame && engine.input().wasKeyPressed(Key::F) && !ImGui::GetIO().WantTextInput &&
        !m_camera.isNavigating()) {
        frameSelection(context);
    }

    // The editor's camera is flown only while it is the one that is looked through.
    m_camera.update(engine.input(), engine.deltaSeconds(), hovered && !m_showsGame);

    // Alt pressed and let go of alone takes the keyboard to the menus, in Dear ImGui; Alt
    // that turned the camera did not mean that. A key that is claimed does not do it, and
    // this claim lasts until Alt is let go of.
    if (m_camera.isOrbiting()) {
        ImGui::SetKeyOwner(ImGuiMod_Alt, ImGui::GetID("viewport-orbit"), ImGuiInputFlags_LockUntilRelease);
    }

    // While the camera is being steered the pointer is hidden and held in place, so that
    // it cannot run into the edge of the screen.
    if (Window* window = engine.window(); window != nullptr && m_camera.isNavigating() != m_pointerCaptured) {
        m_pointerCaptured = m_camera.isNavigating();
        window->setCursorMode(m_pointerCaptured ? CursorMode::Captured : CursorMode::Normal);
    }
}

float ViewportPanel::gizmoPixelScale() {
    return ImGui::GetStyle().FontScaleDpi * ImGui::GetIO().DisplayFramebufferScale.y;
}

bool ViewportPanel::frameSelection(EditorContext& context) {
    const World& world = context.world();
    Aabb bounds;
    for (const Entity selected : context.selection()) {
        world.traverseSubtree(selected, [&](Entity entity, uint32_t /*depth*/) {
            // Worked out, not as the last frame left it: a command may frame what it has
            // just made or moved.
            const Mat4 placement = world.computeWorldMatrix(entity);
            if (const std::optional<PosedMesh> mesh = posedMeshOf(world, context.assets(), entity)) {
                bounds.expand(mesh->bounds.transformed(placement));
            } else {
                bounds.expand(Vec3(placement[3]));
            }
        });
    }
    if (bounds.isEmpty()) {
        return false;
    }
    // A point, or something as small, is framed as a metre around it.
    m_camera.frame(bounds.center(), std::max(glm::length(bounds.extents()), kFramedRadius));
    return true;
}

bool ViewportPanel::updateGizmo(EditorContext& context, Vec2 pixel, bool hovered) {
    if (m_held) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            dragHeld(context.world(), pixel);
        } else {
            releaseGizmo(context);
        }
        return true;
    }

    m_hoveredHandle = GizmoHandle::None;
    World& world = context.world();
    const Entity primary = context.primarySelection();
    if (primary.isNull() || !hovered || m_camera.isNavigating()) {
        return false;
    }
    const Vec2 viewport(m_targets.size());
    const GizmoFrame frame =
        gizmoFrame(world.worldMatrix(primary), m_gizmoSpace, m_gizmoMode, m_view, viewport, gizmoPixelScale());
    m_hoveredHandle = pickHandle(m_gizmoMode, frame, m_view, viewport, pixel, gizmoPixelScale());
    if (m_hoveredHandle == GizmoHandle::None) {
        return false;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        HeldGizmo held{.drag = beginDrag(m_gizmoMode, m_hoveredHandle, frame, m_view, viewport, pixel)};
        const ComponentDescriptor* transform = context.components().find("Transform");
        for (const Entity entity : movedTogether(context)) {
            const Entity parent = world.parent(entity);
            held.entities.push_back({
                .id = world.id(entity),
                .start = world.transform(entity),
                .startWorld = world.worldMatrix(entity),
                .parentWorld = parent.isNull() ? Mat4(1.0f) : world.worldMatrix(parent),
                .before = transform->type.toJson(transform->getConst(world.registry(), entity.handle)),
            });
        }
        m_held = std::move(held);
    }
    return true;
}

void ViewportPanel::dragHeld(World& world, Vec2 pixel) {
    if (!m_held) {
        return;
    }
    const GizmoDrag& drag = m_held->drag;
    const Vec2 viewport(m_targets.size());
    for (const HeldEntity& held : m_held->entities) {
        // Something else may have taken an entity away in the middle of the drag.
        const Entity entity = world.findById(held.id);
        if (entity.isNull()) {
            continue;
        }
        Transform& transform = world.transform(entity);
        // Where it was taken hold of, nothing has moved: the transform it started with, to the
        // last digit, which going through the matrix of its parent would not leave it.
        if (pixel == drag.grabbedPixel) {
            transform = held.start;
            continue;
        }
        switch (drag.mode) {
            case GizmoMode::Translate: {
                const Vec3 to = Vec3(held.startWorld[3]) + dragTranslation(drag, m_view, viewport, pixel);
                transform.position = Vec3(glm::inverse(held.parentWorld) * Vec4(to, 1.0f));
                break;
            }
            case GizmoMode::Rotate: {
                // Turned in the world about its own centre: the turn, seen from what it hangs under.
                const Quat parent = Transform::fromMatrix(held.parentWorld).rotation;
                const Quat turn = dragRotation(drag, m_view, viewport, pixel);
                transform.rotation = normalizeRotation(glm::conjugate(parent) * turn * parent * held.start.rotation);
                break;
            }
            case GizmoMode::Scale:
                transform.scale = held.start.scale * dragScale(drag, m_view, viewport, pixel);
                break;
        }
    }
}

void ViewportPanel::releaseGizmo(EditorContext& context) {
    if (!m_held) {
        return;
    }
    const HeldGizmo held = std::move(*m_held);
    m_held.reset();
    std::vector<Entity> entities;
    std::vector<Json> before;
    for (const HeldEntity& entity : held.entities) {
        if (const Entity found = context.world().findById(entity.id); !found.isNull()) {
            entities.push_back(found);
            before.push_back(entity.before);
        }
    }
    reportIfFailed(EditorActions(context).recordComponentEdits(entities, "Transform", before));
}

} // namespace cordite
