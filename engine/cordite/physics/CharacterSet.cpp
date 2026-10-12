#include "cordite/physics/CharacterSet.h"

#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterBase.h>
#include <Jolt/Physics/Character/CharacterID.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// How far Jolt keeps the shape of a character from what it touches, in metres: a
/// shape that is a little away from everything is moved more cheaply, and gets stuck
/// less. Jolt also puts the shape this far above where the character is, so that the
/// character is on the ground while its shape is not. The shape is made smaller by
/// this much all round: what is kept out of the world is then as large as was asked for.
constexpr float kCharacterPadding = 0.02f;
/// How much smaller than its character the body is that stands for it. What a
/// character runs into, it has to meet itself: its body would push with all the force
/// there is, and is stopped by nothing.
constexpr float kBodyFraction = 0.9f;
/// The cylinder of a capsule that is shorter than this is none: the capsule is a ball.
constexpr float kMinCylinder = 0.001f;
/// How fast a character has to rise from the ground under it to be on its way up
/// rather than standing, in metres per second.
constexpr float kLeavingGroundSpeed = 0.1f;
/// How far ground may fall away under a character within one step before the
/// character has left it, in metres - whatever its step height: this is what keeps it
/// on a slope that it walks down, at any speed a character walks or runs with.
constexpr float kFollowGroundDistance = 0.25f;
/// A character that has risen or fallen by less than this share of what its speed
/// asked for was stopped: by a ceiling, or by ground that it is wedged in.
constexpr float kBlockedShare = 0.01f;
/// A body lies on top of a character where it presses on it more downwards than
/// sideways: where the direction from the body to the character points down by more
/// than this, which is the sine of 45 degrees.
constexpr float kOnTopDownwards = 0.70710678f;

/// An upright capsule of `radius` whose middle is `middleHeight` above the point it is
/// placed at; a ball where it is no taller than it is wide.
Result<JPH::ShapeRefC> makeUprightCapsule(float radius, float height, float middleHeight) {
    const float halfCylinder = 0.5f * height - radius;
    JPH::ShapeRefC rounded;
    if (halfCylinder < kMinCylinder) {
        rounded = new JPH::SphereShape(radius);
    } else {
        rounded = new JPH::CapsuleShape(halfCylinder, radius);
    }
    const JPH::RotatedTranslatedShapeSettings settings(JPH::Vec3(0.0f, middleHeight, 0.0f), JPH::Quat::sIdentity(),
                                                       rounded);
    const JPH::ShapeSettings::ShapeResult made = settings.Create();
    if (made.HasError()) {
        return makeError(ErrorCode::InvalidArgument, "no shape could be made for a character: {}",
                         made.GetError().c_str());
    }
    return made.Get();
}

Status validateCharacter(const CharacterDesc& desc) {
    if (!isFinite(desc.radius) || !isFinite(desc.height) || !isFinite(desc.position) || !isFinite(desc.mass) ||
        !isFinite(desc.maxSlope) || !isFinite(desc.stepHeight) || !isFinite(desc.pushForce) ||
        !isFinite(desc.gravityScale) || !isFinite(desc.airControl)) {
        return makeError(ErrorCode::InvalidArgument, "what a character is made with is not all numbers");
    }
    if (desc.radius < kMinCharacterRadius || desc.height <= 0.0f) {
        return makeError(ErrorCode::InvalidArgument,
                         "a character is at least {} m around and has a height, not {} m around and {} m tall",
                         kMinCharacterRadius, desc.radius, desc.height);
    }
    if (desc.mass <= 0.0f) {
        return makeError(ErrorCode::InvalidArgument, "a character weighs more than nothing, not {} kg", desc.mass);
    }
    if (desc.maxSlope < 0.0f || desc.maxSlope >= kHalfPi) {
        return makeError(ErrorCode::InvalidArgument,
                         "the steepest ground a character walks on is from level to less than upright - from 0 "
                         "to less than pi / 2 - not {}",
                         desc.maxSlope);
    }
    if (desc.stepHeight < 0.0f || desc.pushForce < 0.0f) {
        return makeError(ErrorCode::InvalidArgument,
                         "the step a character climbs and the force it pushes with are not less than nothing: {} "
                         "m and {} N",
                         desc.stepHeight, desc.pushForce);
    }
    if (desc.airControl < 0.0f || desc.airControl > 1.0f) {
        return makeError(ErrorCode::InvalidArgument,
                         "how much say a character has in the air is from 0 for none to 1 for all, not {}",
                         desc.airControl);
    }
    return {};
}

GroundState groundStateOf(JPH::CharacterBase::EGroundState state) {
    switch (state) {
        case JPH::CharacterBase::EGroundState::OnGround:
            return GroundState::OnGround;
        case JPH::CharacterBase::EGroundState::OnSteepGround:
            return GroundState::OnSteepGround;
        case JPH::CharacterBase::EGroundState::NotSupported:
        case JPH::CharacterBase::EGroundState::InAir:
            break;
    }
    return GroundState::InAir;
}

} // namespace

bool CarriedBodyFilter::OnContactValidate(const JPH::CharacterVirtual* /*character*/,
                                          const JPH::CharacterContact& contact) {
    // The normal of a contact points at the character.
    const bool liesOnTop = contact.mContactNormal.GetY() < -kOnTopDownwards;
    return !liesOnTop || contact.mMotionTypeB != JPH::EMotionType::Dynamic;
}

Result<CharacterId> CharacterSet::create(const CharacterDesc& desc, JPH::PhysicsSystem& system,
                                         JPH::ObjectLayer bodyLayer, const JPH::CollisionGroup& bodyGroup) {
    CORDITE_TRY(validateCharacter(desc));
    // One that is no taller than it is wide is a ball.
    const float height = std::max(desc.height, 2.0f * desc.radius);
    // Both shapes end up kCharacterPadding higher than they are made here: the
    // character's with its lowest point that far above the soles, the body's around
    // the middle of the character.
    const float middle = 0.5f * height - kCharacterPadding;
    CORDITE_TRY_ASSIGN(const JPH::ShapeRefC shape,
                       makeUprightCapsule(desc.radius - kCharacterPadding, height - 2.0f * kCharacterPadding, middle));
    CORDITE_TRY_ASSIGN(const JPH::ShapeRefC bodyShape,
                       makeUprightCapsule(kBodyFraction * desc.radius, kBodyFraction * height, middle));

    // Counted up, and so used once each - until the count comes round, when those
    // still in use are passed over.
    do {
        ++m_last;
    } while (m_last == CharacterId::kNone || m_last == JPH::CharacterID::cInvalidCharacterID ||
             m_characters.contains(m_last));

    JPH::CharacterVirtualSettings settings;
    // Jolt sorts by this where the order of characters matters, and hands ids out of one
    // count for the whole program by default: two worlds would not get the same.
    settings.mID = JPH::CharacterID(m_last);
    settings.mShape = shape;
    settings.mMass = desc.mass;
    settings.mMaxStrength = desc.pushForce;
    settings.mMaxSlopeAngle = desc.maxSlope;
    settings.mCharacterPadding = kCharacterPadding;
    // What touches the lower half ball of the capsule carries the character; what
    // touches it higher up only stands in its way.
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc.radius);
    // Ground is made of triangles, and the edge at which two of them meet in a furrow
    // is nothing to run into: without this, a character is held up there for a step.
    settings.mEnhancedInternalEdgeRemoval = true;
    settings.mInnerBodyShape = bodyShape;
    settings.mInnerBodyLayer = bodyLayer;

    Character character;
    character.desc = desc;
    character.jolt = new JPH::CharacterVirtual(&settings, JPH::RVec3(toJolt(desc.position)), JPH::Quat::sIdentity(),
                                               desc.userData, &system);
    const JPH::BodyID body = character.jolt->GetInnerBodyID();
    if (body.IsInvalid()) {
        return makeError(ErrorCode::OutOfRange,
                         "the physics world is full: it holds {} bodies, and a character needs one",
                         system.GetMaxBodies());
    }
    system.GetBodyInterface().SetCollisionGroup(body, bodyGroup);
    character.jolt->SetListener(&m_carriedBodies);
    m_owners.emplace(body.GetIndexAndSequenceNumber(), m_last);
    m_characters.emplace(m_last, std::move(character));
    return CharacterId{m_last};
}

void CharacterSet::destroy(CharacterId character) {
    const auto found = m_characters.find(character.value);
    if (found == m_characters.end()) {
        return;
    }
    m_owners.erase(found->second.jolt->GetInnerBodyID().GetIndexAndSequenceNumber());
    // The character takes its body out of the simulation as it goes.
    m_characters.erase(found);
}

const CharacterSet::Character* CharacterSet::find(CharacterId character) const {
    const auto found = m_characters.find(character.value);
    return found != m_characters.end() ? &found->second : nullptr;
}

CharacterSet::Character* CharacterSet::find(CharacterId character) {
    const auto found = m_characters.find(character.value);
    return found != m_characters.end() ? &found->second : nullptr;
}

BodyId CharacterSet::bodyOf(CharacterId character) const {
    const Character* found = find(character);
    return found != nullptr ? BodyId{found->jolt->GetInnerBodyID().GetIndexAndSequenceNumber()} : BodyId();
}

CharacterId CharacterSet::ownerOf(BodyId body) const {
    const auto found = m_owners.find(body.value);
    return found != m_owners.end() ? CharacterId{found->second} : CharacterId();
}

CharacterState CharacterSet::state(CharacterId character) const {
    const Character* found = find(character);
    if (found == nullptr) {
        return {};
    }
    const JPH::CharacterVirtual& jolt = *found->jolt;
    CharacterState state;
    state.position = fromJolt(JPH::Vec3(jolt.GetPosition()));
    state.velocity = found->velocity;
    state.ground = groundStateOf(jolt.GetGroundState());
    if (state.ground != GroundState::InAir) {
        state.groundNormal = normalizeOr(fromJolt(jolt.GetGroundNormal()), kWorldUp);
        const JPH::BodyID ground = jolt.GetGroundBodyID();
        state.groundBody = ground.IsInvalid() ? BodyId() : BodyId{ground.GetIndexAndSequenceNumber()};
    }
    return state;
}

void CharacterSet::walk(CharacterId character, const Vec3& velocity) {
    Character* found = find(character);
    if (found == nullptr || !isFinite(velocity)) {
        return;
    }
    found->walk = Vec3(velocity.x, 0.0f, velocity.z);
}

void CharacterSet::jump(CharacterId character, float speed) {
    Character* found = find(character);
    if (found == nullptr || !isFinite(speed)) {
        return;
    }
    found->jump = std::max(speed, 0.0f);
}

void CharacterSet::place(CharacterId character, const Vec3& position) {
    Character* found = find(character);
    if (found == nullptr || !isFinite(position)) {
        return;
    }
    found->jolt->SetPosition(JPH::RVec3(toJolt(position)));
}

void CharacterSet::look(CharacterId character, const JPH::BroadPhaseLayerFilter& broadPhaseFilter,
                        const JPH::ObjectLayerFilter& objectFilter, JPH::TempAllocator& memory) {
    if (Character* found = find(character)) {
        found->jolt->RefreshContacts(broadPhaseFilter, objectFilter, {}, {}, memory);
    }
}

void CharacterSet::setVelocity(CharacterId character, const Vec3& velocity) {
    Character* found = find(character);
    if (found == nullptr || !isFinite(velocity)) {
        return;
    }
    found->jolt->SetLinearVelocity(toJolt(velocity));
    found->velocity = velocity;
    found->carried = Vec3(velocity.x, 0.0f, velocity.z);
}

void CharacterSet::step(float seconds, const Vec3& gravity, const JPH::BroadPhaseLayerFilter& broadPhaseFilter,
                        const JPH::ObjectLayerFilter& objectFilter, JPH::TempAllocator& memory) {
    for (auto& [id, character] : m_characters) {
        JPH::CharacterVirtual& jolt = *character.jolt;
        // What it stands on may have changed its speed since the last step.
        jolt.UpdateGroundVelocity();
        const Vec3 velocity = fromJolt(jolt.GetLinearVelocity());
        const Vec3 groundVelocity = fromJolt(jolt.GetGroundVelocity());
        const Vec3 rising(0.0f, velocity.y, 0.0f);
        // It stands when there is ground under it that it can walk on, and it is not
        // on its way up from it.
        const bool stands = jolt.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                            velocity.y - groundVelocity.y < kLeavingGroundSpeed;

        // Standing, it has the speed of what it stands on - a platform carries it - and
        // goes along the ground as it was told. In the air it keeps rising or falling,
        // and goes sideways as it did when it last stood, as far as it has no say there.
        Vec3 next = rising;
        // Gravity pulls at what is in the air, and at what is leaving the ground. What
        // stands is held by the ground: pulled at, it would creep down every slope it
        // stands on. Where the ground falls away under its feet, Jolt takes it down.
        bool isPulled = true;
        if (stands) {
            next = groundVelocity + character.walk;
            character.carried = Vec3(next.x, 0.0f, next.z);
            next.y += character.jump;
            isPulled = character.jump > 0.0f;
        } else {
            next += glm::mix(character.carried, character.walk, character.desc.airControl);
        }
        // A jump that finds no ground to leave is none.
        character.jump = 0.0f;
        const Vec3 pull = gravity * character.desc.gravityScale;
        if (isPulled) {
            next += pull * seconds;
        }
        jolt.SetLinearVelocity(toJolt(next));

        JPH::CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = JPH::Vec3(0.0f, character.desc.stepHeight, 0.0f);
        // Up a step, Jolt looks for ground to stand on this far ahead. With less than its
        // radius ahead, a character comes down on the edge of the step with the round of
        // its foot - which, for a large character on a high step, is ground too steep to
        // stand on, and it does not get up.
        settings.mWalkStairsStepForwardTest = character.desc.radius;
        settings.mStickToFloorStepDown =
            JPH::Vec3(0.0f, -std::max(character.desc.stepHeight, kFollowGroundDistance), 0.0f);
        const Vec3 from = fromJolt(JPH::Vec3(jolt.GetPosition()));
        // The pull is what a character presses on what it stands on with.
        jolt.ExtendedUpdate(seconds, toJolt(pull), settings, broadPhaseFilter, objectFilter, {}, {}, memory);

        // Jolt moves a character as far as it can at the speed it was given, and leaves
        // that speed as it was. How fast the character went is where it got to.
        const Vec3 moved = (fromJolt(JPH::Vec3(jolt.GetPosition())) - from) / seconds;
        Vec3 kept = fromJolt(jolt.GetLinearVelocity());
        if (jolt.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround) {
            // On the ground it rises and falls as the ground does - not by the pull that
            // presses it down, and not by the jolt that takes it up a step.
            character.velocity = Vec3(moved.x, jolt.GetGroundVelocity().GetY(), moved.z);
            kept = character.velocity;
        } else {
            character.velocity = moved;
            // What it falls by adds up from step to step, also where something lets it
            // fall more slowly: that is how it slides down ground too steep for it. What
            // has stopped it altogether - a ceiling on its way up, a cleft it is wedged
            // in - has taken that speed from it.
            if (std::abs(moved.y) < kBlockedShare * std::abs(kept.y)) {
                kept.y = 0.0f;
            }
        }
        jolt.SetLinearVelocity(toJolt(kept));
    }
}

} // namespace cordite
