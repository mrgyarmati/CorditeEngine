#include "cordite/animation/Pose.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

Quat quatOf(const Vec4& value) {
    // glm's constructor takes w first.
    return {value.w, value.x, value.y, value.z};
}

bool isFinite(const Aabb& box) {
    return std::isfinite(box.min.x) && std::isfinite(box.min.y) && std::isfinite(box.min.z) &&
           std::isfinite(box.max.x) && std::isfinite(box.max.y) && std::isfinite(box.max.z);
}

/// `rotation` at unit length, or no rotation when it has no length to speak of.
Quat unitOr(const Quat& rotation) {
    const float length = glm::length(rotation);
    return std::isfinite(length) && length > kEpsilon ? rotation / length : kIdentityQuat;
}

} // namespace

std::vector<int32_t> bindTracks(const AnimationData& clip, const SkeletonData& skeleton) {
    std::vector<int32_t> jointOfTrack(clip.tracks.size(), kNoJoint);
    for (size_t index = 0; index < clip.tracks.size(); ++index) {
        jointOfTrack[index] = findJoint(skeleton, clip.tracks[index].joint);
    }
    return jointOfTrack;
}

void restPose(const SkeletonData& skeleton, std::vector<Transform>& pose) {
    pose.resize(skeleton.joints.size());
    for (size_t index = 0; index < skeleton.joints.size(); ++index) {
        pose[index] = skeleton.joints[index].rest;
    }
}

float clipTime(float seconds, float duration, bool loops) {
    if (!std::isfinite(seconds) || !std::isfinite(duration) || duration <= 0.0f) {
        return 0.0f;
    }
    // The end of a clip is a time it has, also when it loops: a clock that is set
    // there shows the last pose, not the first.
    if (!loops || (seconds >= 0.0f && seconds <= duration)) {
        return std::clamp(seconds, 0.0f, duration);
    }
    // What is left of a whole number of runs. Rounding can put it a hair outside.
    const float within = seconds - duration * std::floor(seconds / duration);
    return std::clamp(within, 0.0f, duration);
}

Quat sampleRotation(const Curve<Vec4>& curve, float seconds) {
    const std::span<const Keyframe<Vec4>> keys = curve.keys();
    if (keys.empty()) {
        return kIdentityQuat;
    }
    // Before the first key, at or after the last, and for a time that is none: a key.
    if (!(seconds > keys.front().time)) {
        return unitOr(quatOf(keys.front().value));
    }
    if (seconds >= keys.back().time) {
        return unitOr(quatOf(keys.back().value));
    }

    // The first key after the time; the one before it is where the stretch begins.
    const auto after = std::ranges::upper_bound(keys, seconds, {}, &Keyframe<Vec4>::time);
    const Keyframe<Vec4>& from = *(after - 1);
    const Keyframe<Vec4>& to = *after;
    if (from.interpolation == Interpolation::Step) {
        return unitOr(quatOf(from.value));
    }
    if (from.interpolation == Interpolation::Linear) {
        // Along the arc, at an even speed. Keys are the same way round as their
        // neighbours; should two not be, the arc is still the short one.
        const float share = (seconds - from.time) / (to.time - from.time);
        return unitOr(glm::slerp(unitOr(quatOf(from.value)), unitOr(quatOf(to.value)), share));
    }
    // A spline through four numbers, brought back to a rotation.
    return unitOr(quatOf(curve.evaluate(seconds)));
}

void sampleClip(const AnimationData& clip, std::span<const int32_t> jointOfTrack, float seconds,
                std::span<Transform> pose) {
    CORDITE_ASSERT(jointOfTrack.size() == clip.tracks.size(), "{} tracks, but joints for {}", clip.tracks.size(),
                   jointOfTrack.size());
    if (jointOfTrack.size() != clip.tracks.size()) {
        return;
    }
    for (size_t index = 0; index < clip.tracks.size(); ++index) {
        const int32_t joint = jointOfTrack[index];
        if (joint < 0 || static_cast<size_t>(joint) >= pose.size()) {
            continue;
        }
        const AnimationTrack& track = clip.tracks[index];
        Transform& local = pose[static_cast<size_t>(joint)];
        if (!track.translation.empty()) {
            local.position = track.translation.evaluate(seconds);
        }
        if (!track.rotation.empty()) {
            local.rotation = sampleRotation(track.rotation, seconds);
        }
        if (!track.scale.empty()) {
            local.scale = track.scale.evaluate(seconds);
        }
    }
}

void blendPoses(std::span<const Transform> from, std::span<const Transform> to, float amount,
                std::span<Transform> out) {
    CORDITE_ASSERT(from.size() == to.size() && out.size() == from.size(),
                   "poses of {}, {} and {} joints cannot be blended", from.size(), to.size(), out.size());
    if (from.size() != to.size() || out.size() != from.size()) {
        return;
    }
    const float share = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
    for (size_t index = 0; index < out.size(); ++index) {
        out[index] = Transform::interpolate(from[index], to[index], share);
    }
}

void jointMatrices(const SkeletonData& skeleton, std::span<const uint32_t> order, std::span<const Transform> pose,
                   std::span<Mat4> inModel) {
    const size_t count = skeleton.joints.size();
    CORDITE_ASSERT(pose.size() >= count && inModel.size() >= count, "a pose of {} joints for a skeleton of {}",
                   pose.size(), count);
    if (pose.size() < count || inModel.size() < count) {
        return;
    }
    for (const uint32_t joint : order) {
        if (joint >= count) {
            continue;
        }
        const SkeletonJoint& data = skeleton.joints[joint];
        const Mat4 local = data.parentOffset * pose[joint].toMatrix();
        // A parent comes before its children in `order`, so its matrix is there.
        const bool hasParent = data.parent >= 0 && static_cast<size_t>(data.parent) < count;
        inModel[joint] = hasParent ? inModel[static_cast<size_t>(data.parent)] * local : local;
    }
}

void skinningMatrices(const SkeletonData& skeleton, std::span<const Mat4> inModel, std::span<Mat4> skinning) {
    const size_t count = skeleton.joints.size();
    CORDITE_ASSERT(inModel.size() >= count && skinning.size() >= count, "{} matrices for a skeleton of {} joints",
                   inModel.size(), count);
    if (inModel.size() < count || skinning.size() < count) {
        return;
    }
    for (size_t joint = 0; joint < count; ++joint) {
        skinning[joint] = inModel[joint] * skeleton.joints[joint].inverseBind;
    }
}

Vec3 skinPosition(const Vec3& position, const MeshSkinVertex& skin, std::span<const Mat4> skinning) {
    Vec3 moved(0.0f);
    for (size_t slot = 0; slot < skin.weights.size(); ++slot) {
        const float weight = skin.weights[slot];
        if (weight <= 0.0f) {
            continue;
        }
        const size_t joint = skin.joints[slot];
        const Vec3 byJoint = joint < skinning.size() ? Vec3(skinning[joint] * Vec4(position, 1.0f)) : position;
        moved += byJoint * weight;
    }
    return moved;
}

Aabb skinnedBounds(std::span<const Aabb> jointBounds, std::span<const Mat4> skinning) {
    Aabb bounds;
    for (size_t joint = 0; joint < jointBounds.size(); ++joint) {
        const Aabb& box = jointBounds[joint];
        if (box.isEmpty()) {
            continue;
        }
        const Aabb moved = joint < skinning.size() ? box.transformed(skinning[joint]) : box;
        bounds.expand(isFinite(moved) ? moved : box);
    }
    return bounds;
}

} // namespace cordite
