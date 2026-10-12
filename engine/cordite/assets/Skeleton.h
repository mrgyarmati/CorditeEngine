#pragma once

/// @file
/// Skeletons: the joints a skinned mesh hangs on.
///
/// A skinned mesh is not placed as a whole. Each of its vertices follows up to four
/// joints (MeshSkinVertex in Mesh.h), and the joints form a tree: moving one moves
/// everything below it. A SkeletonData is that tree, as it is when nothing moves it -
/// the rest pose - together with what takes the mesh, as it was modelled, to each joint.
///
/// @code
/// CORDITE_TRY(validateSkeleton(skeleton));                 // always, for data from a file
///
/// std::vector<Mat4> inModel(skeleton.joints.size());       // where each joint is in the model
/// for (const uint32_t joint : jointOrder(skeleton)) {      // parents before their children
///     const SkeletonJoint& data = skeleton.joints[joint];
///     const Mat4 parent = data.parent == kNoJoint ? Mat4(1.0f) : inModel[static_cast<size_t>(data.parent)];
///     inModel[joint] = parent * data.parentOffset * data.rest.toMatrix();
/// }
/// // A vertex bound to joint j is at inModel[j] * skeleton.joints[j].inverseBind * position.
/// @endcode
///
/// Joints are in the order of the file they came from, because that is the order the
/// vertices of a mesh name them in; a joint may come before its parent. jointOrder() is
/// the order to work them out in.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

/// The most joints a skeleton may have. A file is untrusted, and a vertex names its
/// joints in sixteen bits; a character has a hundred joints or two.
inline constexpr uint32_t kMaxSkeletonJoints = 1024;

/// What SkeletonJoint::parent is for a joint that hangs on no other.
inline constexpr int32_t kNoJoint = -1;

struct SkeletonJoint {
    /// Unique within the skeleton: animations name the joints they move.
    std::string name;
    /// The joint this one hangs on, as an index into SkeletonData::joints, or kNoJoint.
    int32_t parent = kNoJoint;
    /// Where the joint is, relative to what it hangs on, when nothing moves it. An
    /// animation replaces this, or parts of it.
    Transform rest;
    /// What stands between the joint and what it hangs on - its parent joint, or the
    /// model itself - and is not a joint: placed parts of the file's hierarchy that an
    /// animation of joints does not move. The identity in most files, for all but the
    /// topmost joints.
    Mat4 parentOffset{1.0f};
    /// Takes a point of the mesh, where it was modelled, into the joint's own space: the
    /// inverse of where the joint was in the model when the mesh was bound to it.
    Mat4 inverseBind{1.0f};

    bool operator==(const SkeletonJoint&) const = default;
};

struct SkeletonData {
    std::string name;
    std::vector<SkeletonJoint> joints;

    bool operator==(const SkeletonData&) const = default;
};

/// Checks what everything that poses a skeleton relies on: no more joints than
/// kMaxSkeletonJoints, names that are there and differ, parents that exist and never
/// lead back to their own joint, and finite numbers. The error names the skeleton and
/// the first thing that is wrong.
[[nodiscard]] Status validateSkeleton(const SkeletonData& skeleton);

/// The joints of a skeleton that passes validateSkeleton(), in an order in which every
/// joint comes after the one it hangs on: the order to work out a pose in. Level by
/// level - the topmost joints first, then the ones that hang on those, and so on - and
/// within a level by index. A skeleton that does not pass gets as many of its joints as
/// can be ordered.
[[nodiscard]] std::vector<uint32_t> jointOrder(const SkeletonData& skeleton);

/// The index of the joint with the given name, or kNoJoint.
[[nodiscard]] int32_t findJoint(const SkeletonData& skeleton, std::string_view name);

} // namespace cordite
