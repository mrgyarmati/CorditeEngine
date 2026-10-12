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
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
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

    bool operator==(const PhysicsFrameStatistics&) const = default;
};

class PhysicsSystem {
public:
    /// Fails as PhysicsWorld::create() does, and for steps of no length or none at all.
    [[nodiscard]] static Result<std::unique_ptr<PhysicsSystem>> create(const PhysicsSystemDesc& desc = {});
    ~PhysicsSystem();
    CORDITE_NON_MOVABLE(PhysicsSystem);

    /// Brings the bodies in line with the entities of `world`, lets `deltaSeconds` pass
    /// in as many whole steps as have come together, and puts the entities of dynamic
    /// bodies where their bodies are. Less than no time, and a time that is no number,
    /// is no time. Works the world transforms out first; they are not up to date
    /// afterwards for what was moved. `assets` is where the meshes of colliders that
    /// are made of meshes come from.
    PhysicsFrameStatistics update(World& world, AssetManager& assets, float deltaSeconds);

    /// Brings the bodies in line with the entities of `world` and lets no time pass:
    /// nothing is moved by the simulation, every body is where its entity is, and the
    /// world is left as it is but for its world transforms, which are worked out.
    PhysicsFrameStatistics synchronize(World& world, AssetManager& assets);

    /// The simulation itself, for what is asked of it directly: rays, forces, gravity.
    /// Bodies are the system's to make and to remove.
    [[nodiscard]] PhysicsWorld& simulation() { return *m_physics; }

    [[nodiscard]] const PhysicsWorld& simulation() const { return *m_physics; }

    /// The body of an entity - of the entity itself, not of one above it that its
    /// collider is a part of - or the null id.
    [[nodiscard]] BodyId bodyOf(Entity entity) const;
    /// The entity a body belongs to, or the null entity.
    [[nodiscard]] Entity entityOf(BodyId body) const;

    /// The geometry that colliders of `shape` - a convex hull, or a mesh - of the mesh
    /// asset `mesh` are made of: prepared by the update that first met such a collider.
    /// The null id before that, and for a mesh of which none could be made.
    [[nodiscard]] GeometryId geometryOf(const Uuid& mesh, ShapeType shape) const;

    /// Removes every body and all geometry, and forgets time that has not become a step
    /// yet. The next update makes them again from the world it is given.
    void clear();

private:
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
        bool seen = false;
    };

    /// A body that the world asks for.
    struct Wanted {
        Entity entity;
        /// What it is to be made of: its description, without where it is.
        BodyDesc blueprint;
        /// Where it is to be: where its entity is in the world.
        Vec3 position{0.0f};
        Quat rotation = kIdentityQuat;
        /// How large its entity is in it.
        Vec3 scale{1.0f};
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
    };

    /// What is said of an entity in the log is about its collider, or about its body.
    enum class About : uint8_t {
        Collider,
        Body,
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

    /// What was made of a mesh: geometry, or the reason why there is none.
    struct MeshGeometry {
        GeometryId id;
        std::string problem;
    };

    static constexpr int32_t kNoBody = -1;

    PhysicsSystem(std::unique_ptr<PhysicsWorld> physics, const PhysicsSystemDesc& desc);

    /// Fills m_wanted with the bodies `world` asks for, parents before what is below them.
    void gather(const World& world, AssetManager& assets);
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
    /// Writes the speeds of the bodies that move to their RigidBody components, and
    /// puts the entities of dynamic bodies where the bodies are.
    void writeBack(World& world);
    /// Puts the entity of a dynamic body where the body is, unless it is there.
    void followBody(World& world, Entity entity, Record& record);
    /// Says `problem` of an entity in the log, unless it was the last thing said of it
    /// about that.
    void report(const World& world, Entity entity, About about, std::string_view problem);
    [[nodiscard]] PhysicsFrameStatistics statistics(uint32_t steps) const;

    std::unique_ptr<PhysicsWorld> m_physics;
    FixedTimestep m_timestep;
    std::unordered_map<Entity, Record> m_records;
    /// The entities that have bodies, parents before what is below them.
    std::vector<Entity> m_order;
    /// The bodies the world asked for at the last update: the first m_wantedCount. The
    /// rest are kept for what they have allocated, as is m_levels.
    std::vector<Wanted> m_wanted;
    size_t m_wantedCount = 0;
    std::vector<Level> m_levels;
    /// What has been said of entities whose colliders are no shapes, that have no body
    /// or that cannot be placed, so that each thing is said once.
    std::unordered_map<ReportKey, Report, ReportKeyHash> m_reported;
    /// The geometry made of meshes so far, and AssetDatabase::revision() when it was.
    std::unordered_map<GeometryKey, MeshGeometry, GeometryKeyHash> m_geometries;
    uint64_t m_assetRevision = 0;
};

} // namespace cordite
