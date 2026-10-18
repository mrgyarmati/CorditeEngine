#include "cordite/animation/AnimationSystem.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// How many degrees the rotation part of a matrix turns by.
float degreesOf(const Mat4& matrix) {
    return glm::degrees(glm::angle(glm::quat_cast(Mat3(matrix))));
}

/// A project with the skinned bar in it, a world, and a figure in the world that the
/// bar's skeleton moves.
struct Stage {
    Stage() {
        REQUIRE(exportGltf(test::makeSkinnedBar(), directory.path() / "assets" / "bar.gltf").has_value());
        assets = unwrap(AssetManager::create({.rootDirectory = directory.path() / "assets"}));
        skeleton = idOf("bar.gltf#skeleton/0");
        bend = idOf("bar.gltf#animation/0");
        sway = idOf("bar.gltf#animation/1");
        figure = world.createEntity("Figure");
        world.add<AnimatorComponent>(figure) = {.skeleton = skeleton, .clip = bend};
    }

    [[nodiscard]] Uuid idOf(std::string_view reference) const {
        const AssetInfo* asset = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
        return asset->id;
    }

    [[nodiscard]] AnimatorComponent& animator() { return *world.tryGet<AnimatorComponent>(figure); }

    [[nodiscard]] const SkeletonPoseComponent* pose() const { return world.tryGet<SkeletonPoseComponent>(figure); }

    /// How far the middle joint is turned, in degrees. Measured against the root: where
    /// the root is stretched, the matrix of what hangs on it is more than a rotation.
    [[nodiscard]] float middleTurn() const {
        const SkeletonPoseComponent* posed = pose();
        REQUIRE(posed != nullptr);
        REQUIRE(posed->joints.size() == 3);
        return degreesOf(glm::inverse(posed->joints[0]) * posed->joints[1]);
    }

    /// Where a joint is.
    [[nodiscard]] Vec3 jointAt(size_t joint) const {
        const SkeletonPoseComponent* posed = pose();
        REQUIRE(posed != nullptr);
        REQUIRE(joint < posed->joints.size());
        return Vec3(posed->joints[joint][3]);
    }

    /// Lets time pass.
    AnimationStatistics update(float seconds) { return system.update(world, *assets, seconds); }

    /// Poses without letting time pass.
    AnimationStatistics evaluate() { return system.evaluate(world, *assets); }

    TempDirectory directory;
    std::unique_ptr<AssetManager> assets;
    World world;
    AnimationSystem system;
    Uuid skeleton;
    Uuid bend;
    Uuid sway;
    Entity figure;
};

} // namespace

TEST_SUITE("AnimationSystem") {
    TEST_CASE("an animator with a skeleton gets a pose: at the beginning of its clip, the rest pose") {
        Stage stage;
        CHECK(stage.pose() == nullptr);
        CHECK(stage.update(0.0f) == AnimationStatistics{.animators = 1, .joints = 3});

        const SkeletonPoseComponent* pose = stage.pose();
        REQUIRE(pose != nullptr);
        REQUIRE(pose->joints.size() == 3);
        REQUIRE(pose->skinning.size() == 3);
        CHECK_NEAR(stage.jointAt(0), Vec3(0.0f), 1.0e-6f);
        CHECK_NEAR(stage.jointAt(1), Vec3(0.0f, 1.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(stage.jointAt(2), Vec3(0.0f, 2.0f, 0.0f), 1.0e-6f);
        // Bound where they stand, the joints leave every vertex where it was modelled.
        for (const Mat4& matrix : pose->skinning) {
            CHECK(test::nearlyEqual(matrix, Mat4(1.0f), 1.0e-6f));
        }
    }

    TEST_CASE("time moves an animator on, and its pose with it") {
        Stage stage;
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.25f));
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.5f));
        // Half way through "Bend" the middle joint is turned an eighth of a turn, and
        // the tip has gone round it.
        CHECK(stage.middleTurn() == doctest::Approx(45.0f).epsilon(1.0e-3));
        const float along = std::sqrt(0.5f);
        CHECK_NEAR(stage.jointAt(2), Vec3(-along, 1.0f + along, 0.0f), 1.0e-5f);
        // What hangs on the tip follows it; what hangs on the root does not move.
        const SkeletonPoseComponent* pose = stage.pose();
        CHECK(test::nearlyEqual(pose->skinning[0], Mat4(1.0f), 1.0e-6f));
        CHECK_NEAR(Vec3(pose->skinning[2] * Vec4(0.0f, 2.0f, 0.0f, 1.0f)), Vec3(-along, 1.0f + along, 0.0f), 1.0e-5f);
    }

    TEST_CASE("a clip that loops starts again, and one that does not stays in its last pose") {
        Stage stage;
        stage.update(0.75f);
        stage.update(0.5f);
        // A second and a quarter into a clip of one second: a quarter into its next run.
        CHECK(stage.animator().time == doctest::Approx(0.25f));
        CHECK(stage.middleTurn() == doctest::Approx(22.5f).epsilon(1.0e-3));
        // However long a step is: seven seconds and a quarter later it is half way.
        stage.update(7.25f);
        CHECK(stage.animator().time == doctest::Approx(0.5f));

        stage.animator().loop = false;
        stage.animator().time = 0.75f;
        stage.update(0.5f);
        CHECK(stage.animator().time == 1.0f);
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));
        stage.update(0.5f);
        CHECK(stage.animator().time == 1.0f);
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));
        // It is still playing: given a clock that runs backwards, it comes back.
        CHECK(stage.animator().playing);
        stage.animator().speed = -1.0f;
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.75f));
        // And stops at the beginning, as it did at the end.
        stage.update(5.0f);
        CHECK(stage.animator().time == 0.0f);
    }

    TEST_CASE("an animator plays as fast as it is told, backwards too, or not at all") {
        Stage stage;
        stage.animator().speed = 2.0f;
        stage.update(0.125f);
        CHECK(stage.animator().time == doctest::Approx(0.25f));

        stage.animator().speed = -1.0f;
        stage.update(0.5f);
        // Backwards past the beginning of a clip that loops is its end.
        CHECK(stage.animator().time == doctest::Approx(0.75f));

        stage.animator().playing = false;
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.75f));
        CHECK(stage.middleTurn() == doctest::Approx(67.5f).epsilon(1.0e-3));

        // A speed that is none is no speed; a step that is none, or backwards, no step.
        stage.animator().playing = true;
        stage.animator().speed = kNotANumber;
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.75f));
        stage.animator().speed = 1.0f;
        stage.update(kNotANumber);
        stage.update(-5.0f);
        stage.update(std::numeric_limits<float>::infinity());
        CHECK(stage.animator().time == doctest::Approx(0.75f));
        CHECK(stage.middleTurn() == doctest::Approx(67.5f).epsilon(1.0e-3));
    }

    TEST_CASE("evaluating poses at the times the animators hold, and changes nothing they hold") {
        Stage stage;
        stage.animator().time = 0.5f;
        CHECK(stage.evaluate() == AnimationStatistics{.animators = 1, .joints = 3});
        CHECK(stage.animator().time == 0.5f);
        CHECK(stage.middleTurn() == doctest::Approx(45.0f).epsilon(1.0e-3));

        // Set to another time, it shows that one: a clip can be looked through by hand.
        // The end of a clip is a time it has, also when it loops.
        stage.animator().time = 1.0f;
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));

        // A time beyond the clip is shown as a clock that ran there would stand - and
        // stays what it was set to: what is in a scene is not put right behind its back.
        stage.animator().time = 2.25f;
        stage.animator().blendClip = stage.sway;
        stage.animator().blendTime = 7.0f;
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(22.5f).epsilon(1.0e-3));
        CHECK(stage.animator().time == 2.25f);
        CHECK(stage.animator().blendTime == 7.0f);
        stage.animator().loop = false;
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));
        CHECK(stage.animator().time == 2.25f);
        // A time that is no number is the beginning.
        stage.animator().time = kNotANumber;
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));
        CHECK(std::isnan(stage.animator().time));

        // When time passes again, the clocks are brought into their clips.
        stage.animator().time = 2.25f;
        stage.animator().loop = true;
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.5f));
        CHECK(stage.animator().blendTime == doctest::Approx(1.25f));
        // But not the clock of an animator that is not playing.
        stage.animator().playing = false;
        stage.animator().time = 2.25f;
        stage.update(0.25f);
        CHECK(stage.animator().time == 2.25f);
        CHECK(stage.middleTurn() == doctest::Approx(22.5f).epsilon(1.0e-3));
    }

    TEST_CASE("an animator without a clip holds the rest pose, and a clip changes only the joints it names") {
        Stage stage;
        stage.animator().clip = Uuid();
        stage.animator().time = 0.5f;
        stage.update(0.25f);
        // No clip, no clock.
        CHECK(stage.animator().time == 0.5f);
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));

        // "Sway" moves and stretches the root; the joints above it follow.
        stage.animator().clip = stage.sway;
        stage.animator().time = 1.5f;
        stage.update(0.0f);
        CHECK_NEAR(stage.jointAt(0), Vec3(1.0f, 0.0f, 0.0f), 1.0e-6f);
        CHECK_NEAR(stage.jointAt(2), Vec3(1.0f, 3.5f, 0.0f), 1.0e-5f);
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));
    }

    TEST_CASE("a second clip is blended over the first by its weight, at a time of its own") {
        Stage stage;
        AnimatorComponent& animator = stage.animator();
        animator.clip = stage.bend;
        animator.time = 1.0f;
        animator.loop = false;
        animator.blendClip = stage.sway;
        animator.blendTime = 1.5f;
        animator.playing = false;

        // None of it: the bend alone.
        animator.blendWeight = 0.0f;
        stage.update(0.0f);
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));
        CHECK_NEAR(stage.jointAt(0), Vec3(0.0f), 1.0e-6f);

        // All of it: the sway alone - the root moved, the middle joint as it rests.
        animator.blendWeight = 1.0f;
        stage.update(0.0f);
        CHECK(stage.middleTurn() == doctest::Approx(0.0f).epsilon(1.0e-3));
        CHECK_NEAR(stage.jointAt(0), Vec3(1.0f, 0.0f, 0.0f), 1.0e-6f);

        // A quarter of it: a quarter of the way from the one to the other, joint by joint.
        animator.blendWeight = 0.25f;
        stage.update(0.0f);
        CHECK(stage.middleTurn() == doctest::Approx(67.5f).epsilon(1.0e-3));
        CHECK_NEAR(stage.jointAt(0), Vec3(0.25f, 0.0f, 0.0f), 1.0e-6f);

        // Both clocks move with the same speed, each inside its own clip - also while
        // the second clip has no weight, so that it is in step when it is faded in.
        animator.playing = true;
        animator.loop = true;
        animator.time = 0.0f;
        animator.blendTime = 0.0f;
        animator.blendWeight = 0.0f;
        stage.update(0.75f);
        stage.update(0.75f);
        CHECK(animator.time == doctest::Approx(0.5f));
        CHECK(animator.blendTime == doctest::Approx(1.5f));
        // A weight that is none, or out of range, is none, or all.
        animator.blendWeight = kNotANumber;
        stage.evaluate();
        CHECK_NEAR(stage.jointAt(0), Vec3(0.0f), 1.0e-6f);
        animator.blendWeight = -2.0f;
        stage.evaluate();
        CHECK_NEAR(stage.jointAt(0), Vec3(0.0f), 1.0e-6f);
        animator.blendWeight = 9.0f;
        stage.evaluate();
        CHECK_NEAR(stage.jointAt(0), Vec3(1.0f, 0.0f, 0.0f), 1.0e-6f);

        // A second clip alone, without a first, is blended over the rest pose.
        animator.clip = Uuid();
        animator.blendTime = 1.5f;
        animator.blendWeight = 0.5f;
        stage.evaluate();
        CHECK_NEAR(stage.jointAt(0), Vec3(0.5f, 0.0f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("a pose goes when its animator does: removed, switched off, or left without a skeleton") {
        Stage stage;
        stage.update(0.1f);
        REQUIRE(stage.pose() != nullptr);

        stage.animator().skeleton = Uuid();
        CHECK(stage.update(0.1f) == AnimationStatistics());
        CHECK(stage.pose() == nullptr);
        stage.animator().skeleton = stage.skeleton;
        stage.update(0.1f);
        REQUIRE(stage.pose() != nullptr);

        // Switched off - itself, or something it hangs under - it is not posed, and
        // its clock stands.
        const float before = stage.animator().time;
        const Entity group = stage.world.createEntity("Group");
        REQUIRE(stage.world.setParent(stage.figure, group).has_value());
        stage.world.setEnabled(group, false);
        CHECK(stage.update(0.1f) == AnimationStatistics());
        CHECK(stage.pose() == nullptr);
        CHECK(stage.animator().time == before);
        stage.world.setEnabled(group, true);
        stage.update(0.1f);
        CHECK(stage.pose() != nullptr);

        stage.world.remove<AnimatorComponent>(stage.figure);
        stage.evaluate();
        CHECK(stage.pose() == nullptr);

        // Several animators, each with a pose and a clock of its own.
        const Entity second = stage.world.createEntity("Second");
        stage.world.add<AnimatorComponent>(second) = {.skeleton = stage.skeleton, .clip = stage.sway, .time = 1.0f};
        stage.world.add<AnimatorComponent>(stage.figure) = {.skeleton = stage.skeleton, .clip = stage.bend};
        CHECK(stage.update(0.5f) == AnimationStatistics{.animators = 2, .joints = 6});
        CHECK(stage.world.tryGet<AnimatorComponent>(second)->time == doctest::Approx(1.5f));
        CHECK(stage.animator().time == doctest::Approx(0.5f));
        CHECK_NEAR(Vec3(stage.world.tryGet<SkeletonPoseComponent>(second)->joints[0][3]), Vec3(1.0f, 0.0f, 0.0f),
                   1.0e-6f);
        CHECK_NEAR(stage.jointAt(0), Vec3(0.0f), 1.0e-6f);

        // One of the two goes: the other keeps its pose.
        stage.world.remove<AnimatorComponent>(second);
        CHECK(stage.update(0.0f) == AnimationStatistics{.animators = 1, .joints = 3});
        CHECK(stage.world.tryGet<SkeletonPoseComponent>(second) == nullptr);
        CHECK(stage.pose() != nullptr);
        // An entity that is destroyed takes its pose along.
        stage.world.destroyEntity(stage.figure);
        CHECK(stage.update(0.0f) == AnimationStatistics());
    }

    TEST_CASE("an asset an animator cannot use is reported once, and nothing else stops") {
        Stage stage;
        const LogCapture log;

        // A clip that is no clip: the skeleton is posed at rest, and the clock stands.
        stage.animator().clip = stage.skeleton;
        stage.animator().time = 0.25f;
        stage.update(0.1f);
        stage.update(0.1f);
        REQUIRE(stage.pose() != nullptr);
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));
        CHECK(stage.animator().time == 0.25f);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("an animator cannot use the asset " + stage.skeleton.toString()));
        CHECK(log.contains("bar.gltf#skeleton/0 is a skeleton, not an animation"));

        // A skeleton that is none: no pose.
        stage.animator().clip = stage.bend;
        stage.animator().skeleton = stage.bend;
        stage.update(0.1f);
        stage.update(0.1f);
        CHECK(stage.pose() == nullptr);
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("bar.gltf#animation/0 is an animation, not a skeleton"));

        // An id nothing has.
        stage.animator().skeleton = *Uuid::parse("11111111-1111-4111-8111-111111111111");
        stage.update(0.1f);
        stage.update(0.1f);
        CHECK(stage.pose() == nullptr);
        CHECK(log.count(LogLevel::Error) == 3);
        CHECK(log.contains("there is no asset with the id 11111111-1111-4111-8111-111111111111"));

        // Put right, it plays.
        stage.animator().skeleton = stage.skeleton;
        stage.update(0.1f);
        CHECK(stage.pose() != nullptr);
        CHECK(log.count(LogLevel::Error) == 3);
    }

    TEST_CASE("a clip made for other joints moves none of these") {
        Stage stage;
        // The same bar, its joints renamed, in a second file: its clips name joints
        // that the first skeleton does not have.
        GltfDocument other = test::makeSkinnedBar();
        other.nodes[test::kBarMiddleNode].name = "Elbow";
        other.animations[0].animation.tracks[0].joint = "Elbow";
        other.skins[0].skeleton.joints[1].name = "Elbow";
        REQUIRE(exportGltf(other, stage.directory.path() / "assets" / "arm.gltf").has_value());
        REQUIRE(stage.assets->refresh().has_value());

        stage.animator().clip = stage.idOf("arm.gltf#animation/0");
        stage.animator().time = 0.5f;
        stage.update(0.0f);
        REQUIRE(stage.pose() != nullptr);
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));
        // The clock runs all the same: the clip is playing, it has nothing to say here.
        stage.update(0.25f);
        CHECK(stage.animator().time == doctest::Approx(0.75f));

        // On the skeleton it was made for, it bends - and the same clip goes on moving
        // nothing of the other skeleton, in the same world.
        const Entity arm = stage.world.createEntity("Arm");
        stage.world.add<AnimatorComponent>(arm) = {.skeleton = stage.idOf("arm.gltf#skeleton/0"),
                                                   .clip = stage.idOf("arm.gltf#animation/0"),
                                                   .time = 0.75f,
                                                   .playing = false};
        CHECK(stage.update(0.0f) == AnimationStatistics{.animators = 2, .joints = 6});
        const SkeletonPoseComponent* armPose = stage.world.tryGet<SkeletonPoseComponent>(arm);
        REQUIRE(armPose != nullptr);
        CHECK(degreesOf(armPose->joints[1]) == doctest::Approx(67.5f).epsilon(1.0e-3));
        CHECK(stage.middleTurn() == doctest::Approx(0.0f));
    }

    TEST_CASE("assets that were refreshed are read again") {
        Stage stage;
        stage.animator().time = 1.0f;
        stage.animator().loop = false;
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));

        // The file is written again with a clip that turns half as far.
        GltfDocument changed = test::makeSkinnedBar();
        Curve<Vec4>& rotation = changed.animations[0].animation.tracks[0].rotation;
        rotation.clear();
        for (const float time : {0.0f, 1.0f}) {
            const Quat turn = glm::angleAxis(time * kHalfPi * 0.5f, Vec3(0.0f, 0.0f, 1.0f));
            rotation.addKey(time, Vec4(turn.x, turn.y, turn.z, turn.w));
        }
        REQUIRE(exportGltf(changed, stage.directory.path() / "assets" / "bar.gltf").has_value());

        // Until the assets are refreshed, what was loaded is what plays.
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(90.0f).epsilon(1.0e-3));
        REQUIRE(stage.assets->refresh().has_value());
        stage.evaluate();
        CHECK(stage.middleTurn() == doctest::Approx(45.0f).epsilon(1.0e-3));

        // clear() forgets what was reported, too: it is said again.
        const LogCapture log;
        stage.animator().clip = stage.skeleton;
        stage.evaluate();
        stage.evaluate();
        CHECK(log.count(LogLevel::Error) == 1);
        stage.system.clear();
        stage.evaluate();
        CHECK(log.count(LogLevel::Error) == 2);
    }
}

TEST_SUITE("AnimationComponents") {
    TEST_CASE("the animator is registered under the name scene files use, and described") {
        ComponentRegistry registry;
        REQUIRE(registerAnimationComponents(registry).has_value());
        // The animator first, then the shake of a camera (CameraShakeTests) and the player of
        // a sequence (SequenceSystemTests).
        REQUIRE(registry.components().size() == 3);
        CHECK(registry.components()[0]->type.name() == "Animator");
        const ComponentDescriptor* descriptor = registry.find("Animator");
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->traits.optional);
        CHECK_FALSE(descriptor->traits.description.empty());
        for (const PropertyInfo& property : descriptor->type.properties()) {
            CAPTURE(property.name());
            CHECK_FALSE(property.attributes().description.empty());
        }
        // The pose is worked out, not kept: a scene knows nothing of it.
        CHECK(registry.find("SkeletonPose") == nullptr);
        // Registered once.
        CHECK_FALSE(registerAnimationComponents(registry).has_value());

        const TypeInfo& type = descriptor->type;
        REQUIRE(type.properties().size() == 9);
        for (const char* name : {"skeleton", "clip", "blendClip"}) {
            const PropertyInfo* property = type.findProperty(name);
            REQUIRE(property != nullptr);
            CHECK(property->attributes().semantic == PropertySemantic::AssetReference);
        }
        CHECK(type.findProperty("skeleton")->attributes().referenceType == "Skeleton");
        CHECK(type.findProperty("clip")->attributes().referenceType == "Animation");
        CHECK(type.findProperty("blendClip")->attributes().referenceType == "Animation");
        CHECK(type.findProperty("time")->attributes().unit == "s");
        CHECK(type.findProperty("blendTime")->attributes().unit == "s");
    }

    TEST_CASE("the pose of a mesh is that of its entity, or of the nearest entity above it that has one") {
        World world;
        const Entity figure = world.createEntity("Figure");
        const Entity arm = world.createEntity("Arm");
        const Entity hand = world.createEntity("Hand");
        const Entity bystander = world.createEntity("Bystander");
        REQUIRE(world.setParent(arm, figure).has_value());
        REQUIRE(world.setParent(hand, arm).has_value());

        // Nobody has one.
        Entity owner = hand;
        CHECK(findSkeletonPose(world, hand, &owner) == nullptr);
        CHECK(owner.isNull());
        CHECK(findSkeletonPose(world, hand) == nullptr);

        // The figure has one: everything below it is drawn in it, and nothing beside it.
        const SkeletonPoseComponent* ofFigure = &world.add<SkeletonPoseComponent>(figure);
        CHECK(findSkeletonPose(world, figure, &owner) == ofFigure);
        CHECK(owner == figure);
        CHECK(findSkeletonPose(world, hand, &owner) == ofFigure);
        CHECK(owner == figure);
        CHECK(findSkeletonPose(world, bystander) == nullptr);

        // The nearest wins: what hangs under an arm with a pose of its own is drawn in
        // that one. (Adding a component may move the others: they are asked for again.)
        world.add<SkeletonPoseComponent>(arm);
        CHECK(findSkeletonPose(world, hand, &owner) == world.tryGet<SkeletonPoseComponent>(arm));
        CHECK(owner == arm);
        CHECK(findSkeletonPose(world, figure, &owner) == world.tryGet<SkeletonPoseComponent>(figure));
        CHECK(owner == figure);

        // An entity that is no more, and no entity at all, have none.
        world.destroyEntity(hand);
        CHECK(findSkeletonPose(world, hand, &owner) == nullptr);
        CHECK(owner.isNull());
        CHECK(findSkeletonPose(world, Entity{}) == nullptr);
    }

    TEST_CASE("an animator is saved with every property and read back the same") {
        ComponentRegistry registry;
        REQUIRE(registerAnimationComponents(registry).has_value());
        const TypeInfo& type = registry.find("Animator")->type;

        // A new one plays what it is given, from the beginning, round and round.
        const AnimatorComponent fresh;
        CHECK(type.toJson(&fresh) == unwrap(parseJson(R"({"skeleton": null, "clip": null,
                                                         "time": 0.0, "speed": 1.0, "loop": true, "playing": true,
                                                         "blendClip": null, "blendTime": 0.0, "blendWeight": 0.0})")));

        const AnimatorComponent animator{.skeleton = *Uuid::parse("0a56c2f1-21e0-819d-8bac-567d1e1c4345"),
                                         .clip = *Uuid::parse("5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79"),
                                         .time = 1.5f,
                                         .speed = -0.5f,
                                         .loop = false,
                                         .playing = false,
                                         .blendClip = *Uuid::parse("3f0c8d5e-6f0a-4f38-9d3b-1b2a7c0de481"),
                                         .blendTime = 0.25f,
                                         .blendWeight = 0.75f};
        const Json saved = type.toJson(&animator);
        CHECK(saved == unwrap(parseJson(R"({"skeleton": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
                                           "clip": "5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79",
                                           "time": 1.5, "speed": -0.5, "loop": false, "playing": false,
                                           "blendClip": "3f0c8d5e-6f0a-4f38-9d3b-1b2a7c0de481",
                                           "blendTime": 0.25, "blendWeight": 0.75})")));

        AnimatorComponent loaded;
        REQUIRE(type.fromJson(&loaded, saved).has_value());
        CHECK(loaded.skeleton == animator.skeleton);
        CHECK(loaded.clip == animator.clip);
        CHECK(loaded.time == 1.5f);
        CHECK(loaded.speed == -0.5f);
        CHECK_FALSE(loaded.loop);
        CHECK_FALSE(loaded.playing);
        CHECK(loaded.blendClip == animator.blendClip);
        CHECK(loaded.blendTime == 0.25f);
        CHECK(loaded.blendWeight == 0.75f);

        // What is out of range, of the wrong kind or not a property is refused, and
        // refused whole.
        for (const char* text : {
                 R"({"time": -1.0})",
                 R"({"speed": 101.0})",
                 R"({"speed": -100.5})",
                 R"({"blendWeight": 1.5})",
                 R"({"blendWeight": -0.1})",
                 R"({"blendTime": -2.0})",
                 R"({"skeleton": "bar.gltf"})",
                 R"({"clip": 3})",
                 R"({"loop": "yes"})",
                 R"({"pose": []})",
                 R"({"time": 2.0, "blendWeight": 4.0})",
             }) {
            CAPTURE(std::string(text));
            CHECK_FALSE(type.fromJson(&loaded, unwrap(parseJson(text))).has_value());
            CHECK(loaded.time == 1.5f);
            CHECK(loaded.blendWeight == 0.75f);
        }
    }
}
