#pragma once

/// @file
/// Animations: how the joints of a skeleton move over time.
///
/// An AnimationData is a clip - a walk, a wave, a door that opens. For each joint it
/// moves it has a track: where the joint is relative to what it hangs on, how it is
/// turned and how large it is, each as a curve through keys (math/Curve.h). A track names
/// its joint, so a clip fits every skeleton that has joints of those names. What a clip
/// says nothing about stays as the skeleton has it at rest.
///
/// @code
/// CORDITE_TRY(validateAnimation(clip));                      // always, for data from a file
///
/// for (const AnimationTrack& track : clip.tracks) {
///     Transform& joint = pose[findJoint(skeleton, track.joint)];
///     if (!track.translation.empty()) {
///         joint.position = track.translation.evaluate(seconds);
///     }
/// }
/// @endcode
///
/// Rotations are quaternions written as four numbers, x, y, z, w, because that is what a
/// curve can blend. Neighbouring keys are the same way round - a quaternion and its
/// negative are the same rotation, and going from one to the other the long way would
/// spin the joint - and what a curve gives between two keys has to be brought back to
/// unit length (cordite/animation samples them).

#include "cordite/core/Result.h"
#include "cordite/math/Curve.h"
#include "cordite/math/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cordite {

/// The most keys the curves of one animation may have between them. A file is
/// untrusted; ten minutes of a hundred joints, written down sixty times a second, is a
/// tenth of this.
inline constexpr uint32_t kMaxAnimationKeys = 1u << 25;

/// What an animation does to one joint.
struct AnimationTrack {
    /// The name of the joint (SkeletonJoint::name).
    std::string joint;
    /// Where the joint is relative to what it hangs on, in metres. Without keys the
    /// joint stays where the skeleton has it.
    Curve<Vec3> translation;
    /// How the joint is turned: quaternions as x, y, z, w, of unit length at the keys.
    Curve<Vec4> rotation;
    Curve<Vec3> scale;

    bool operator==(const AnimationTrack&) const = default;
};

struct AnimationData {
    std::string name;
    /// How long the clip is, in seconds: the time of its last key, or more. A clip that
    /// loops goes back to its beginning after this.
    float duration = 0.0f;
    /// At most one for each joint.
    std::vector<AnimationTrack> tracks;

    bool operator==(const AnimationData&) const = default;
};

/// Checks what everything that plays an animation relies on: no more keys than
/// kMaxAnimationKeys, one track for a joint at the most, joints that are named, finite
/// numbers, times that are not negative, rotations of unit length, and a duration that
/// covers every key. The error names the animation and the first thing that is wrong.
[[nodiscard]] Status validateAnimation(const AnimationData& animation);

/// The time of the last key of all the tracks: the shortest an animation can be.
[[nodiscard]] float lastKeyTime(const AnimationData& animation);

} // namespace cordite
