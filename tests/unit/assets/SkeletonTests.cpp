#include "cordite/assets/Skeleton.h"

#include "cordite/assets/Animation.h"

#include "support/TestSupport.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// Joints that hang on one another as `parents` says, named "Joint 0", "Joint 1", ...
SkeletonData skeletonWith(const std::vector<int32_t>& parents) {
    SkeletonData skeleton;
    skeleton.name = "Test";
    for (size_t index = 0; index < parents.size(); ++index) {
        skeleton.joints.push_back({.name = "Joint " + std::to_string(index), .parent = parents[index]});
    }
    return skeleton;
}

} // namespace

TEST_SUITE("Skeleton") {
    TEST_CASE("a skeleton whose joints hang on one another as a tree is valid") {
        CHECK(validateSkeleton(SkeletonData()).has_value());
        CHECK(validateSkeleton(skeletonWith({kNoJoint})).has_value());
        CHECK(validateSkeleton(skeletonWith({kNoJoint, 0, 1, 1, 0})).has_value());
        // A joint may come before the one it hangs on, and there may be several trees.
        CHECK(validateSkeleton(skeletonWith({2, 0, kNoJoint, kNoJoint, 3})).has_value());

        // As many joints as a skeleton may have, each on the one before.
        std::vector<int32_t> chain(kMaxSkeletonJoints);
        for (size_t index = 0; index < chain.size(); ++index) {
            chain[index] = static_cast<int32_t>(index) - 1;
        }
        CHECK(validateSkeleton(skeletonWith(chain)).has_value());
    }

    TEST_CASE("a skeleton with too many joints, names that are missing or the same, or joints on nothing is refused") {
        std::vector<int32_t> tooMany(kMaxSkeletonJoints + 1, kNoJoint);
        Status status = validateSkeleton(skeletonWith(tooMany));
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::OutOfRange);
        CHECK(status.error().message.find("1025") != std::string::npos);

        SkeletonData unnamed = skeletonWith({kNoJoint, 0});
        unnamed.joints[1].name.clear();
        status = validateSkeleton(unnamed);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("has no name") != std::string::npos);

        SkeletonData twins = skeletonWith({kNoJoint, 0, 0});
        twins.joints[2].name = "Joint 1";
        status = validateSkeleton(twins);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("'Joint 1'") != std::string::npos);

        for (const int32_t parent : {3, 100, -2}) {
            CAPTURE(parent);
            status = validateSkeleton(skeletonWith({kNoJoint, 0, parent}));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::OutOfRange);
            CHECK(status.error().message.find("'Joint 2'") != std::string::npos);
        }
    }

    TEST_CASE("joints that hang on themselves, however many joints in between, are refused") {
        for (const std::vector<int32_t>& parents :
             {std::vector<int32_t>{0}, std::vector<int32_t>{1, 0}, std::vector<int32_t>{kNoJoint, 3, 1, 2},
              std::vector<int32_t>{kNoJoint, 0, 4, 2, 3}}) {
            const Status status = validateSkeleton(skeletonWith(parents));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().message.find("hangs on itself") != std::string::npos);
        }
    }

    TEST_CASE("numbers that are none are refused, wherever they are") {
        SkeletonData skeleton = skeletonWith({kNoJoint, 0});
        REQUIRE(validateSkeleton(skeleton).has_value());

        skeleton.joints[1].rest.position.y = kNotANumber;
        CHECK_FALSE(validateSkeleton(skeleton).has_value());
        skeleton.joints[1].rest = {};
        skeleton.joints[1].rest.rotation.w = std::numeric_limits<float>::infinity();
        CHECK_FALSE(validateSkeleton(skeleton).has_value());
        skeleton.joints[1].rest = {};
        skeleton.joints[1].rest.scale.x = kNotANumber;
        CHECK_FALSE(validateSkeleton(skeleton).has_value());
        skeleton.joints[1].rest = {};
        skeleton.joints[0].inverseBind[3][1] = kNotANumber;
        CHECK_FALSE(validateSkeleton(skeleton).has_value());
        skeleton.joints[0].inverseBind = Mat4(1.0f);
        skeleton.joints[0].parentOffset[0][0] = kNotANumber;
        const Status status = validateSkeleton(skeleton);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().message.find("'Joint 0'") != std::string::npos);
        skeleton.joints[0].parentOffset = Mat4(1.0f);
        CHECK(validateSkeleton(skeleton).has_value());
    }

    TEST_CASE("the order to work joints out in has every joint after the one it hangs on") {
        CHECK(jointOrder(SkeletonData()).empty());
        CHECK(jointOrder(skeletonWith({kNoJoint, 0, 1})) == std::vector<uint32_t>{0, 1, 2});
        // Level by level, and within a level by index: 2 and 3 hang on nothing, 0 and 4
        // on those, 1 on 0.
        CHECK(jointOrder(skeletonWith({2, 0, kNoJoint, kNoJoint, 3})) == std::vector<uint32_t>{2, 3, 0, 4, 1});
        // Children that come before their parents, a chain the wrong way round.
        CHECK(jointOrder(skeletonWith({1, 2, 3, kNoJoint})) == std::vector<uint32_t>{3, 2, 1, 0});

        // Every joint once, each after its parent, for a skeleton of any shape.
        const SkeletonData skeleton = skeletonWith({5, 0, 0, kNoJoint, 3, 3, 4, 6, 6, 2});
        const std::vector<uint32_t> order = jointOrder(skeleton);
        REQUIRE(order.size() == skeleton.joints.size());
        std::vector<bool> seen(order.size(), false);
        for (const uint32_t joint : order) {
            REQUIRE(joint < seen.size());
            CHECK_FALSE(seen[joint]);
            const int32_t parent = skeleton.joints[joint].parent;
            CHECK((parent == kNoJoint || seen[static_cast<size_t>(parent)]));
            seen[joint] = true;
        }

        // What hangs on itself has no place, and what hangs on that has none either; the
        // rest keeps its own.
        CHECK(jointOrder(skeletonWith({kNoJoint, 2, 1, 2, 0})) == std::vector<uint32_t>{0, 4});
        CHECK(jointOrder(skeletonWith({7, kNoJoint})) == std::vector<uint32_t>{1});
    }

    TEST_CASE("a joint is found by its name") {
        const SkeletonData skeleton = skeletonWith({kNoJoint, 0, 1});
        CHECK(findJoint(skeleton, "Joint 0") == 0);
        CHECK(findJoint(skeleton, "Joint 2") == 2);
        CHECK(findJoint(skeleton, "joint 2") == kNoJoint);
        CHECK(findJoint(skeleton, "") == kNoJoint);
        CHECK(findJoint(SkeletonData(), "Joint 0") == kNoJoint);
    }
}

TEST_SUITE("Animation") {
    TEST_CASE("an animation with keys that are numbers, in a time that covers them, is valid") {
        CHECK(validateAnimation(AnimationData()).has_value());

        AnimationData clip;
        clip.name = "Wave";
        clip.duration = 2.0f;
        AnimationTrack arm;
        arm.joint = "Arm";
        arm.translation.addKey(0.0f, Vec3(0.0f));
        arm.translation.addKey(2.0f, Vec3(1.0f, 2.0f, 3.0f), Interpolation::Step);
        arm.rotation.addKey(0.5f, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        arm.rotation.addKey(1.5f, Vec4(0.0f, 0.70710678f, 0.0f, 0.70710678f), Interpolation::Cubic);
        arm.scale.addKey(1.0f, Vec3(2.0f));
        clip.tracks.push_back(arm);
        clip.tracks.push_back({.joint = "Hand"});
        CHECK(validateAnimation(clip).has_value());
        CHECK(lastKeyTime(clip) == 2.0f);
        CHECK(lastKeyTime(AnimationData()) == 0.0f);

        // Longer than its last key, it holds its last pose to the end.
        clip.duration = 5.0f;
        CHECK(validateAnimation(clip).has_value());
    }

    TEST_CASE("an animation that cannot be played is refused, and the error says what is wrong with it") {
        AnimationData clip;
        clip.name = "Wave";
        clip.duration = 1.0f;
        AnimationTrack arm;
        arm.joint = "Arm";
        arm.rotation.addKey(0.0f, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        arm.rotation.addKey(1.0f, Vec4(1.0f, 0.0f, 0.0f, 0.0f));
        clip.tracks.push_back(arm);
        REQUIRE(validateAnimation(clip).has_value());

        const auto refused = [](const AnimationData& animation, std::string_view because) {
            const Status status = validateAnimation(animation);
            REQUIRE_FALSE(status.has_value());
            const std::string& message = status.error().message;
            CHECK_MESSAGE(message.find(because) != std::string::npos, message);
            CHECK(message.find("'Wave'") != std::string::npos);
        };

        AnimationData broken = clip;
        broken.duration = 0.5f;
        refused(broken, "keys after its end");
        broken.duration = kNotANumber;
        refused(broken, "duration");
        broken.duration = -1.0f;
        refused(broken, "duration");

        broken = clip;
        broken.tracks.push_back(arm);
        refused(broken, "more than one track for the joint 'Arm'");
        broken = clip;
        broken.tracks[0].joint.clear();
        refused(broken, "names no joint");

        // A rotation has to be a rotation.
        broken = clip;
        broken.tracks[0].rotation.addKey(0.5f, Vec4(0.0f, 0.0f, 0.0f, 2.0f));
        refused(broken, "not of unit length");
        broken = clip;
        broken.tracks[0].rotation.addKey(0.5f, Vec4(0.0f));
        refused(broken, "not of unit length");

        // Numbers that are none: in a value, in a slope, in a time.
        broken = clip;
        broken.tracks[0].translation.addKey(0.5f, Vec3(0.0f, kNotANumber, 0.0f));
        refused(broken, "a number that is none");
        broken = clip;
        Keyframe<Vec3> key;
        key.time = 0.25f;
        key.outTangent = Vec3(std::numeric_limits<float>::infinity());
        broken.tracks[0].scale.addKey(key);
        refused(broken, "a number that is none");
        broken = clip;
        broken.tracks[0].scale.addKey(-0.5f, Vec3(1.0f));
        refused(broken, "before the beginning");
    }
}
