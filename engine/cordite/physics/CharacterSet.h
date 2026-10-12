#pragma once

/// @file
/// For the sources of the physics module alone: the characters of a PhysicsWorld, as
/// Jolt has them, and how they move.
///
/// @code
/// CharacterSet characters;
/// CORDITE_TRY_ASSIGN(const CharacterId walker, characters.create(desc, system, bodyLayer, bodyGroup));
/// characters.walk(walker, Vec3(2.0f, 0.0f, 0.0f));
/// // Every step, before the bodies are stepped:
/// characters.step(1.0f / 60.0f, gravity, solidBroadPhase, solidObjects, memory);
/// const CharacterState state = characters.state(walker);
/// @endcode
///
/// A character is Jolt's CharacterVirtual: a shape that is moved through the world by
/// asking what is in its way, not a body of the simulation. So that the simulation
/// knows of it all the same - rays hit it, sensors notice it, bodies rest on it - each
/// has a kinematic body inside it, a little smaller than itself, which Jolt keeps
/// where the character is.
///
/// What Jolt leaves to its user is how a character's speed comes about; step() is
/// where that is decided: a character that stands takes the speed of what it stands on
/// and adds what it was told to walk with, one in the air keeps falling.
///
/// The set must be emptied, or destroyed, before the JPH::PhysicsSystem its characters
/// were made in.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/physics/JoltSupport.h"
#include "cordite/physics/PhysicsWorld.h"

#include <Jolt/Core/Reference.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

namespace cordite {

/// What a character meets and takes no notice of: a body that the simulation moves
/// and that lies on top of it. Such a body is carried by the body that stands for the
/// character. Held off by the character itself it would press on it from above while
/// the ground presses from below, and between the two Jolt lets a character go
/// nowhere: a crate on its head would pin it to the spot.
class CarriedBodyFilter final : public JPH::CharacterContactListener {
public:
    bool OnContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override;
};

class CharacterSet {
public:
    CharacterSet() = default;
    ~CharacterSet() = default;
    CORDITE_NON_MOVABLE(CharacterSet);

    /// Adds a character to `system`, and the body that stands for it - in `bodyLayer`,
    /// with `bodyGroup` to tell it from other bodies where joints keep pairs from
    /// colliding. Fails, and adds nothing, for what PhysicsWorld::createCharacter()
    /// refuses and when the system has no room for the body.
    [[nodiscard]] Result<CharacterId> create(const CharacterDesc& desc, JPH::PhysicsSystem& system,
                                             JPH::ObjectLayer bodyLayer, const JPH::CollisionGroup& bodyGroup);

    /// Removes a character and its body. An id that is no character's is ignored.
    void destroy(CharacterId character);

    [[nodiscard]] bool contains(CharacterId character) const { return m_characters.contains(character.value); }

    [[nodiscard]] size_t size() const { return m_characters.size(); }

    /// The body that stands for a character, or the null id for an id that is none's.
    [[nodiscard]] BodyId bodyOf(CharacterId character) const;

    /// Calls `visit(BodyId)` with the body of every character, in the order the
    /// characters were made.
    template <typename Visit>
    void forEachBody(Visit&& visit) const {
        for (const auto& [id, character] : m_characters) {
            visit(BodyId{character.jolt->GetInnerBodyID().GetIndexAndSequenceNumber()});
        }
    }

    /// The character a body stands for, or the null id.
    [[nodiscard]] CharacterId ownerOf(BodyId body) const;

    // What follows does nothing, and answers with a character that stands nowhere, for an
    // id that is no character's.

    [[nodiscard]] CharacterState state(CharacterId character) const;

    /// See the functions of PhysicsWorld that are named alike.
    void walk(CharacterId character, const Vec3& velocity);
    void jump(CharacterId character, float speed);
    void place(CharacterId character, const Vec3& position);
    void setVelocity(CharacterId character, const Vec3& velocity);

    /// Moves every character on by `seconds`, in the order they were made: works out
    /// the speed of each from where it stands and what it was told, and has Jolt move
    /// it through whatever the two filters let it run into. `gravity` is the world's;
    /// `memory` is what the queries work in.
    void step(float seconds, const Vec3& gravity, const JPH::BroadPhaseLayerFilter& broadPhaseFilter,
              const JPH::ObjectLayerFilter& objectFilter, JPH::TempAllocator& memory);

private:
    struct Character {
        JPH::Ref<JPH::CharacterVirtual> jolt;
        CharacterDesc desc;
        /// What it was told to walk with: along the ground, nothing of it up or down.
        Vec3 walk{0.0f};
        /// How fast it went sideways when it last stood - or was last given a speed:
        /// what it goes on with in the air, as far as it has no say there.
        Vec3 carried{0.0f};
        /// How fast it moved in the last step: what CharacterState::velocity says. The
        /// speed that Jolt holds for it is what the next step starts from, which is
        /// not the same where something is in its way.
        Vec3 velocity{0.0f};
        /// The speed it is to leave the ground with in the next step; 0 for no jump.
        float jump = 0.0f;
    };

    [[nodiscard]] const Character* find(CharacterId character) const;
    [[nodiscard]] Character* find(CharacterId character);

    /// Listens for all characters, and so declared before them: it is gone after them.
    CarriedBodyFilter m_carriedBodies;
    /// By the value of their CharacterId, and so in the order they were made: the order
    /// they are moved in, which has to be the same every time.
    std::map<uint32_t, Character> m_characters;
    /// Whose body a body is, by the values of the two ids.
    std::unordered_map<uint32_t, uint32_t> m_owners;
    uint32_t m_last = CharacterId::kNone;
};

} // namespace cordite
