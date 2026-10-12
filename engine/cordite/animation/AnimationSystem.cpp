#include "cordite/animation/AnimationSystem.h"

#include "cordite/animation/Pose.h"
#include "cordite/core/Log.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cordite {

void AnimationSystem::clear() {
    m_skeletons.clear();
    m_clips.clear();
    m_reported.clear();
}

void AnimationSystem::report(const Uuid& id, const Error& error) {
    if (m_reported.insert(id).second) {
        CORDITE_LOG_ERROR("Animation", "an animator cannot use the asset {}: {}", id.toString(), error.describe());
    }
}

const AnimationSystem::Skeleton& AnimationSystem::skeletonFor(const Uuid& id, AssetManager& assets) {
    if (const auto found = m_skeletons.find(id); found != m_skeletons.end()) {
        return found->second;
    }
    Skeleton skeleton;
    if (Result<std::shared_ptr<const SkeletonData>> loaded = assets.loadSkeleton(id)) {
        skeleton.data = std::move(*loaded);
        skeleton.order = jointOrder(*skeleton.data);
    } else {
        report(id, loaded.error());
    }
    // Kept also when there is nothing to keep, so that it is not loaded again every frame.
    return m_skeletons.emplace(id, std::move(skeleton)).first->second;
}

const AnimationSystem::BoundClip* AnimationSystem::clipFor(const Uuid& clip, const Uuid& skeletonId,
                                                           const Skeleton& skeleton, AssetManager& assets) {
    if (clip.isNil() || !skeleton.data) {
        return nullptr;
    }
    std::unordered_map<Uuid, BoundClip>& bySkeleton = m_clips[clip];
    auto found = bySkeleton.find(skeletonId);
    if (found == bySkeleton.end()) {
        BoundClip bound;
        if (Result<std::shared_ptr<const AnimationData>> loaded = assets.loadAnimation(clip)) {
            bound.data = std::move(*loaded);
            bound.jointOfTrack = bindTracks(*bound.data, *skeleton.data);
        } else {
            report(clip, loaded.error());
        }
        found = bySkeleton.emplace(skeletonId, std::move(bound)).first;
    }
    return found->second.data ? &found->second : nullptr;
}

void AnimationSystem::poseAnimator(AnimatorComponent& animator, const Skeleton& skeleton, std::optional<float> step,
                                   AssetManager& assets) {
    const SkeletonData& data = *skeleton.data;
    // A clock is written only when it moves: an animator that is not playing, and every
    // animator of a scene that is only being looked at, keeps what it holds.
    const bool moves = step.has_value() && animator.playing;
    const float moved = moves && std::isfinite(animator.speed) ? *step * animator.speed : 0.0f;
    const auto timeIn = [&](const BoundClip& clip, float& clock) {
        const float time = clipTime((std::isfinite(clock) ? clock : 0.0f) + moved, clip.data->duration, animator.loop);
        if (moves) {
            clock = time;
        }
        return time;
    };

    restPose(data, m_pose);
    if (const BoundClip* clip = clipFor(animator.clip, animator.skeleton, skeleton, assets)) {
        sampleClip(*clip->data, clip->jointOfTrack, timeIn(*clip, animator.time), m_pose);
    }
    if (const BoundClip* blend = clipFor(animator.blendClip, animator.skeleton, skeleton, assets)) {
        // Its clock moves on whatever its weight is: a clip that is faded in is in step.
        const float time = timeIn(*blend, animator.blendTime);
        const float weight = std::isfinite(animator.blendWeight) ? std::clamp(animator.blendWeight, 0.0f, 1.0f) : 0.0f;
        if (weight > 0.0f) {
            restPose(data, m_blendPose);
            sampleClip(*blend->data, blend->jointOfTrack, time, m_blendPose);
            blendPoses(m_pose, m_blendPose, weight, m_pose);
        }
    }
}

void AnimationSystem::removeOtherPoses(World& world) {
    const auto poses = world.registry().view<SkeletonPoseComponent>();
    if (poses.size() == m_posed.size()) {
        // Every pose there is was made in this run.
        return;
    }
    const std::unordered_set<Entity> posed(m_posed.begin(), m_posed.end());
    std::vector<Entity> stale;
    for (const entt::entity handle : poses) {
        if (!posed.contains(Entity{handle})) {
            stale.push_back(Entity{handle});
        }
    }
    for (const Entity entity : stale) {
        world.remove<SkeletonPoseComponent>(entity);
    }
}

AnimationStatistics AnimationSystem::run(World& world, AssetManager& assets, std::optional<float> step) {
    if (const uint64_t revision = assets.database().revision(); revision != m_assetRevision) {
        clear();
        m_assetRevision = revision;
    }

    AnimationStatistics statistics;
    m_posed.clear();
    entt::registry& registry = world.registry();
    const auto animators = registry.view<AnimatorComponent>();
    for (const auto [handle, animator] : animators.each()) {
        if (animator.skeleton.isNil() || !world.isEnabledInHierarchy(Entity{handle})) {
            continue;
        }
        const Skeleton& skeleton = skeletonFor(animator.skeleton, assets);
        if (!skeleton.data) {
            continue;
        }
        poseAnimator(animator, skeleton, step, assets);

        const size_t jointCount = skeleton.data->joints.size();
        SkeletonPoseComponent& pose = registry.get_or_emplace<SkeletonPoseComponent>(handle);
        pose.joints.resize(jointCount);
        pose.skinning.resize(jointCount);
        jointMatrices(*skeleton.data, skeleton.order, m_pose, pose.joints);
        skinningMatrices(*skeleton.data, pose.joints, pose.skinning);

        m_posed.push_back(Entity{handle});
        ++statistics.animators;
        statistics.joints += static_cast<uint32_t>(jointCount);
    }
    // A pose whose animator has gone, or has been switched off, is a pose of the past.
    removeOtherPoses(world);
    return statistics;
}

AnimationStatistics AnimationSystem::update(World& world, AssetManager& assets, float deltaSeconds) {
    return run(world, assets, std::isfinite(deltaSeconds) ? std::max(deltaSeconds, 0.0f) : 0.0f);
}

AnimationStatistics AnimationSystem::evaluate(World& world, AssetManager& assets) {
    return run(world, assets, std::nullopt);
}

} // namespace cordite
