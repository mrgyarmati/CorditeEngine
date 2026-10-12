#include "cordite/assets/Animation.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <unordered_set>

namespace cordite {
namespace {

/// How far the length of a rotation's key may be from one: what a file's rounding to
/// sixteen bits leaves, generously.
constexpr float kUnitTolerance = 1.0e-2f;

bool isFiniteValue(const Vec3& value) {
    return isFinite(value);
}

bool isFiniteValue(const Vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

/// What is wrong with the keys of a curve, in a few words, or nothing.
template <typename Value>
std::string_view problemOf(const Curve<Value>& curve) {
    for (const Keyframe<Value>& key : curve.keys()) {
        if (!std::isfinite(key.time) || key.time < 0.0f) {
            return "a key at a time that is none, or before the beginning";
        }
        if (!isFiniteValue(key.value) || !isFiniteValue(key.inTangent) || !isFiniteValue(key.outTangent)) {
            return "a key with a number that is none";
        }
    }
    return {};
}

} // namespace

float lastKeyTime(const AnimationData& animation) {
    float last = 0.0f;
    for (const AnimationTrack& track : animation.tracks) {
        last = std::max({last, track.translation.endTime(), track.rotation.endTime(), track.scale.endTime()});
    }
    return last;
}

Status validateAnimation(const AnimationData& animation) {
    if (!std::isfinite(animation.duration) || animation.duration < 0.0f) {
        return makeError(ErrorCode::InvalidArgument, "animation '{}' has a duration that is no time", animation.name);
    }

    uint64_t keyCount = 0;
    std::unordered_set<std::string_view> joints;
    for (const AnimationTrack& track : animation.tracks) {
        if (track.joint.empty()) {
            return makeError(ErrorCode::InvalidArgument, "animation '{}' has a track that names no joint",
                             animation.name);
        }
        if (!joints.insert(track.joint).second) {
            return makeError(ErrorCode::InvalidArgument, "animation '{}' has more than one track for the joint '{}'",
                             animation.name, track.joint);
        }
        keyCount += track.translation.keys().size() + track.rotation.keys().size() + track.scale.keys().size();
        if (keyCount > kMaxAnimationKeys) {
            return makeError(ErrorCode::OutOfRange, "animation '{}' has more than {} keys", animation.name,
                             kMaxAnimationKeys);
        }

        for (const std::string_view problem :
             {problemOf(track.translation), problemOf(track.rotation), problemOf(track.scale)}) {
            if (!problem.empty()) {
                return makeError(ErrorCode::InvalidArgument, "the track for '{}' of animation '{}' has {}", track.joint,
                                 animation.name, problem);
            }
        }
        for (const Keyframe<Vec4>& key : track.rotation.keys()) {
            if (std::abs(glm::length(key.value) - 1.0f) > kUnitTolerance) {
                return makeError(ErrorCode::InvalidArgument,
                                 "the track for '{}' of animation '{}' has a rotation that is not of unit length",
                                 track.joint, animation.name);
            }
        }
    }

    if (lastKeyTime(animation) > animation.duration) {
        return makeError(ErrorCode::InvalidArgument, "animation '{}' has keys after its end, {} seconds",
                         animation.name, animation.duration);
    }
    return {};
}

} // namespace cordite
