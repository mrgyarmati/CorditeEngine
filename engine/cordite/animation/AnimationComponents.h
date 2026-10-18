#pragma once

/// @file
/// The components of animation: the animator, the pose it works out, the shake of a
/// camera and the player of a sequence.
///
/// @code
/// const Entity figure = world.createEntity("Figure");
/// world.add<AnimatorComponent>(figure) = {.skeleton = figureSkeletonId, .clip = walkId};
///
/// animation.update(world, assets, frameSeconds);                      // AnimationSystem
/// const SkeletonPoseComponent* pose = world.tryGet<SkeletonPoseComponent>(figure);
/// const SkeletonPoseComponent* sameOne = findSkeletonPose(world, meshBelowFigure);
///
/// world.add<CameraShakeComponent>(camera).trauma = 0.5f;               // CameraShake.h
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "Animator": {"skeleton": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
///              "clip": "5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79",
///              "time": 0, "speed": 1, "loop": true, "playing": true,
///              "blendClip": null, "blendTime": 0, "blendWeight": 0},
/// "CameraShake": {"decay": 1, "maxOffset": [0.05, 0.05, 0.05], "maxRotation": [3, 3, 2],
///                 "frequency": 12},
/// "SequencePlayer": {"sequence": "6f1c2e4a-0b3d-4c5e-9f70-8a1b2c3d4e5f", "time": 0, "speed": 1,
///                    "loop": false, "playing": false}
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

/// Shakes the view through the Camera of its entity by how much has happened to it: its
/// trauma. The shake moves the picture alone - the entity stays where it is, and so does
/// whatever is worked out from it: where its rays start, where it is heard from. How it
/// moves the view at a moment is cameraShakeOffset, and shakeCameras lets time pass for
/// it (CameraShake.h). What a scene holds is how the camera shakes; that it does, and
/// where in its shaking it is, are the game's: neither is saved.
struct CameraShakeComponent {
    /// How much has happened to the camera, from nothing to the most there can be. A game
    /// raises it when something does - an explosion, a landing - and it falls by `decay`
    /// every second while time passes. The camera is moved by its square, so a little is
    /// hardly seen and a lot is violent.
    float trauma = 0.0f;
    /// How much trauma goes away every second.
    float decay = 1.0f;
    /// The furthest the camera is moved along its own axes - right, up and back - at a
    /// trauma of 1, in metres.
    Vec3 maxOffset{0.05f, 0.05f, 0.05f};
    /// The furthest the camera is turned about its own axes at a trauma of 1, in degrees:
    /// about X it looks up and down, about Y from side to side, and about Z it rolls.
    Vec3 maxRotation{3.0f, 3.0f, 2.0f};
    /// How fast it shakes: about how many times a second it changes its way.
    float frequency = 12.0f;
    /// Where in its shaking the camera is, in turns of the noise it is shaken by: it moves
    /// on by `frequency` every second while time passes, and goes round at
    /// kCameraShakePeriod.
    float phase = 0.0f;
};

/// Plays a sequence (assets/Sequence.h) on its entity, while the game's time passes: the
/// paths of its tracks set out from here, and its events are told to the scripts of this
/// entity. What it plays is written into the world - transforms, properties of
/// components - so a program that only shows a scene, an editor, does not play it
/// (SequenceSystem.h).
struct SequencePlayerComponent {
    /// The sequence asset. Nil is none, and nothing plays.
    Uuid sequence;
    /// Where in the sequence it is, in seconds. It moves on while the player plays; set,
    /// it is where the sequence goes on from - what it passes over is not told.
    float time = 0.0f;
    /// How fast the time moves: 1 as the sequence was made, 2 twice as fast, 0 not at all.
    float speed = 1.0f;
    /// At its end the sequence begins again; without, it stops there, as its last keys
    /// leave everything.
    bool loop = false;
    /// Whether it plays: its time moves on and what it says is put into the world. One
    /// that does not loop stops itself at its end.
    bool playing = true;
    /// The camera the game is seen through while it plays - what its camera track says at
    /// its time - and nil for none: the game's own (findGameCamera, renderer/CameraView.h).
    /// The system's to say; not saved.
    Uuid camera;
};

/// The pose a skinned mesh of `entity` is drawn in: that of the entity itself, or of the
/// nearest entity above it that has one - the meshes of a figure hang under what
/// animates it. Null when there is none. `owner`, when given, is told whose pose it is;
/// the null entity for none.
[[nodiscard]] const SkeletonPoseComponent* findSkeletonPose(const World& world, Entity entity, Entity* owner = nullptr);

/// After this many turns the noise a camera is shaken by begins again, without a jump:
/// CameraShakeComponent::phase stays below it, where a float still has the digits that
/// a sixtieth of a second moves it by.
inline constexpr float kCameraShakePeriod = 256.0f;

/// Registers the components of the animation module that a scene holds: "Animator",
/// "CameraShake" and "SequencePlayer".
[[nodiscard]] Status registerAnimationComponents(ComponentRegistry& registry);

} // namespace cordite
