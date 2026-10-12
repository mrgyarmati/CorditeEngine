#pragma once

/// @file
/// Keeps a world of entities and a simulation of bodies in step.
///
/// The entities of a World say what there is - colliders, rigid bodies
/// (PhysicsComponents.h) - and the system makes bodies of them, lets time pass in steps
/// of a fixed length, and puts the entities where their bodies have gone.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<PhysicsSystem> physics, PhysicsSystem::create());
///
/// // A game, every frame, after it has changed what it wanted to change:
/// physics->update(world, assets, frameSeconds);
///
/// // A program that shows a scene without running it: bodies are where the entities are,
/// // and no time passes.
/// physics->synchronize(world, assets);
///
/// if (const std::optional<RayHit> hit = physics->simulation().raycast(ray, 100.0f)) {
///     const Entity what = physics->entityOf(hit->body);
/// }
/// for (const BodyId body : physics->simulation().overlapSphere(blast, 5.0f)) {
///     const Entity caught = physics->entityOf(body);
/// }
///
/// // What the last update brought together, and apart.
/// for (const CollisionEvent& event : physics->events()) {
///     if (event.type == CollisionEventType::TriggerEntered && event.first == doorway) {
///         open(door);
///     }
/// }
/// @endcode
///
/// Who moves whom while time passes - update():
///
/// - A dynamic body is moved by the simulation, and its entity follows: after every
///   update the entity is where its body is. Giving the entity another transform puts the
///   body there at once; giving its RigidBody another speed gives the body that speed.
///   What the entity hangs under does not carry the body along: moved, it leaves the
///   entity where the body is, in the world.
/// - A kinematic body follows its entity: moved a little every frame, it goes through
///   the steps of the simulation at the speed that takes, and pushes what is in its way.
///   A frame without a step leaves it where it is, and the next step takes it all the way.
/// - A static body is where its entity is.
/// - A character walks as its CharacterController says, and its entity follows: after
///   every update the entity is where the character's soles are - turned as it was,
///   which is not the simulation's to say. Putting the entity somewhere else puts the
///   character there at once. What the entity hangs under does not carry the character
///   along; what the character stands on does.
///
/// While no time passes - synchronize() - the world alone has the say: every body is
/// where its entity is, however the entity came to be there, and nothing is written to
/// the world.
///
/// A body is made anew when what it is made of changes - a size, a mass, a collider
/// more or less, the scale of its entity; a moving body keeps its speed through that.
/// Entities that are switched off have no bodies. An entity whose body cannot be made -
/// a collider scaled to nothing - is named in the log once, and has none; one that is
/// nowhere - a transform that is not made of numbers - is named once too, and its body
/// stays where it was.
///
/// What is shown is the simulation after its last whole step: between two steps nothing
/// is moved, so at frame rates other than the step's a body moves in steps of its own.
///
/// A collider that is a trigger is a body of its own wherever it is in the hierarchy,
/// and never part of another: it follows its entity, stops nothing, and notices the
/// bodies that move into it and out of it. What bodies begin to touch and stop
/// touching, and what enters and leaves triggers, is in events() after every update.
///
/// A CharacterController makes its entity a character - PhysicsWorld::createCharacter() -
/// of the size its component and the scale of its entity give it. What the component
/// is told - `movement`, `jump` - is passed on with every update, and what the
/// simulation says of the character is written to the component after it. A character
/// that is to be of another size is made anew where it is, and goes on as it did.
///
/// A Joint holds the body its entity is part of to the body of the entity it names, or
/// to the world. It is made while time passes - an editor has no use for one - and
/// holds the two as they stand to each other when it first does: that is what its
/// anchor and its axis are said of, what a hinge is turned against and what limits
/// count from. The system remembers it, and makes the joint of it however often the
/// joint is made again - because a body was made anew, or the Joint given other
/// values - and however far the two have moved since. Only a Joint that names another
/// entity, or one that could not be made for a while because a body was missing,
/// begins again from how the two then stand.
///
/// A collider that is the hull around a mesh, or the triangles of one, is made of the
/// mesh asset it names. The mesh is loaded and prepared when a collider first asks for
/// it, and what was prepared serves every collider of that mesh and shape, at any
/// scale, until the assets are refreshed. A collider whose mesh cannot be had, or of
/// which no shape can be made, is left out of its body and named in the log once.
///
/// A body has no scale. Its shapes are as large as the scales above them, multiplied,
/// and an entity that is mirrored - scaled by less than nothing - stays mirrored as it
/// is while its body moves it. Shear, which turning under a scale that is not the same
/// along every axis brings about, is not followed.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/core/Time.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

class AssetManager;

struct PhysicsSystemDesc {
    PhysicsWorldDesc world;
    /// The length of a step of the simulation, in seconds. The same every time, whatever
    /// a frame takes: that is what keeps a simulation stable, and repeatable.
    double stepSeconds = 1.0 / 60.0;
    /// The most steps one update takes. A frame that would need more - a pause, a
    /// breakpoint - lets the simulation fall behind instead of taking ever longer.
    uint32_t maxStepsPerUpdate = 8;
};

/// What an update of the physics did.
struct PhysicsFrameStatistics {
    /// Steps of the simulation taken.
    uint32_t steps = 0;
    /// Bodies there are, and of them the ones that are being simulated.
    uint32_t bodies = 0;
    uint32_t activeBodies = 0;
    /// Hulls and surfaces of triangles that were made of meshes, for colliders.
    uint32_t geometries = 0;
    /// Joints that hold bodies together.
    uint32_t joints = 0;
    /// Characters. Each has a body among `bodies` that stands for it.
    uint32_t characters = 0;

    bool operator==(const PhysicsFrameStatistics&) const = default;
};

/// What can happen between the bodies of two entities.
enum class CollisionEventType : uint8_t {
    /// Two bodies have begun to touch.
    CollisionBegan,
    /// They have stopped: they have parted, or one of them is gone.
    CollisionEnded,
    /// A body has reached into a trigger.
    TriggerEntered,
    /// It has left it again, or one of the two is gone.
    TriggerLeft,
};

/// Something that happened between the bodies of two entities during an update.
struct CollisionEvent {
    CollisionEventType type = CollisionEventType::CollisionBegan;
    /// The entities that have the bodies. Of a trigger, the first is the entity whose
    /// collider the trigger is, and the second what entered or left it - a body, or a
    /// character. An entity may be gone by the time this is read: ask World::isValid().
    Entity first;
    Entity second;
    /// Of CollisionBegan and TriggerEntered: where the two touch, the direction from
    /// the first to the second there, and how fast they were closing in on each other
    /// along it, in metres per second - how hard they hit.
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float speed = 0.0f;

    bool operator==(const CollisionEvent&) const = default;
};

class PhysicsSystem {
public:
    /// Fails as PhysicsWorld::create() does, and for steps of no length or none at all.
    [[nodiscard]] static Result<std::unique_ptr<PhysicsSystem>> create(const PhysicsSystemDesc& desc = {});
    ~PhysicsSystem();
    CORDITE_NON_MOVABLE(PhysicsSystem);

    /// Brings the bodies and the joints in line with the entities of `world`, lets
    /// `deltaSeconds` pass in as many whole steps as have come together, and puts the
    /// entities of dynamic bodies where their bodies are. Less than no time, and a time
    /// that is no number, is no time. Works the world transforms out first; they are
    /// not up to date afterwards for what was moved. `assets` is where the meshes of
    /// colliders that are made of meshes come from.
    PhysicsFrameStatistics update(World& world, AssetManager& assets, float deltaSeconds);

    /// Brings the bodies in line with the entities of `world` and lets no time pass:
    /// nothing is moved by the simulation, every body is where its entity is, and the
    /// world is left as it is but for its world transforms, which are worked out.
    /// Joints are not looked at: none is made, and those there are stay as they are.
    PhysicsFrameStatistics synchronize(World& world, AssetManager& assets);

    /// The simulation itself, for what is asked of it directly: rays, forces, gravity.
    /// Bodies are the system's to make and to remove.
    [[nodiscard]] PhysicsWorld& simulation() { return *m_physics; }

    [[nodiscard]] const PhysicsWorld& simulation() const { return *m_physics; }

    /// The body of an entity that collides - of the entity itself, not of one above it
    /// that its collider is a part of - or the null id.
    [[nodiscard]] BodyId bodyOf(Entity entity) const;
    /// The body of the trigger that an entity's collider is, or the null id.
    [[nodiscard]] BodyId triggerOf(Entity entity) const;
    /// The joint that an entity's Joint has become, or the null id: before the first
    /// update that met it, and for one that could not be made.
    [[nodiscard]] JointId jointOf(Entity entity) const;
    /// The character that an entity's CharacterController has become, or the null id:
    /// before the first update or synchronize() that met it, and for one that could
    /// not be made.
    [[nodiscard]] CharacterId characterOf(Entity entity) const;
    /// The entity a body belongs to - also the body that stands for a character - or
    /// the null entity.
    [[nodiscard]] Entity entityOf(BodyId body) const;

    /// What happened during the last update(): which bodies began to touch and which
    /// stopped, what entered a trigger and what left one - each pair of entities once,
    /// step by step in the order it happened, and within a step in the order of the
    /// bodies. Two bodies that fall asleep against each other touch on. A body that
    /// was made anew because what it is made of changed does not stop and begin to
    /// touch what it still touches. None after synchronize(), in which no time passes.
    ///
    /// A character is no body, and what it walks into is not said: it enters and leaves
    /// triggers, and bodies that come to rest on it touch it. A trigger says nothing of
    /// what it hangs on - the body or the character of its own entity, or of an entity
    /// above it - which it would otherwise be entered by for as long as it is there.
    [[nodiscard]] std::span<const CollisionEvent> events() const { return m_events; }

    /// The geometry that colliders of `shape` - a convex hull, or a mesh - of the mesh
    /// asset `mesh` are made of: prepared by the update that first met such a collider.
    /// The null id before that, and for a mesh of which none could be made.
    [[nodiscard]] GeometryId geometryOf(const Uuid& mesh, ShapeType shape) const;

    /// Removes every body, every character and all geometry, and forgets time that has
    /// not become a step yet. The next update makes them again from the world it is
    /// given.
    void clear();

private:
    /// Which body of an entity: the one that collides, or the trigger its collider is.
    struct BodyKey {
        Entity entity;
        bool trigger = false;

        bool operator==(const BodyKey&) const = default;
    };

    struct BodyKeyHash {
        size_t operator()(const BodyKey& key) const noexcept;
    };

    /// Who has the say over where a dynamic body and its entity are: the simulation
    /// while time passes, the world while it does not.
    enum class Authority : uint8_t {
        Simulation,
        World
    };

    /// What the system knows of a body it has made.
    struct Record {
        BodyId body;
        /// What the body was made of: its description, without where it is.
        BodyDesc blueprint;
        /// Where the body was when it and its entity were last brought in line: where
        /// it was put or sent, or where the simulation had it.
        Vec3 position{0.0f};
        Quat rotation = kIdentityQuat;
        /// Of a dynamic body: its entity's own transform, and the matrix of what the
        /// entity hangs under, as they were then. They tell - exactly, so that rounding
        /// moves nothing - that the entity has been moved since, by itself or by
        /// something above it.
        Transform local;
        Mat4 parentToWorld{1.0f};
        /// How large the entity is in its body, which has no scale of its own.
        Vec3 scale{1.0f};
        /// The speeds last written to the entity's RigidBody, which tell the same of them.
        Vec3 linearVelocity{0.0f};
        Vec3 angularVelocity{0.0f};
        /// Where its entity came in the last walk through the hierarchy: what an entity
        /// hangs under comes before it.
        uint32_t place = 0;
        bool seen = false;
    };

    /// A body that the world asks for.
    struct Wanted {
        Entity entity;
        /// It is the trigger that the entity's collider is, not a body that collides.
        bool trigger = false;
        /// What it is to be made of: its description, without where it is.
        BodyDesc blueprint;
        /// Where it is to be: where its entity is in the world.
        Vec3 position{0.0f};
        Quat rotation = kIdentityQuat;
        /// How large its entity is in it.
        Vec3 scale{1.0f};
        /// Where its entity came in the walk through the hierarchy.
        uint32_t place = 0;
    };

    /// A character that the world asks for.
    struct WantedCharacter {
        Entity entity;
        /// What it is to be made of, without where it is.
        CharacterDesc blueprint;
        /// Where its soles are to be: where its entity is in the world.
        Vec3 position{0.0f};
        uint32_t place = 0;
    };

    /// What the system knows of a character it has made.
    struct CharacterRecord {
        CharacterId character;
        CharacterDesc blueprint;
        /// Where the character was when it and its entity were last brought in line,
        /// and - to tell, exactly, that the entity has been put somewhere else since -
        /// where the entity then was under what it hangs under, and the matrix of that.
        /// How the entity is turned and how large it is does not move a character.
        Vec3 position{0.0f};
        Vec3 localPosition{0.0f};
        Mat4 parentToWorld{1.0f};
        uint32_t place = 0;
        bool seen = false;
    };

    /// What the walk through the hierarchy knows of a depth it is in.
    struct Level {
        /// Whether what is there is switched on at all.
        bool enabled = false;
        /// Which of m_wanted the colliders found below are part of, or kNoBody.
        int32_t body = kNoBody;
        /// How large things are there: the scales of the entity and of everything above
        /// it, multiplied.
        Vec3 scale{1.0f};
        /// Takes a point from there to that body.
        Mat4 toBody{1.0f};
        /// The character that what is there hangs on: the nearest above with no rigid
        /// body between, or none.
        Entity character;
    };

    /// What is said of an entity in the log is about its collider, its body, its joint
    /// or the character it is.
    enum class About : uint8_t {
        Collider,
        Body,
        Joint,
        Character,
    };

    struct ReportKey {
        Entity entity;
        About about = About::Body;

        bool operator==(const ReportKey&) const = default;
    };

    struct ReportKeyHash {
        size_t operator()(const ReportKey& key) const noexcept;
    };

    /// Something said of an entity in the log, and whether it still holds.
    struct Report {
        std::string problem;
        bool current = false;
    };

    /// Which geometry: that of a mesh, for colliders of one shape.
    struct GeometryKey {
        Uuid mesh;
        ShapeType shape = ShapeType::ConvexHull;

        bool operator==(const GeometryKey&) const = default;
    };

    struct GeometryKeyHash {
        size_t operator()(const GeometryKey& key) const noexcept;
    };

    /// What the system knows of a joint it has made, or tried to.
    struct JointRecord {
        JointId joint;
        /// What it was made of.
        JointComponent blueprint;
        /// The bodies it holds; the second is none for the world.
        BodyId first;
        BodyId second;
        /// What took a point of the entity to the space of the entity it is held to -
        /// to the world, for none - when the two were first joined: how they stood to
        /// each other then, which is what the joint holds them to and counts from.
        /// Where the joint is on the entity itself, its component says.
        Mat4 restToConnected{1.0f};
        /// Whether that has been found. It cannot be under something of no size.
        bool hasRest = false;
        bool seen = false;
    };

    /// What was made of a mesh: geometry, or the reason why there is none.
    struct MeshGeometry {
        GeometryId id;
        std::string problem;
    };

    static constexpr int32_t kNoBody = -1;

    PhysicsSystem(std::unique_ptr<PhysicsWorld> physics, const PhysicsSystemDesc& desc);

    /// Fills m_wanted and m_wantedCharacters with the bodies and the characters that
    /// `world` asks for, parents before what is below them.
    void gather(const World& world, AssetManager& assets);
    /// The level of an entity that the walk has come to, `depth` below the top: with
    /// what it takes over from the level above it.
    [[nodiscard]] Level& enterLevel(const World& world, Entity entity, uint32_t depth);
    /// Asks for the character of an entity that has a CharacterController, and notes at
    /// its level that what is below hangs on one. `place` is where the entity came in
    /// the walk.
    void gatherCharacter(const World& world, Entity entity, Level& level, uint32_t place);
    /// The geometry for colliders of `shape` of a mesh: prepared the first time it is
    /// asked for, and remembered - also that there is none, and why.
    [[nodiscard]] const MeshGeometry& prepareGeometry(AssetManager& assets, const Uuid& mesh, ShapeType shape);
    /// Forgets all geometry when the assets have been refreshed since it was prepared:
    /// the meshes it was made of may be others now.
    void followAssets(const AssetManager& assets);
    void forgetGeometry();
    /// Makes, remakes, moves and removes bodies until they are what m_wanted says.
    /// `steps` is how many steps follow, for bodies that are moved through them.
    void reconcile(const World& world, uint32_t steps, Authority authority);
    /// Makes the body of `wanted`, in place of `previous` where there is one. Returns
    /// its record, or null when the body cannot be made.
    [[nodiscard]] Record* makeBody(const World& world, const Wanted& wanted, const Record* previous);
    /// Puts a body that is as it was wanted where its entity is, as its kind of motion
    /// and `authority` have it.
    void placeBody(const World& world, const Wanted& wanted, Record& record, uint32_t steps, Authority authority);
    /// Gives a dynamic body the speeds its RigidBody was given since they were last
    /// written.
    void applyVelocities(const World& world, Entity entity, Record& record);
    /// Makes, remakes, places and removes characters until they are what
    /// m_wantedCharacters says.
    void reconcileCharacters(const World& world, Authority authority);
    /// Makes the character of `wanted`, in place of `previous` where there is one - and
    /// going on as that one did. Returns its record, or null when it cannot be made.
    [[nodiscard]] CharacterRecord* makeCharacter(const World& world, const WantedCharacter& wanted,
                                                 const CharacterRecord* previous);
    /// Puts a character that is as it was wanted where its entity is, if the entity
    /// was put somewhere else since the two were last in line - or, while the world
    /// has the say, was moved by whatever it hangs under.
    void putCharacter(const World& world, const WantedCharacter& wanted, CharacterRecord& record, Authority authority);
    /// Passes on what the CharacterControllers were told: how to walk, and to jump -
    /// which is taken from them.
    void driveCharacters(World& world);
    /// Writes what the simulation says of a character to its CharacterController, and
    /// puts its entity where the character is, unless it is there.
    void followCharacter(World& world, Entity entity);
    /// Makes, remakes and removes joints until they are what the Joint components of
    /// the entities in m_jointEntities say, between the bodies there are.
    void reconcileJoints(const World& world);
    /// What the simulation is asked for, for the joint of an entity that is held to
    /// `connected` - or to the world, for none - between the bodies of the record.
    [[nodiscard]] JointDesc makeJointDesc(const World& world, Entity entity, Entity connected,
                                          const JointRecord& record) const;
    /// Forgets what was said about one kind of thing and was not said again since this
    /// was last called for it, so that it is said again should it hold again. Whoever
    /// reports about a kind of thing calls this for it, after looking at all of them.
    void forgetWhatNoLongerHolds(About about);
    /// Adds to m_events what the bodies said of touching in the step that was just
    /// taken - but for what a trigger would say of what it hangs on.
    void hearContacts(const World& world);
    /// Takes out of m_events what only came of a body being made anew: that it stopped
    /// touching something, and began to touch it.
    void dropEchoesOfRemaking();
    /// Writes the speeds of the bodies that move to their RigidBody components, and
    /// puts the entities of dynamic bodies and of characters where those are: each
    /// after what it hangs under.
    void writeBack(World& world);
    /// Puts the entity of a dynamic body where the body is, unless it is there.
    void followBody(World& world, Entity entity, Record& record);
    /// Says `problem` of an entity in the log, unless it was the last thing said of it
    /// about that.
    void report(const World& world, Entity entity, About about, std::string_view problem);
    [[nodiscard]] PhysicsFrameStatistics statistics(uint32_t steps) const;

    std::unique_ptr<PhysicsWorld> m_physics;
    FixedTimestep m_timestep;
    std::unordered_map<BodyKey, Record, BodyKeyHash> m_records;
    /// The bodies there are, parents before what is below them.
    std::vector<BodyKey> m_order;
    /// What happened in the last update.
    std::vector<CollisionEvent> m_events;
    /// The entities whose bodies were made anew since the last step was taken: what
    /// the next step says of their bodies stopping and beginning to touch is no news.
    std::unordered_set<Entity> m_remade;
    /// The bodies the world asked for at the last update: the first m_wantedCount. The
    /// rest are kept for what they have allocated, as is m_levels.
    std::vector<Wanted> m_wanted;
    size_t m_wantedCount = 0;
    std::vector<Level> m_levels;
    /// What has been said of entities whose colliders are no shapes, that have no body
    /// or that cannot be placed, so that each thing is said once.
    std::unordered_map<ReportKey, Report, ReportKeyHash> m_reported;
    /// The characters there are, by their entities; those the world asked for at the
    /// last update; and those that were made of them, parents before what is below.
    std::unordered_map<Entity, CharacterRecord> m_characters;
    std::vector<WantedCharacter> m_wantedCharacters;
    std::vector<Entity> m_characterOrder;
    /// The joints there are, by the entity that has the component, and the entities
    /// with a Joint that the last walk through the world met.
    std::unordered_map<Entity, JointRecord> m_joints;
    std::vector<Entity> m_jointEntities;
    /// The geometry made of meshes so far, and AssetDatabase::revision() when it was.
    std::unordered_map<GeometryKey, MeshGeometry, GeometryKeyHash> m_geometries;
    uint64_t m_assetRevision = 0;
};

} // namespace cordite
