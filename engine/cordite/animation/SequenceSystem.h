#pragma once

/// @file
/// Plays the sequences of a world: moves the time of every SequencePlayer that plays on,
/// puts what its tracks say at that time into the transforms and the components they
/// name, says which camera it has cut to, and which of its events the time passed.
///
/// @code
/// SequenceSystem sequences;
///
/// // A game, every frame, after its scripts and before its simulation:
/// sequences.update(world, components, assets, frameSeconds);
/// scripts.tellSequenceEvents(context, sequences.events());
/// // The view is through the camera a sequence has cut to, while it plays:
/// const Entity camera = findGameCamera(world);   // renderer/CameraView.h
/// @endcode
///
/// Nothing is played while no time passes: a program that shows a scene without running
/// it - an editor - does not call update(), for what a sequence writes would change the
/// scene behind the back of its history. A sequence that cannot be loaded, and a track
/// whose target cannot be found or written, is reported once and does nothing.

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Sequence.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

/// An event of a sequence that the time of its player passed.
struct SequenceEvent {
    /// The entity whose SequencePlayer played the sequence: whose scripts are told.
    Entity player;
    /// Its name, as the sequence file has it.
    std::string name;

    bool operator==(const SequenceEvent&) const = default;
};

/// What the sequences of a world came to in an update.
struct SequenceStatistics {
    /// Players that were playing.
    uint32_t players = 0;
    /// Tracks that were put into the world.
    uint32_t tracks = 0;

    bool operator==(const SequenceStatistics&) const = default;
};

class SequenceSystem {
public:
    /// Lets `deltaSeconds` pass for every SequencePlayer of an enabled entity that is
    /// playing, in the order of the hierarchy: its time moves on by it, at its speed; the
    /// events it passed - from where it was, up to where it got to - are listed in
    /// events(), in order; and what every track says at the new time is put into the
    /// world. One that does not loop stops at its end, with what its last keys say;
    /// one that loops goes round, and a long frame passes every event once at the most.
    /// Every player's `camera` is set to the cut its sequence is at, and to none for a
    /// player that does not play. Less than no time, and a time that is no number, is
    /// no time: the tracks are put into the world all the same. Returns what was played.
    SequenceStatistics update(World& world, const ComponentRegistry& components, AssetManager& assets,
                              float deltaSeconds);

    /// The events the last update() passed. Valid until the next update() or clear().
    [[nodiscard]] std::span<const SequenceEvent> events() const { return m_events; }

    /// Forgets the sequences it holds and what it has reported; they are loaded again when
    /// next needed. update() does that itself when the assets were refreshed.
    void clear();

private:
    /// A sequence, or nothing for one that cannot be loaded.
    [[nodiscard]] std::shared_ptr<const SequenceData> sequenceFor(const Uuid& id, AssetManager& assets);
    /// Moves the time of a player on by `step`, lists the events it passes and stops one
    /// that does not loop at its end. Returns the time the sequence is at.
    float advance(Entity entity, SequencePlayerComponent& player, const SequenceData& sequence, float step);
    /// Lists the events from `begin` on - at it included - up to `end`, which is included
    /// where `toEnd` says so.
    void pass(Entity entity, const SequenceData& sequence, float begin, float end, bool toEnd);
    /// Puts what every track says at `time` into the world. Returns how many tracks were.
    uint32_t play(World& world, const ComponentRegistry& components, Entity entity, const SequenceData& sequence,
                  float time);
    /// The id of the camera the sequence has cut to at `time`; nil for none.
    [[nodiscard]] Uuid cutAt(const World& world, Entity entity, const SequenceData& sequence, float time);
    /// Says once what is wrong with something a player plays.
    void report(const World& world, Entity entity, const std::string& what, const Error& error);

    std::unordered_map<Uuid, std::shared_ptr<const SequenceData>> m_sequences;
    /// What was said: by the player, what of it, and the message.
    std::unordered_set<std::string> m_reported;
    /// AssetDatabase::revision() when the above was loaded.
    uint64_t m_assetRevision = 0;
    std::vector<SequenceEvent> m_events;
    /// Working memory, kept so that it is not allocated every frame.
    std::vector<Entity> m_players;
};

} // namespace cordite
