#pragma once

/// @file
/// What moved since a view was last drawn: how far, and which way, what each pixel shows
/// went across the picture since the picture before - its motion vector. Motion blur is
/// made of them.
///
/// The base pass writes them (SceneTargets::motion()) for every solid surface, from where
/// each of its vertices is now and where it was when the view was last drawn: where its
/// entity was then, the pose its figure was in then, and where the camera was then. A
/// view remembers all of that in its targets (MotionHistory), and forgets it when its
/// camera cuts to another shot (MotionHistory::continuesWith()). Where nothing was drawn
/// the sky is seen, which moves as the camera turns and not as it goes: a pass that
/// needs its motion works it out from the directions alone (motionOfSky in
/// shaders/common/Motion.slang).
///
/// @code
/// MotionHistory& history = targets.motionHistory();
/// const bool continues = history.continuesWith(view, settings.viewEntity);
/// const Mat4& viewProjectionBefore = continues ? history.view.viewProjection : view.viewProjection;
/// // ... the draws take what they were placed with from history.placements and
/// // history.poses, and the view constants viewProjectionBefore ...
/// history.remember(view, settings.viewEntity);
/// @endcode
///
/// A motion vector is in parts of the picture - (1, 1) is its width and height - with x to
/// the right and y down, from where a point was to where it is: what went right by a
/// tenth of the picture has (0.1, 0). Translucent surfaces, which are not in the depth
/// buffer, have none of their own: their pixels have the motion of what is behind them.

#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"

#include <utility>
#include <vector>

namespace cordite {

/// A camera that went farther than this between two pictures of a view, in metres,
/// turned by more than this, in radians, or zoomed in or out by more than this factor,
/// has cut to another shot: nothing in the picture that follows is taken to have moved.
/// A camera that is moved this fast within a frame - 600 metres a second at sixty frames
/// - is not one whose motion a picture could show.
inline constexpr float kCameraCutDistance = 10.0f;
inline constexpr float kCameraCutAngle = kPi / 4.0f;
inline constexpr float kCameraCutZoom = 1.5f;

/// What a view remembers of the last time it was drawn, for the motion of the next
/// picture. It belongs to the view's targets (SceneTargets::motionHistory()), which are
/// moved about: everything in it moves without allocating.
struct MotionHistory {
    /// False until the view has been drawn once, and after forget().
    bool valid = false;
    /// What it was seen through: the entity of its camera (SceneRenderSettings::viewEntity),
    /// nil for a view of no entity.
    Uuid camera;
    CameraView view;
    /// Where every entity with a MeshRenderer placed its mesh, in the world, by the id of
    /// the entity - also those the view did not see, which may come into it. In the order
    /// of the ids, once remember() has put them in it.
    std::vector<std::pair<Uuid, Mat4>> placements;
    /// The joint matrices of every pose something was drawn in
    /// (SkeletonPoseComponent::skinning), by the id of the entity that has the pose. In the
    /// order of the ids, as `placements`.
    std::vector<std::pair<Uuid, std::vector<Mat4>>> poses;

    /// Whether a picture of `next`, seen through the entity `seenThrough`, follows on from
    /// what is remembered: the view has been drawn before, through the same entity, and
    /// its camera has not cut to another shot since (isCameraCut()). If not, nothing in the
    /// picture has moved.
    [[nodiscard]] bool continuesWith(const CameraView& next, const Uuid& seenThrough) const;

    /// Where the entity `id` placed its mesh, or null when that is not remembered.
    [[nodiscard]] const Mat4* placementOf(const Uuid& id) const;

    /// The joint matrices of the pose of the entity `id`, or null when none is
    /// remembered.
    [[nodiscard]] const std::vector<Mat4>* poseOf(const Uuid& id) const;

    /// Remembers the camera of the picture just drawn, `drawn`, seen through the entity
    /// `seenThrough`, and puts `placements` and `poses` - which the renderer filled - in
    /// the order of their ids.
    void remember(const CameraView& drawn, const Uuid& seenThrough);

    /// Forgets everything: the next picture is the first.
    void forget();
};

/// Whether a camera that saw `before` and then `after` cut from one shot to another
/// between the two: it went farther than kCameraCutDistance, turned by more than
/// kCameraCutAngle - about any axis, rolling included -, zoomed in or out by more than
/// kCameraCutZoom, or changed between perspective and none. A camera that is not made of
/// numbers cut, too.
[[nodiscard]] bool isCameraCut(const CameraView& before, const CameraView& after);

/// The color the Motion debug view shows for a motion vector `motion` of a picture
/// `targetSize` pixels large, as light: black for none; which way it went is the hue -
/// red to the right, yellow-green up, cyan to the left, blue-purple down - and how far is
/// the brightness, all of it at kMotionDebugFullLength pixels and more. The same
/// arithmetic as motionDebugColor in shaders/passes/DebugView.slang.
[[nodiscard]] Vec3 motionDebugColor(Vec2 motion, UVec2 targetSize);

/// How long a motion is shown at its brightest by the Motion debug view, in pixels.
inline constexpr float kMotionDebugFullLength = 16.0f;

} // namespace cordite
