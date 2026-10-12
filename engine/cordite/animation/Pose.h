#pragma once

/// @file
/// Poses: where the joints of a skeleton are at one moment, and how to get there.
///
/// A pose says, for every joint of a skeleton, where it is relative to what it hangs on.
/// It starts as the rest pose; a clip puts what it says at a time over that; two poses
/// can be blended; and from a pose come the matrices a skinned mesh is drawn with.
///
/// @code
/// const std::vector<uint32_t> order = jointOrder(skeleton);
/// const std::vector<int32_t> tracks = bindTracks(walk, skeleton);
///
/// std::vector<Transform> pose;
/// restPose(skeleton, pose);
/// sampleClip(walk, tracks, clipTime(seconds, walk.duration, true), pose);
///
/// std::vector<Mat4> inModel(pose.size());
/// std::vector<Mat4> skinning(pose.size());
/// jointMatrices(skeleton, order, pose, inModel);
/// skinningMatrices(skeleton, inModel, skinning);
/// const Vec3 moved = skinPosition(vertex.position, mesh.skin[index], skinning);
/// @endcode
///
/// The functions take the data as it is: a skeleton and an animation that have passed
/// validateSkeleton() and validateAnimation(), as everything the asset manager hands
/// out has. Spans of the wrong size are an error of the caller, asserted; where there
/// are no assertions, the function then changes nothing.

#include "cordite/assets/Animation.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/math/Curve.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"

#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// Which joint of a skeleton each track of a clip moves: the one of the track's name, or
/// kNoJoint for a track whose joint the skeleton does not have. Work it out once for a
/// clip and a skeleton, not for every frame.
[[nodiscard]] std::vector<int32_t> bindTracks(const AnimationData& clip, const SkeletonData& skeleton);

/// Makes `pose` the pose of a skeleton at rest: one entry for every joint.
void restPose(const SkeletonData& skeleton, std::vector<Transform>& pose);

/// The time in a clip after `seconds` of playing it. A time the clip has - from zero to
/// its duration, both included - is that time. One beyond its ends is, for a clip that
/// loops, what is left when whole runs of the clip are taken away - backwards too, for
/// negative seconds - and for one that does not, the end it is beyond. A clip without
/// duration is always at its beginning; a time that is no number is the beginning.
[[nodiscard]] float clipTime(float seconds, float duration, bool loops);

/// What a curve of rotations says at a time, as a rotation: of unit length, and between
/// two keys on the shortest way from the one to the other (at an even speed where the
/// keys are joined by straight lines, as glTF asks). The identity for a curve without keys.
[[nodiscard]] Quat sampleRotation(const Curve<Vec4>& curve, float seconds);

/// Puts what a clip says at `seconds` over a pose. `jointOfTrack` is what bindTracks()
/// gave for the clip and the skeleton of the pose. Of a joint, only what the clip has
/// keys for is changed - a track with rotations alone leaves where the joint is - and
/// joints without a track are left as they are.
void sampleClip(const AnimationData& clip, std::span<const int32_t> jointOfTrack, float seconds,
                std::span<Transform> pose);

/// Blends two poses of the same skeleton, joint by joint: `amount` 0 gives `from`, 1
/// gives `to`; positions and scales along a straight line, rotations along the shortest
/// arc. `out` may be one of the two.
void blendPoses(std::span<const Transform> from, std::span<const Transform> to, float amount, std::span<Transform> out);

/// Where every joint is in the model for a pose: what takes a point from the joint's own
/// space to the space of the model. `order` is jointOrder() of the skeleton.
void jointMatrices(const SkeletonData& skeleton, std::span<const uint32_t> order, std::span<const Transform> pose,
                   std::span<Mat4> inModel);

/// What a skinned mesh is drawn with: for every joint, the matrix that takes a vertex
/// from where the mesh was modelled to where the joint has it now.
void skinningMatrices(const SkeletonData& skeleton, std::span<const Mat4> inModel, std::span<Mat4> skinning);

/// Where a point of a skinned mesh is for the given matrices: moved by each of the
/// joints it follows, by their weights. What the vertex shader does, for code that has
/// to know without drawing - bounds, picking, tests. A joint the matrices do not have
/// leaves the point where it was.
[[nodiscard]] Vec3 skinPosition(const Vec3& position, const MeshSkinVertex& skin, std::span<const Mat4> skinning);

} // namespace cordite
