#include "cordite/assets/GltfDocument.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

namespace cordite {

std::string jointNameOf(const GltfDocument& document, uint32_t node) {
    if (node >= document.nodes.size()) {
        return {};
    }
    const std::string& name = document.nodes[node].name;
    const auto sameName = std::ranges::count(document.nodes, name, &GltfNode::name);
    return sameName <= 1 && !name.empty() ? name : fmt::format("{} #{}", name, node);
}

Result<SkeletonData> skeletonOf(const GltfDocument& document, std::span<const uint32_t> joints,
                                std::span<const Mat4> inverseBind, std::string name) {
    const size_t nodeCount = document.nodes.size();
    if (joints.size() > kMaxSkeletonJoints) {
        return makeError(ErrorCode::OutOfRange, "skin '{}' has {} joints; the limit is {}", name, joints.size(),
                         kMaxSkeletonJoints);
    }
    if (!inverseBind.empty() && inverseBind.size() != joints.size()) {
        return makeError(ErrorCode::InvalidArgument, "skin '{}' has {} joints, but {} inverse bind matrices", name,
                         joints.size(), inverseBind.size());
    }

    // Which joint each node is.
    std::vector<int32_t> jointOfNode(nodeCount, kNoJoint);
    for (size_t index = 0; index < joints.size(); ++index) {
        const uint32_t node = joints[index];
        if (node >= nodeCount) {
            return makeError(ErrorCode::OutOfRange, "skin '{}' names node {} as a joint, but there are only {}", name,
                             node, nodeCount);
        }
        if (jointOfNode[node] != kNoJoint) {
            return makeError(ErrorCode::InvalidArgument, "skin '{}' lists the node '{}' as a joint twice", name,
                             document.nodes[node].name);
        }
        jointOfNode[node] = static_cast<int32_t>(index);
    }

    SkeletonData skeleton;
    skeleton.joints.resize(joints.size());
    for (size_t index = 0; index < joints.size(); ++index) {
        const uint32_t node = joints[index];
        SkeletonJoint& joint = skeleton.joints[index];
        joint.name = jointNameOf(document, node);
        joint.rest = document.nodes[node].transform;
        if (!inverseBind.empty()) {
            joint.inverseBind = inverseBind[index];
        }

        // Up the hierarchy to the nearest node that is a joint. What is passed on the
        // way is not moved by an animation of joints, and counts as part of the place
        // the joint hangs from.
        size_t steps = 0;
        for (std::optional<uint32_t> above = document.nodes[node].parent; above;
             above = document.nodes[*above].parent) {
            if (*above >= nodeCount || ++steps > nodeCount) {
                return makeError(ErrorCode::InvalidArgument,
                                 "the node '{}' of skin '{}' hangs on a node that is not there, or on itself",
                                 document.nodes[node].name, name);
            }
            if (jointOfNode[*above] != kNoJoint) {
                joint.parent = jointOfNode[*above];
                break;
            }
            joint.parentOffset = document.nodes[*above].transform.toMatrix() * joint.parentOffset;
        }
    }
    skeleton.name = std::move(name);
    CORDITE_TRY(validateSkeleton(skeleton));
    return skeleton;
}

} // namespace cordite
