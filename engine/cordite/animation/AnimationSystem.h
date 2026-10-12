#pragma once

/// @file
/// Plays the animations of a world: moves the clock of every animator on, and works out
/// the pose it is in.
///
/// @code
/// AnimationSystem animation;
///
/// // A game, every frame, before anything is drawn:
/// animation.update(world, assets, frameSeconds);
///
/// // A program that shows a scene without running it - an editor: the clocks stand
/// // still, and the poses are those of the times the animators hold.
/// animation.evaluate(world, assets);
/// @endcode
///
/// Both give every enabled entity whose Animator has a skeleton a SkeletonPose, and take
/// the pose away from entities that have no such animator any more. Skeletons and clips
/// are loaded when an animator first names them; one that cannot be loaded is reported
/// once, and an animator without a usable skeleton poses nothing.

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/Animation.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Transform.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

/// What the animations of a world came to, the last time they were worked out.
struct AnimationStatistics {
    /// Animators that were given a pose.
    uint32_t animators = 0;
    /// The joints of all those poses together.
    uint32_t joints = 0;

    bool operator==(const AnimationStatistics&) const = default;
};

class AnimationSystem {
public:
    /// Lets `deltaSeconds` pass: moves the clocks of the animators that are playing on
    /// by it, each at its own speed, and poses every skeleton at the time its animator
    /// then holds. Less than no time, and a time that is no number, is no time. Returns
    /// what it posed.
    AnimationStatistics update(World& world, AssetManager& assets, float deltaSeconds);

    /// Poses every skeleton at the time its animator holds, and changes nothing an
    /// animator holds - not even a time that lies beyond its clip, which is taken as
    /// AnimatorComponent::time says. Returns what it posed.
    AnimationStatistics evaluate(World& world, AssetManager& assets);

    /// Forgets the skeletons and clips it holds, and what it has reported: they are
    /// loaded again when next needed. update() and evaluate() do that themselves when
    /// the assets were refreshed.
    void clear();

private:
    /// A skeleton, with the order to walk its joints in. Without data for one that
    /// could not be loaded.
    struct Skeleton {
        std::shared_ptr<const SkeletonData> data;
        std::vector<uint32_t> order;
    };

    /// A clip as it moves one skeleton: which joint each of its tracks is for.
    struct BoundClip {
        std::shared_ptr<const AnimationData> data;
        std::vector<int32_t> jointOfTrack;
    };

    /// `step` is the time that passes; none when the clocks are not to be touched.
    AnimationStatistics run(World& world, AssetManager& assets, std::optional<float> step);
    /// Works out the pose of an animator into m_pose, moving its clocks on first when
    /// there is a step and it is playing.
    void poseAnimator(AnimatorComponent& animator, const Skeleton& skeleton, std::optional<float> step,
                      AssetManager& assets);
    /// Takes the pose away from every entity that is not in m_posed.
    void removeOtherPoses(World& world);

    [[nodiscard]] const Skeleton& skeletonFor(const Uuid& id, AssetManager& assets);
    /// Null for no clip, and for one that cannot be loaded.
    [[nodiscard]] const BoundClip* clipFor(const Uuid& clip, const Uuid& skeletonId, const Skeleton& skeleton,
                                           AssetManager& assets);
    /// Says once what is wrong with an asset an animator names.
    void report(const Uuid& id, const Error& error);

    std::unordered_map<Uuid, Skeleton> m_skeletons;
    /// By the clip, and by the skeleton it is bound to.
    std::unordered_map<Uuid, std::unordered_map<Uuid, BoundClip>> m_clips;
    std::unordered_set<Uuid> m_reported;
    /// AssetDatabase::revision() when the above was loaded.
    uint64_t m_assetRevision = 0;
    /// Working memory, kept so that it is not allocated every frame.
    std::vector<Transform> m_pose;
    std::vector<Transform> m_blendPose;
    /// The entities that were given a pose in this run.
    std::vector<Entity> m_posed;
    std::vector<Entity> m_stale;
};

} // namespace cordite
