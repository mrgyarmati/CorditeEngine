#include "cordite/animation/AnimationComponents.h"

#include "cordite/core/Reflection.h"

namespace cordite {
namespace {

/// The longest time a clock can be set to, in seconds. A clock that runs is kept inside
/// its clip, so this only bounds what is typed in or read from a file.
constexpr double kLongestTime = 1000000.0;

TypeInfo describeAnimator() {
    return TypeBuilder<AnimatorComponent>("Animator")
        .property("skeleton", &AnimatorComponent::skeleton,
                  {.description = "The skeleton that is posed; without one there is no pose",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Skeleton"})
        .property("clip", &AnimatorComponent::clip,
                  {.description = "The animation that is playing; without one the skeleton is at rest. A clip "
                                  "moves the joints that have the names its tracks name",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Animation"})
        .property("time", &AnimatorComponent::time,
                  {.description = "Where in the clip the pose is taken from; it moves on while the animator is "
                                  "playing",
                   .minimum = 0.0,
                   .maximum = kLongestTime,
                   .step = 0.01,
                   .unit = "s"})
        .property("speed", &AnimatorComponent::speed,
                  {.description = "How fast the time moves: 1 as the clip was made, 2 twice as fast, negative "
                                  "backwards",
                   .minimum = -100.0,
                   .maximum = 100.0,
                   .step = 0.01})
        .property("loop", &AnimatorComponent::loop,
                  {.description = "At its end the clip starts again; without, it stays in its last pose"})
        .property("playing", &AnimatorComponent::playing,
                  {.description = "Whether the time moves on; without, the pose is that of `time`, and stays"})
        .property("blendClip", &AnimatorComponent::blendClip,
                  {.description = "A second animation, blended over the first by `blendWeight`",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Animation"})
        .property("blendTime", &AnimatorComponent::blendTime,
                  {.description = "Where in the second clip its pose is taken from; it moves on with the same speed",
                   .minimum = 0.0,
                   .maximum = kLongestTime,
                   .step = 0.01,
                   .unit = "s"})
        .property("blendWeight", &AnimatorComponent::blendWeight,
                  {.description = "How much of the second clip is taken: 0 none of it, 1 nothing but it",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .build();
}

} // namespace

const SkeletonPoseComponent* findSkeletonPose(const World& world, Entity entity, Entity* owner) {
    const SkeletonPoseComponent* pose = nullptr;
    Entity at = entity;
    while (world.isValid(at)) {
        pose = world.tryGet<SkeletonPoseComponent>(at);
        if (pose != nullptr) {
            break;
        }
        at = world.parent(at);
    }
    if (owner != nullptr) {
        *owner = pose != nullptr ? at : Entity{};
    }
    return pose;
}

Status registerAnimationComponents(ComponentRegistry& registry) {
    return registry.add<AnimatorComponent>(
        describeAnimator(), {.description = "Poses a skeleton: which one, the animation that is playing, and where "
                                            "in it the pose is taken from."});
}

} // namespace cordite
