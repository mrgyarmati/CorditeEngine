#include "cordite/animation/Pose.h"

#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace cordite;
using cordite::test::makeSkinnedBar;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

Vec4 asNumbers(const Quat& rotation) {
    return {rotation.x, rotation.y, rotation.z, rotation.w};
}

/// A rotation about +Z by `degrees`.
Quat aboutZ(float degrees) {
    return glm::angleAxis(toRadians(degrees), Vec3(0.0f, 0.0f, 1.0f));
}

/// How many degrees a rotation turns by.
float degreesOf(const Quat& rotation) {
    return glm::degrees(glm::angle(rotation));
}

/// The bar's skeleton, its clips, and the matrices of a pose of it.
struct Bar {
    Bar() : document(makeSkinnedBar()), skeleton(document.skins[0].skeleton), order(jointOrder(skeleton)) {
        restPose(skeleton, pose);
    }

    /// The matrices its mesh is drawn with in the pose it has now.
    [[nodiscard]] std::vector<Mat4> skinning() const {
        std::vector<Mat4> inModel(pose.size());
        std::vector<Mat4> matrices(pose.size());
        jointMatrices(skeleton, order, pose, inModel);
        skinningMatrices(skeleton, inModel, matrices);
        return matrices;
    }

    GltfDocument document;
    SkeletonData skeleton;
    std::vector<uint32_t> order;
    std::vector<Transform> pose;
};

} // namespace

TEST_SUITE("Pose") {
    TEST_CASE("the tracks of a clip find their joints by name") {
        const Bar bar;
        const AnimationData& bend = bar.document.animations[0].animation;
        CHECK(bindTracks(bend, bar.skeleton) == std::vector<int32_t>{1});

        AnimationData clip;
        clip.tracks = {{.joint = "Tip"}, {.joint = "Elbow"}, {.joint = "Root"}, {.joint = "tip"}};
        CHECK(bindTracks(clip, bar.skeleton) == std::vector<int32_t>{2, kNoJoint, 0, kNoJoint});
        CHECK(bindTracks(AnimationData(), bar.skeleton).empty());
        CHECK(bindTracks(clip, SkeletonData()) == std::vector<int32_t>(4, kNoJoint));
    }

    TEST_CASE("the rest pose is where the skeleton has its joints") {
        const Bar bar;
        REQUIRE(bar.pose.size() == 3);
        CHECK(bar.pose[0] == Transform());
        CHECK(bar.pose[1].position == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(bar.pose[2] == bar.skeleton.joints[2].rest);

        // A pose that was something else is made the rest pose, of the right size.
        std::vector<Transform> pose(7, Transform{.position = Vec3(9.0f)});
        restPose(bar.skeleton, pose);
        CHECK(pose == bar.pose);
        restPose(SkeletonData(), pose);
        CHECK(pose.empty());
    }

    TEST_CASE("a clip that loops goes round and round, and one that does not stops at its ends") {
        CHECK(clipTime(0.0f, 2.0f, true) == 0.0f);
        CHECK(clipTime(1.5f, 2.0f, true) == 1.5f);
        // The end is a time the clip has: a clock that is set there shows the last pose.
        CHECK(clipTime(2.0f, 2.0f, true) == 2.0f);
        CHECK(clipTime(2.5f, 2.0f, true) == 0.5f);
        CHECK(clipTime(4.0f, 2.0f, true) == 0.0f);
        CHECK(clipTime(9.25f, 2.0f, true) == 1.25f);
        // Backwards from the beginning is forwards from the end.
        CHECK(clipTime(-0.5f, 2.0f, true) == 1.5f);
        CHECK(clipTime(-4.5f, 2.0f, true) == 1.5f);
        // Never outside the clip, however the division is rounded - also for times so
        // large, and clips so short, that there is no saying how many runs fit.
        for (const float seconds : {-1.0e-9f, 1.0e9f, -1.0e9f, 5.9999995f, 3.0e38f, -3.0e38f}) {
            for (const float duration : {2.0f, 1.0e-30f, 0.3f}) {
                CAPTURE(seconds);
                CAPTURE(duration);
                const float time = clipTime(seconds, duration, true);
                CHECK(time >= 0.0f);
                CHECK(time <= duration);
            }
        }
        // A step back from the beginning that is too small to count is the end.
        CHECK(clipTime(-1.0e-9f, 2.0f, true) == 2.0f);

        CHECK(clipTime(1.5f, 2.0f, false) == 1.5f);
        CHECK(clipTime(2.5f, 2.0f, false) == 2.0f);
        CHECK(clipTime(-0.5f, 2.0f, false) == 0.0f);

        // A clip of no length is at its beginning; so is a time that is none.
        for (const bool loops : {true, false}) {
            CHECK(clipTime(3.0f, 0.0f, loops) == 0.0f);
            CHECK(clipTime(3.0f, -1.0f, loops) == 0.0f);
            CHECK(clipTime(kNotANumber, 2.0f, loops) == 0.0f);
            CHECK(clipTime(3.0f, kNotANumber, loops) == 0.0f);
            CHECK(clipTime(std::numeric_limits<float>::infinity(), 2.0f, loops) == 0.0f);
        }
    }

    TEST_CASE("a rotation between two keys is on the arc between them, at an even speed") {
        Curve<Vec4> curve;
        CHECK(sampleRotation(curve, 1.0f) == kIdentityQuat);

        // A quarter turn about +Z in three seconds, from the first second on.
        curve.addKey(1.0f, asNumbers(aboutZ(0.0f)));
        curve.addKey(4.0f, asNumbers(aboutZ(90.0f)));
        CHECK(degreesOf(sampleRotation(curve, 0.0f)) == doctest::Approx(0.0f));
        CHECK(degreesOf(sampleRotation(curve, 1.0f)) == doctest::Approx(0.0f));
        // A third of the way is a third of the angle: thirty degrees. A straight line
        // between the numbers would be at 26.6.
        CHECK(degreesOf(sampleRotation(curve, 2.0f)) == doctest::Approx(30.0f).epsilon(1.0e-4));
        CHECK(degreesOf(sampleRotation(curve, 3.0f)) == doctest::Approx(60.0f).epsilon(1.0e-4));
        CHECK(degreesOf(sampleRotation(curve, 4.0f)) == doctest::Approx(90.0f).epsilon(1.0e-4));
        CHECK(degreesOf(sampleRotation(curve, 99.0f)) == doctest::Approx(90.0f).epsilon(1.0e-4));
        CHECK_NEAR(asNumbers(sampleRotation(curve, 2.5f)), asNumbers(aboutZ(45.0f)), 1.0e-6f);
        CHECK(sampleRotation(curve, kNotANumber) == aboutZ(0.0f));
        for (const float seconds : {1.3f, 2.7f, 3.9f}) {
            CHECK(glm::length(sampleRotation(curve, seconds)) == doctest::Approx(1.0f));
        }

        // Stepped, a key holds until the next.
        Curve<Vec4> stepped;
        stepped.addKey(0.0f, asNumbers(aboutZ(10.0f)), Interpolation::Step);
        stepped.addKey(1.0f, asNumbers(aboutZ(80.0f)), Interpolation::Step);
        CHECK(degreesOf(sampleRotation(stepped, 0.99f)) == doctest::Approx(10.0f).epsilon(1.0e-4));
        CHECK(degreesOf(sampleRotation(stepped, 1.0f)) == doctest::Approx(80.0f).epsilon(1.0e-4));

        // A spline gives numbers that are no rotation until they are brought to unit
        // length; with level slopes it is half way at half time.
        Curve<Vec4> spline;
        spline.addKey(0.0f, asNumbers(aboutZ(0.0f)), Interpolation::Cubic);
        spline.addKey(2.0f, asNumbers(aboutZ(90.0f)), Interpolation::Cubic);
        const Quat middle = sampleRotation(spline, 1.0f);
        CHECK(glm::length(middle) == doctest::Approx(1.0f));
        CHECK(degreesOf(middle) == doctest::Approx(45.0f).epsilon(1.0e-4));

        // Keys that are not of unit length, or the long way round from one another, are
        // rotations all the same - and the short way is taken.
        Curve<Vec4> rough;
        rough.addKey(0.0f, asNumbers(aboutZ(0.0f)) * 3.0f);
        rough.addKey(1.0f, -asNumbers(aboutZ(40.0f)));
        CHECK(degreesOf(sampleRotation(rough, 0.5f)) == doctest::Approx(20.0f).epsilon(1.0e-4));
        CHECK(glm::length(sampleRotation(rough, 0.0f)) == doctest::Approx(1.0f));
        // And a key that is nothing at all is no rotation.
        Curve<Vec4> nothing;
        nothing.addKey(0.0f, Vec4(0.0f));
        CHECK(sampleRotation(nothing, 0.0f) == kIdentityQuat);
    }

    TEST_CASE("a clip changes of a pose what it has keys for, and nothing else") {
        Bar bar;
        const AnimationData& bend = bar.document.animations[0].animation;
        const std::vector<int32_t> tracks = bindTracks(bend, bar.skeleton);
        sampleClip(bend, tracks, 0.5f, bar.pose);
        // The middle joint is turned an eighth of a turn, and is where it was.
        CHECK(degreesOf(bar.pose[1].rotation) == doctest::Approx(45.0f).epsilon(1.0e-4));
        CHECK(bar.pose[1].position == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(bar.pose[1].scale == Vec3(1.0f));
        CHECK(bar.pose[0] == Transform());
        CHECK(bar.pose[2] == bar.skeleton.joints[2].rest);

        // "Sway" moves and stretches the root, in steps and gradually.
        const AnimationData& sway = bar.document.animations[1].animation;
        sampleClip(sway, bindTracks(sway, bar.skeleton), 1.5f, bar.pose);
        CHECK(bar.pose[0].position == Vec3(1.0f, 0.0f, 0.0f));
        CHECK(bar.pose[0].scale == Vec3(1.0f, 1.75f, 1.0f));
        CHECK(bar.pose[0].rotation == kIdentityQuat);
        // What the first clip did is still there: a pose is added to, not replaced.
        CHECK(degreesOf(bar.pose[1].rotation) == doctest::Approx(45.0f).epsilon(1.0e-4));

        // Tracks for joints the skeleton does not have, or that are beyond the pose, are
        // passed over.
        AnimationData other;
        other.duration = 1.0f;
        AnimationTrack elbow;
        elbow.joint = "Elbow";
        elbow.translation.addKey(0.0f, Vec3(5.0f));
        other.tracks.push_back(elbow);
        const std::vector<Transform> before = bar.pose;
        sampleClip(other, bindTracks(other, bar.skeleton), 0.0f, bar.pose);
        CHECK(bar.pose == before);
        sampleClip(other, std::vector<int32_t>{7}, 0.0f, bar.pose);
        CHECK(bar.pose == before);
    }

    TEST_CASE("two poses are blended joint by joint") {
        const std::vector<Transform> from = {
            {.position = Vec3(0.0f), .rotation = aboutZ(0.0f), .scale = Vec3(1.0f)},
            {.position = Vec3(1.0f, 2.0f, 3.0f), .rotation = aboutZ(20.0f), .scale = Vec3(2.0f)}};
        const std::vector<Transform> to = {
            {.position = Vec3(4.0f, 0.0f, 0.0f), .rotation = aboutZ(90.0f), .scale = Vec3(3.0f, 1.0f, 1.0f)},
            {.position = Vec3(1.0f, 2.0f, 3.0f), .rotation = aboutZ(20.0f), .scale = Vec3(2.0f)}};
        std::vector<Transform> out(2);

        blendPoses(from, to, 0.0f, out);
        CHECK_NEAR(out[0].position, from[0].position, 1.0e-6f);
        CHECK(degreesOf(out[0].rotation) == doctest::Approx(0.0f));
        blendPoses(from, to, 1.0f, out);
        CHECK_NEAR(out[0].position, to[0].position, 1.0e-6f);
        CHECK(degreesOf(out[0].rotation) == doctest::Approx(90.0f).epsilon(1.0e-4));
        CHECK_NEAR(out[0].scale, to[0].scale, 1.0e-6f);

        blendPoses(from, to, 0.25f, out);
        CHECK_NEAR(out[0].position, Vec3(1.0f, 0.0f, 0.0f), 1.0e-6f);
        CHECK(degreesOf(out[0].rotation) == doctest::Approx(22.5f).epsilon(1.0e-4));
        CHECK_NEAR(out[0].scale, Vec3(1.5f, 1.0f, 1.0f), 1.0e-6f);
        // What is the same in both stays what it is.
        CHECK_NEAR(out[1].position, Vec3(1.0f, 2.0f, 3.0f), 1.0e-6f);
        CHECK(degreesOf(out[1].rotation) == doctest::Approx(20.0f).epsilon(1.0e-4));

        // More than all is all, less than none is none, and what is no number is none.
        blendPoses(from, to, 7.0f, out);
        CHECK_NEAR(out[0].position, to[0].position, 1.0e-6f);
        blendPoses(from, to, -3.0f, out);
        CHECK_NEAR(out[0].position, from[0].position, 1.0e-6f);
        blendPoses(from, to, kNotANumber, out);
        CHECK_NEAR(out[0].position, from[0].position, 1.0e-6f);

        // Into one of the two.
        std::vector<Transform> pose = from;
        blendPoses(pose, to, 0.5f, pose);
        CHECK_NEAR(pose[0].position, Vec3(2.0f, 0.0f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("at rest the joints of the bar are one above the other, and its mesh is where it was modelled") {
        const Bar bar;
        std::vector<Mat4> inModel(3);
        jointMatrices(bar.skeleton, bar.order, bar.pose, inModel);
        CHECK(Vec3(inModel[0][3]) == Vec3(0.0f));
        CHECK(Vec3(inModel[1][3]) == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(Vec3(inModel[2][3]) == Vec3(0.0f, 2.0f, 0.0f));

        // Bound where they stand, the joints leave every vertex where it is.
        const std::vector<Mat4> skinning = bar.skinning();
        for (const Mat4& matrix : skinning) {
            CHECK(test::nearlyEqual(matrix, Mat4(1.0f), 1.0e-6f));
        }
        const MeshData& mesh = bar.document.meshes[0].mesh;
        for (size_t index = 0; index < mesh.vertices.size(); ++index) {
            CHECK_NEAR(skinPosition(mesh.vertices[index].position, mesh.skin[index], skinning),
                       mesh.vertices[index].position, 1.0e-6f);
        }
    }

    TEST_CASE("turning the middle joint a quarter turn tips the upper half of the bar over") {
        Bar bar;
        bar.pose[1].rotation = aboutZ(90.0f);
        std::vector<Mat4> inModel(3);
        jointMatrices(bar.skeleton, bar.order, bar.pose, inModel);
        // The tip, a metre above the middle joint, is now a metre to its side, at -X.
        CHECK_NEAR(Vec3(inModel[2][3]), Vec3(-1.0f, 1.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(Vec3(inModel[1][3]), Vec3(0.0f, 1.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(Vec3(inModel[0][3]), Vec3(0.0f), 1.0e-6f);

        const std::vector<Mat4> skinning = bar.skinning();
        const auto moved = [&](const Vec3& position) {
            return skinPosition(position, test::barSkinAt(position.y), skinning);
        };
        // The foot stays; the top goes round the middle joint with all there is above it.
        CHECK_NEAR(moved(Vec3(0.1f, 0.0f, 0.1f)), Vec3(0.1f, 0.0f, 0.1f), 1.0e-6f);
        CHECK_NEAR(moved(Vec3(0.1f, 2.0f, 0.1f)), Vec3(-1.0f, 1.1f, 0.1f), 1.0e-6f);
        CHECK_NEAR(moved(Vec3(0.1f, 1.5f, 0.1f)), Vec3(-0.5f, 1.1f, 0.1f), 1.0e-6f);
        CHECK_NEAR(moved(Vec3(-0.1f, 1.0f, -0.1f)), Vec3(0.0f, 0.9f, -0.1f), 1.0e-6f);
        // Half way up the lower half a vertex follows both joints, half and half: it is
        // half way between where the one and where the other has it.
        CHECK_NEAR(moved(Vec3(0.1f, 0.5f, 0.1f)), Vec3(0.3f, 0.8f, 0.1f), 1.0e-6f);

        // Moving the root moves everything; stretching it stretches what hangs on it.
        bar.pose[0].position = Vec3(5.0f, 0.0f, 0.0f);
        bar.pose[0].scale = Vec3(1.0f, 2.0f, 1.0f);
        jointMatrices(bar.skeleton, bar.order, bar.pose, inModel);
        CHECK_NEAR(Vec3(inModel[1][3]), Vec3(5.0f, 2.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(Vec3(inModel[2][3]), Vec3(4.0f, 2.0f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("what stands between a joint and what it hangs on is part of where the joint is") {
        // A skeleton whose joints are listed tip first, under an armature that is moved
        // and twice as large.
        SkeletonData skeleton;
        skeleton.joints.resize(2);
        skeleton.joints[0] = {.name = "Tip", .parent = 1, .rest = {.position = Vec3(0.0f, 1.0f, 0.0f)}};
        skeleton.joints[1] = {.name = "Root", .parent = kNoJoint, .rest = {.position = Vec3(0.0f, 0.5f, 0.0f)}};
        skeleton.joints[1].parentOffset = glm::scale(glm::translate(Mat4(1.0f), Vec3(10.0f, 0.0f, 0.0f)), Vec3(2.0f));
        REQUIRE(validateSkeleton(skeleton).has_value());
        const std::vector<uint32_t> order = jointOrder(skeleton);
        REQUIRE(order == std::vector<uint32_t>{1, 0});

        std::vector<Transform> pose;
        restPose(skeleton, pose);
        std::vector<Mat4> inModel(2);
        jointMatrices(skeleton, order, pose, inModel);
        CHECK_NEAR(Vec3(inModel[1][3]), Vec3(10.0f, 1.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(Vec3(inModel[0][3]), Vec3(10.0f, 3.0f, 0.0f), 1.0e-6f);

        // Bound as they stand, the matrices of the rest pose leave a point where it is.
        skeleton.joints[0].inverseBind = glm::inverse(inModel[0]);
        skeleton.joints[1].inverseBind = glm::inverse(inModel[1]);
        std::vector<Mat4> skinning(2);
        skinningMatrices(skeleton, inModel, skinning);
        const MeshSkinVertex both{.joints = {0, 1, 0, 0}, .weights = {0.5f, 0.5f, 0.0f, 0.0f}};
        CHECK_NEAR(skinPosition(Vec3(3.0f, 4.0f, 5.0f), both, skinning), Vec3(3.0f, 4.0f, 5.0f), 1.0e-5f);
    }

    TEST_CASE("poses and matrices of the wrong size are a mistake of the caller, and change nothing") {
        const Bar bar;
        const AnimationData& bend = bar.document.animations[0].animation;
        const Transform marked{.position = Vec3(7.0f)};
        const std::vector<Transform> twoJoints(2, marked);
        std::vector<Transform> pose(3, marked);
        std::vector<Transform> tooShort(2, marked);
        std::vector<Mat4> matrices(3, Mat4(7.0f));
        std::vector<Mat4> tooFew(2, Mat4(7.0f));
        const std::vector<int32_t> noTracks;
        const std::vector<int32_t> twoTracks = {1, 2};
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS(sampleClip(bend, noTracks, 0.5f, pose), test::AssertionFailure);
        CHECK_THROWS_AS(sampleClip(bend, twoTracks, 0.5f, pose), test::AssertionFailure);
        CHECK_THROWS_AS(blendPoses(twoJoints, bar.pose, 0.5f, pose), test::AssertionFailure);
        CHECK_THROWS_AS(blendPoses(bar.pose, twoJoints, 0.5f, pose), test::AssertionFailure);
        CHECK_THROWS_AS(blendPoses(bar.pose, bar.pose, 0.5f, tooShort), test::AssertionFailure);
        CHECK_THROWS_AS(jointMatrices(bar.skeleton, bar.order, twoJoints, matrices), test::AssertionFailure);
        CHECK_THROWS_AS(jointMatrices(bar.skeleton, bar.order, bar.pose, tooFew), test::AssertionFailure);
        CHECK_THROWS_AS(skinningMatrices(bar.skeleton, tooFew, matrices), test::AssertionFailure);
        CHECK_THROWS_AS(skinningMatrices(bar.skeleton, matrices, tooFew), test::AssertionFailure);
#else
        sampleClip(bend, noTracks, 0.5f, pose);
        sampleClip(bend, twoTracks, 0.5f, pose);
        blendPoses(twoJoints, bar.pose, 0.5f, pose);
        blendPoses(bar.pose, twoJoints, 0.5f, pose);
        blendPoses(bar.pose, bar.pose, 0.5f, tooShort);
        jointMatrices(bar.skeleton, bar.order, twoJoints, matrices);
        jointMatrices(bar.skeleton, bar.order, bar.pose, tooFew);
        skinningMatrices(bar.skeleton, tooFew, matrices);
        skinningMatrices(bar.skeleton, matrices, tooFew);
#endif
        // Either way nothing was written.
        CHECK(pose == std::vector<Transform>(3, marked));
        CHECK(tooShort == std::vector<Transform>(2, marked));
        CHECK(matrices == std::vector<Mat4>(3, Mat4(7.0f)));
        CHECK(tooFew == std::vector<Mat4>(2, Mat4(7.0f)));

        // More room than a skeleton has joints is no mistake: the rest is left alone.
        std::vector<Mat4> roomy(5, Mat4(7.0f));
        std::vector<Mat4> skinning(5, Mat4(7.0f));
        jointMatrices(bar.skeleton, bar.order, bar.pose, roomy);
        skinningMatrices(bar.skeleton, roomy, skinning);
        CHECK(Vec3(roomy[2][3]) == Vec3(0.0f, 2.0f, 0.0f));
        CHECK(roomy[3] == Mat4(7.0f));
        CHECK(test::nearlyEqual(skinning[2], Mat4(1.0f), 1.0e-6f));
        CHECK(skinning[4] == Mat4(7.0f));
    }

    TEST_CASE("a vertex that follows a joint there is no matrix for stays where it is") {
        const std::vector<Mat4> skinning = {glm::translate(Mat4(1.0f), Vec3(1.0f, 0.0f, 0.0f))};
        const Vec3 point(2.0f, 3.0f, 4.0f);
        CHECK(skinPosition(point, {.joints = {0, 0, 0, 0}}, skinning) == Vec3(3.0f, 3.0f, 4.0f));
        CHECK(skinPosition(point, {.joints = {5, 0, 0, 0}}, skinning) == point);
        CHECK(skinPosition(point, {.joints = {0, 5, 0, 0}, .weights = {0.5f, 0.5f, 0.0f, 0.0f}}, skinning) ==
              Vec3(2.5f, 3.0f, 4.0f));
        CHECK(skinPosition(point, {.joints = {0, 0, 0, 0}}, {}) == point);
    }
}
