#pragma once

/// @file
/// The components of animation: the animator, and the pose it works out.
///
/// @code
/// const Entity figure = world.createEntity("Figure");
/// world.add<AnimatorComponent>(figure) = {.skeleton = figureSkeletonId, .clip = walkId};
///
/// animation.update(world, assets, frameSeconds);                      // AnimationSystem
/// const SkeletonPoseComponent* pose = world.tryGet<SkeletonPoseComponent>(figure);
/// const SkeletonPoseComponent* sameOne = findSkeletonPose(world, meshBelowFigure);
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "Animator": {"skeleton": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
///              "clip": "5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79",
///              "time": 0, "speed": 1, "loop": true, "playing": true,
///              "blendClip": null, "blendTime": 0, "blendWeight": 0}
/// @endcode
///
/// The joints of a skeleton are not entities. A skeleton is an asset, and where its
/// joints are at a moment is worked out from the animator into a SkeletonPose, which is
/// neither saved nor edited: playing an animation changes nothing a scene file holds
/// but the animator's own clocks.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <vector>

namespace cordite {

/// Poses a skeleton: says which one, which clip is playing, and where in the clip it is.
struct AnimatorComponent {
    /// The skeleton asset that is posed. Nil is none, and no pose.
    Uuid skeleton;
    /// The animation asset that is playing. Nil is none: the skeleton is at rest. A
    /// clip moves the joints of the skeleton that have the names its tracks name.
    Uuid clip;
    /// Where in the clip the pose is taken from, in seconds. It moves on while the
    /// animator is playing; setting it shows the clip at that time. A time beyond the
    /// clip is taken as a clock that ran there would stand: round and round for a clip
    /// that loops, at the end for one that does not.
    float time = 0.0f;
    /// How fast the time moves: 1 as the clip was made, 2 twice as fast, 0.5 half as
    /// fast, negative backwards.
    float speed = 1.0f;
    /// At its end the clip starts again; without, it stays in its last pose.
    bool loop = true;
    /// Whether the time moves on. Without, the pose is that of `time`, and stays.
    bool playing = true;
    /// A second clip, blended over the first - a run over a walk, the pose to fade
    /// to - at its own time, which moves on with the same speed; nil for none.
    Uuid blendClip;
    float blendTime = 0.0f;
    /// How much of the second clip is taken: 0 none of it, 1 nothing but it.
    float blendWeight = 0.0f;
};

/// The pose an animator has worked out. AnimationSystem gives it to every entity whose
/// animator has a skeleton, each time it runs, and takes it away again; it is not in the
/// component registry, so it is not saved, not shown and not edited.
struct SkeletonPoseComponent {
    /// For every joint of the skeleton, the matrix that takes a vertex of a mesh bound
    /// to the skeleton from where it was modelled to where the joint has it now.
    std::vector<Mat4> skinning;
    /// Where every joint is, in the space the skeleton was modelled in.
    std::vector<Mat4> joints;
};

/// The pose a skinned mesh of `entity` is drawn in: that of the entity itself, or of the
/// nearest entity above it that has one - the meshes of a figure hang under what
/// animates it. Null when there is none. `owner`, when given, is told whose pose it is;
/// the null entity for none.
[[nodiscard]] const SkeletonPoseComponent* findSkeletonPose(const World& world, Entity entity, Entity* owner = nullptr);

/// Registers the components of the animation module that a scene holds: "Animator".
[[nodiscard]] Status registerAnimationComponents(ComponentRegistry& registry);

} // namespace cordite
