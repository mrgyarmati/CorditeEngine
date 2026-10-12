#include "cordite/assets/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_set>

namespace cordite {
namespace {

bool isFiniteMatrix(const Mat4& matrix) {
    for (glm::length_t column = 0; column < 4; ++column) {
        for (glm::length_t row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool isFiniteTransform(const Transform& transform) {
    const Quat& rotation = transform.rotation;
    return isFinite(transform.position) && isFinite(transform.scale) && std::isfinite(rotation.x) &&
           std::isfinite(rotation.y) && std::isfinite(rotation.z) && std::isfinite(rotation.w);
}

} // namespace

Status validateSkeleton(const SkeletonData& skeleton) {
    const size_t count = skeleton.joints.size();
    if (count > kMaxSkeletonJoints) {
        return makeError(ErrorCode::OutOfRange, "skeleton '{}' has {} joints; the limit is {}", skeleton.name, count,
                         kMaxSkeletonJoints);
    }

    std::unordered_set<std::string_view> names;
    for (size_t index = 0; index < count; ++index) {
        const SkeletonJoint& joint = skeleton.joints[index];
        if (joint.name.empty()) {
            return makeError(ErrorCode::InvalidArgument, "joint {} of skeleton '{}' has no name", index, skeleton.name);
        }
        if (!names.insert(joint.name).second) {
            return makeError(ErrorCode::InvalidArgument, "skeleton '{}' has more than one joint called '{}'",
                             skeleton.name, joint.name);
        }
        if (joint.parent != kNoJoint && (joint.parent < 0 || static_cast<size_t>(joint.parent) >= count)) {
            return makeError(ErrorCode::OutOfRange, "joint '{}' of skeleton '{}' hangs on joint {}, and there are {}",
                             joint.name, skeleton.name, joint.parent, count);
        }
        if (!isFiniteTransform(joint.rest) || !isFiniteMatrix(joint.parentOffset) ||
            !isFiniteMatrix(joint.inverseBind)) {
            return makeError(ErrorCode::InvalidArgument, "joint '{}' of skeleton '{}' has a number that is none",
                             joint.name, skeleton.name);
        }
    }

    // A joint that hangs on itself, however many joints in between, hangs on nothing:
    // walking up from it never ends. No walk up is longer than there are joints.
    for (size_t index = 0; index < count; ++index) {
        int32_t current = skeleton.joints[index].parent;
        for (size_t steps = 0; current != kNoJoint; ++steps) {
            if (steps >= count) {
                return makeError(ErrorCode::InvalidArgument, "joint '{}' of skeleton '{}' hangs on itself",
                                 skeleton.joints[index].name, skeleton.name);
            }
            current = skeleton.joints[static_cast<size_t>(current)].parent;
        }
    }
    return {};
}

std::vector<uint32_t> jointOrder(const SkeletonData& skeleton) {
    const size_t count = skeleton.joints.size();
    std::vector<uint32_t> order;
    order.reserve(count);
    std::vector<bool> placed(count, false);

    // Round after round, every joint whose parent has its place gets one. A tree as
    // deep as it has joints takes as many rounds; skeletons are a dozen deep.
    bool placedAny = true;
    while (order.size() < count && placedAny) {
        placedAny = false;
        for (size_t index = 0; index < count; ++index) {
            if (placed[index]) {
                continue;
            }
            const int32_t parent = skeleton.joints[index].parent;
            const bool parentIsPlaced = parent == kNoJoint || (parent >= 0 && static_cast<size_t>(parent) < count &&
                                                               placed[static_cast<size_t>(parent)]);
            if (parentIsPlaced) {
                // Marked after the round, so that a round places one level of the tree
                // and the order does not depend on where in the list a parent stands.
                order.push_back(static_cast<uint32_t>(index));
                placedAny = true;
            }
        }
        for (const uint32_t index : order) {
            placed[index] = true;
        }
    }
    return order;
}

int32_t findJoint(const SkeletonData& skeleton, std::string_view name) {
    const auto found = std::ranges::find(skeleton.joints, name, &SkeletonJoint::name);
    return found == skeleton.joints.end() ? kNoJoint : static_cast<int32_t>(found - skeleton.joints.begin());
}

} // namespace cordite
