#pragma once

/// @file
/// The simulation of rigid bodies: what falls, collides, slides and comes to rest.
///
/// A PhysicsWorld holds bodies - a shape, a mass, where it is and how it moves - and
/// moves them on by a step of time. It knows nothing of entities, components or
/// assets; the physics system (PhysicsSystem.h) is what keeps a World and a
/// PhysicsWorld in step.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::unique_ptr<PhysicsWorld> physics, PhysicsWorld::create());
/// CORDITE_TRY_ASSIGN(const BodyId floor,
///                    physics->createBody({.motion = BodyMotion::Static,
///                                         .shapes = {{.type = ShapeType::Box, .size = Vec3(20.0f, 1.0f, 20.0f)}},
///                                         .position = Vec3(0.0f, -0.5f, 0.0f)}));
/// CORDITE_TRY_ASSIGN(const BodyId ball,
///                    physics->createBody({.shapes = {{.type = ShapeType::Sphere, .radius = 0.5f}},
///                                         .position = Vec3(0.0f, 4.0f, 0.0f)}));
///
/// for (int step = 0; step < 120; ++step) {
///     physics->step(1.0f / 60.0f);
/// }
/// const Vec3 restsAt = physics->position(ball);       // on the floor: (0, 0.5, 0)
///
/// if (const std::optional<RayHit> hit = physics->raycast(Ray{Vec3(0.0f, 10.0f, 0.0f), -kWorldUp}, 100.0f)) {
///     // hit->body == ball, hit->distance == 9, hit->normal == (0, 1, 0)
/// }
/// // What a ball of 20 centimetres runs into on the same way, and what is within a metre of a point.
/// const std::optional<RayHit> touched = physics->sphereCast(Ray{Vec3(0.0f, 10.0f, 0.0f), -kWorldUp}, 0.2f, 100.0f);
/// const std::vector<BodyId> near = physics->overlapSphere(Vec3(0.0f, 0.5f, 0.0f), 1.0f);
/// @endcode
///
/// Bodies are held together by joints:
///
/// @code
/// // A door that turns about its edge, a quarter turn either way. Points and directions are in
/// // the space of each body: here the frame stands where the world begins, and is not turned.
/// CORDITE_TRY_ASSIGN(const JointId hinge,
///                    physics->createJoint({.type = JointType::Hinge,
///                                          .first = door,
///                                          .second = frame,
///                                          .firstPoint = Vec3(-0.5f, 0.0f, 0.0f),
///                                          .secondPoint = Vec3(1.0f, 1.0f, 0.0f),
///                                          .limited = true,
///                                          .minimum = -kHalfPi,
///                                          .maximum = kHalfPi}));
/// @endcode
///
/// Shapes that are not a box, a ball or a capsule are made of geometry, which is
/// prepared once and can then be the shape of any number of bodies, at any size:
///
/// @code
/// CORDITE_TRY_ASSIGN(const GeometryId rock, physics->createConvexHull(points));
/// CORDITE_TRY_ASSIGN(const GeometryId terrain, physics->createTriangleMesh(positions, indices));
/// CORDITE_TRY_ASSIGN(const BodyId boulder,
///                    physics->createBody({.shapes = {{.type = ShapeType::ConvexHull,
///                                                     .geometry = rock,
///                                                     .scale = Vec3(2.0f)}},
///                                         .position = Vec3(0.0f, 10.0f, 0.0f),
///                                         .mass = 500.0f}));
/// CORDITE_TRY_ASSIGN(const BodyId ground,
///                    physics->createBody({.motion = BodyMotion::Static,
///                                         .shapes = {{.type = ShapeType::Mesh, .geometry = terrain}}}));
/// @endcode
///
/// Units and conventions are the engine's: metres, kilograms, seconds; right-handed with
/// +Y up; rotations as unit quaternions. A simulation is repeatable: the same bodies,
/// created in the same order and stepped by the same times, end up in the same places,
/// to the bit, on every machine.
///
/// The library behind it is Jolt Physics, which no header of the engine shows.
///
/// Not thread-safe: use a PhysicsWorld from one thread at a time.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cordite {

/// Refers to a body of one PhysicsWorld. A plain value; it stops being valid when its
/// body is destroyed, and is then never mistaken for a body made later.
struct BodyId {
    /// What a default-constructed BodyId holds: no body.
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    uint32_t value = kNone;

    [[nodiscard]] constexpr bool isNull() const noexcept { return value == kNone; }

    constexpr bool operator==(const BodyId&) const noexcept = default;
};

/// Refers to geometry that one PhysicsWorld has prepared: the hull around a set of
/// points, or a surface of triangles. A plain value; it stops being valid when its
/// geometry is destroyed, and is then never mistaken for geometry prepared later.
struct GeometryId {
    /// What a default-constructed GeometryId holds: no geometry.
    static constexpr uint32_t kNone = 0u;

    uint32_t value = kNone;

    [[nodiscard]] constexpr bool isNull() const noexcept { return value == kNone; }

    constexpr bool operator==(const GeometryId&) const noexcept = default;
};

/// Refers to a joint between bodies of one PhysicsWorld. A plain value; it stops being
/// valid when the joint is destroyed - which it is with either of its bodies - and is
/// then never mistaken for a joint made later.
struct JointId {
    /// What a default-constructed JointId holds: no joint.
    static constexpr uint32_t kNone = 0u;

    uint32_t value = kNone;

    [[nodiscard]] constexpr bool isNull() const noexcept { return value == kNone; }

    constexpr bool operator==(const JointId&) const noexcept = default;
};

/// How a joint holds two bodies together.
enum class JointType : uint8_t {
    /// As they are: the two move as one.
    Fixed,
    /// At a point, about which each turns as it likes: a ball in a socket.
    Ball,
    /// At an axis, about which they turn against each other: a door on its hinges.
    Hinge,
    /// Along an axis, on which they slide against each other without turning: a drawer.
    Slider,
    /// By how far apart a point of the one and a point of the other are: a rod, a rope,
    /// a spring.
    Distance,
};

inline constexpr std::array<JointType, 5> kJointTypes = {JointType::Fixed, JointType::Ball, JointType::Hinge,
                                                         JointType::Slider, JointType::Distance};

/// The name used in files and by tools: "fixed", "ball", "hinge", "slider", "distance".
std::string_view nameOf(JointType type);

/// What a joint is made with. Points and directions are in the space of the body they
/// are on - where the body is, turned as it is - and in that of the world for the
/// second body of a joint to the world.
struct JointDesc {
    JointType type = JointType::Fixed;
    /// The body that is joined, and the one it is joined to - or none, for the world.
    /// At least one of the two is dynamic: nothing else can be held by a joint.
    BodyId first;
    BodyId second;
    /// Where the joint is on each body: the point about which a ball turns, a point on
    /// the axis of a hinge or of a slider, an end of a distance. A fixed joint holds the
    /// two points in one place.
    Vec3 firstPoint{0.0f};
    Vec3 secondPoint{0.0f};
    /// Of a hinge the axis it turns about and of a slider the axis it slides along, on
    /// each body; of any length but none. A fixed joint holds the two axes, and the two
    /// normals, pointing the same way.
    Vec3 firstAxis{0.0f, 1.0f, 0.0f};
    Vec3 secondAxis{0.0f, 1.0f, 0.0f};
    /// A direction across the axis on each body: a hinge is turned by nothing where the
    /// two point the same way. Made to stand across the axis where it does not.
    Vec3 firstNormal{1.0f, 0.0f, 0.0f};
    Vec3 secondNormal{1.0f, 0.0f, 0.0f};
    /// Whether what the joint leaves free is limited, and to what. A hinge: the angle by
    /// which the first body is turned against the second about the second one's axis,
    /// in radians, from `minimum` - between -pi and 0 - to `maximum` - between 0 and
    /// pi. A slider: how far the first has slid against the second along the second
    /// one's axis, in metres, from at most 0 to at least 0. Both are at nothing where
    /// the two points are in one place and the two normals point the same way; limited
    /// to nothing at all, they would be fixed joints, and are refused. A distance: how
    /// far apart its ends may be in metres, from 0 up; one that is not limited keeps
    /// them as far apart as they are when it is made.
    bool limited = false;
    float minimum = 0.0f;
    float maximum = 0.0f;
    /// How a distance, or the limit of a hinge or a slider, gives: the frequency in
    /// hertz at which it swings - 0 for not at all - and how fast that dies away, from
    /// 0 for never to 1 for at once.
    float frequency = 0.0f;
    float damping = 0.0f;
    /// Whether the two bodies collide with each other. Mostly they must not: what is
    /// joined is close together, or reaches into each other.
    bool collide = false;

    bool operator==(const JointDesc&) const = default;
};

/// How a body moves.
enum class BodyMotion : uint8_t {
    /// It does not: the ground, a wall. Costs next to nothing.
    Static,
    /// It goes where it is told and is not moved by anything: a platform, a door. What
    /// is in its way is pushed.
    Kinematic,
    /// It is moved by gravity, by what it collides with and by the forces on it.
    Dynamic,
};

inline constexpr std::array<BodyMotion, 3> kBodyMotions = {BodyMotion::Static, BodyMotion::Kinematic,
                                                           BodyMotion::Dynamic};

/// The name used in files and by tools: "static", "kinematic", "dynamic".
std::string_view nameOf(BodyMotion motion);

/// The form of a body, for collisions.
enum class ShapeType : uint8_t {
    /// A box of ShapeDesc::size.
    Box,
    /// A ball of ShapeDesc::radius.
    Sphere,
    /// A cylinder along Y with a half ball at either end: ShapeDesc::radius thick and
    /// ShapeDesc::height from end to end.
    Capsule,
    /// The smallest shape without dents that holds every point of ShapeDesc::geometry,
    /// which PhysicsWorld::createConvexHull() has prepared; ShapeDesc::scale times as
    /// large.
    ConvexHull,
    /// The triangles of ShapeDesc::geometry, which PhysicsWorld::createTriangleMesh()
    /// has prepared; ShapeDesc::scale times as large. A surface, with no inside: only
    /// for bodies that the simulation does not move.
    Mesh,
};

inline constexpr std::array<ShapeType, 5> kShapeTypes = {ShapeType::Box, ShapeType::Sphere, ShapeType::Capsule,
                                                         ShapeType::ConvexHull, ShapeType::Mesh};

/// The name used in files and by tools: "box", "sphere", "capsule", "convexHull",
/// "mesh".
std::string_view nameOf(ShapeType type);

/// A shape, and where it is in the body it belongs to.
struct ShapeDesc {
    ShapeType type = ShapeType::Box;
    /// Of a box: its edge lengths, in metres.
    Vec3 size{1.0f};
    /// Of a sphere and of a capsule, in metres.
    float radius = 0.5f;
    /// Of a capsule: from end to end, in metres. One that is no taller than it is wide
    /// is a sphere.
    float height = 2.0f;
    /// Of a convex hull and of a mesh: the geometry it is made of, and how many times
    /// as large as that it is along each axis. Less than nothing mirrors; nothing at
    /// all leaves no shape.
    GeometryId geometry;
    Vec3 scale{1.0f};
    /// Where the middle of the shape is, in the space of its body, and how the shape is
    /// turned there; the rotation of any length but none.
    Vec3 offset{0.0f};
    Quat rotation = kIdentityQuat;

    bool operator==(const ShapeDesc&) const = default;
};

/// The most shapes a body can be put together of.
inline constexpr uint32_t kMaxBodyShapes = 1024;

/// What a body is made with.
struct BodyDesc {
    BodyMotion motion = BodyMotion::Dynamic;
    /// What it is put together of: one shape at least, kMaxBodyShapes at most. They
    /// move as one, and may overlap.
    std::vector<ShapeDesc> shapes = {ShapeDesc()};
    /// Where the body is, in metres: the point its shapes are placed against.
    Vec3 position{0.0f};
    /// Of any length but none; brought to unit length.
    Quat rotation = kIdentityQuat;
    /// In kilograms; of a dynamic body, more than nothing. How its mass is spread is
    /// worked out from its shape.
    float mass = 1.0f;
    /// How much it resists sliding: 0 ice, 1 rubber. Of two bodies that touch, the
    /// geometric mean counts.
    float friction = 0.5f;
    /// How much of its speed it keeps when it bounces: 0 none, 1 all of it. Of two
    /// bodies that touch, the larger counts.
    float restitution = 0.0f;
    /// How fast it loses speed, and spin, by itself: the share lost in a second.
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    /// How much gravity pulls at it: 1 as at everything, 0 not at all.
    float gravityScale = 1.0f;
    /// Checked against what it might pass through between two steps, at some cost: for
    /// what is small and fast, like a bullet.
    bool continuous = false;
    /// It may fall asleep when it has come to rest, and then costs nothing until
    /// something touches it.
    bool allowSleep = true;
    /// It stops nothing and is stopped by nothing: it only notices the bodies that
    /// move - dynamic and kinematic ones - when they reach into it and when they leave,
    /// and says so (PhysicsWorld::contactEvents()). Never static: what does not move
    /// does not notice what has fallen asleep in it. No ray, ball or overlap finds it.
    bool sensor = false;
    /// Whatever the owner of the body wants to find again from a BodyId or a RayHit.
    uint64_t userData = 0;

    bool operator==(const BodyDesc&) const = default;
};

/// How large a simulation may become. The memory for all of it is taken when the world
/// is made.
struct PhysicsWorldDesc {
    /// The pull on every dynamic body, in metres per second squared.
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Bodies of all kinds together.
    uint32_t maxBodies = 16384;
    /// Pairs of bodies that are near each other in one step, and points at which bodies
    /// touch. A step that needs more leaves some contacts out, which is reported.
    uint32_t maxBodyPairs = 65536;
    uint32_t maxContactConstraints = 16384;
    /// The most points a convex hull is found around, and the most positions and
    /// triangles a surface of triangles is made of. As given, what a mesh of the engine
    /// can hold. No memory is taken for these.
    uint32_t maxGeometryPoints = 1u << 24;
    uint32_t maxGeometryTriangles = 1u << 24;
};

/// Where a ray - or a ball that is moved along one - hits a body.
struct RayHit {
    BodyId body;
    /// BodyDesc::userData of the body.
    uint64_t userData = 0;
    /// Along the ray, in metres.
    float distance = 0.0f;
    Vec3 position{0.0f};
    /// Of the surface that was hit, pointing out of the body; unit length.
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

/// Whether two bodies have begun to touch, or have stopped.
enum class ContactPhase : uint8_t {
    Began,
    Ended,
};

/// Two bodies that have begun to touch, or have stopped.
struct ContactEvent {
    ContactPhase phase = ContactPhase::Began;
    /// The two bodies. Where one of them is a sensor it is the first; otherwise the
    /// first is the one that has the earlier place among the bodies of the world. Of a
    /// contact that ended, an id may be no body's any more: what is destroyed stops
    /// touching.
    BodyId first;
    BodyId second;
    /// BodyDesc::userData of the two, which says whose they were when they are gone.
    uint64_t firstUserData = 0;
    uint64_t secondUserData = 0;
    /// One of the two is a sensor: nothing was stopped.
    bool sensor = false;
    /// Of a contact that began: where the two touch, the direction from the first to
    /// the second there - of unit length - and how fast they were closing in on each
    /// other along it, in metres per second: how hard they hit. Of one that ended,
    /// nothing.
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float speed = 0.0f;

    bool operator==(const ContactEvent&) const = default;
};

/// What a PhysicsWorld holds, and has done.
struct PhysicsStatistics {
    uint32_t bodies = 0;
    /// Bodies that are being simulated: dynamic and kinematic ones that are not asleep.
    uint32_t activeBodies = 0;
    /// Hulls and surfaces of triangles that are prepared.
    uint32_t geometries = 0;
    uint32_t joints = 0;
    /// Steps taken since the world was made.
    uint64_t steps = 0;
    /// Steps that needed more room than PhysicsWorldDesc gives, and left contacts out.
    uint64_t overflowedSteps = 0;

    bool operator==(const PhysicsStatistics&) const = default;
};

class PhysicsWorld {
public:
    /// Fails when a limit of `desc` is zero or its gravity is not made of numbers.
    [[nodiscard]] static Result<std::unique_ptr<PhysicsWorld>> create(const PhysicsWorldDesc& desc = {});
    ~PhysicsWorld();
    CORDITE_NON_MOVABLE(PhysicsWorld);

    /// Prepares the smallest shape without dents that holds all of `points`, for
    /// shapes of the type ConvexHull. Points inside it, and points that are there
    /// twice, do no harm; a hull of more corners than the simulation handles well is
    /// made of the 256 that matter most. Fails with InvalidArgument for fewer than four
    /// points or more than PhysicsWorldDesc::maxGeometryPoints, for points that are not
    /// numbers, and for points that lie in one plane, between which there is no volume.
    [[nodiscard]] Result<GeometryId> createConvexHull(std::span<const Vec3> points);

    /// Prepares a surface of triangles, for shapes of the type Mesh: three of `indices`
    /// to a triangle, each the number of one of `positions`, counter-clockwise seen
    /// from the side the surface faces. What comes at it from that side is stopped; a
    /// ray finds it from either. Positions less than a tenth of a millimetre apart are
    /// taken for one corner, so that the triangles that meet there are known to;
    /// triangles without area and triangles that are there twice are left out. Fails
    /// with InvalidArgument for
    /// indices that are not three to a triangle or name no position, for positions
    /// that are not numbers, for more positions or triangles than PhysicsWorldDesc
    /// allows, and when no triangle is left.
    [[nodiscard]] Result<GeometryId> createTriangleMesh(std::span<const Vec3> positions,
                                                        std::span<const uint32_t> indices);

    /// Forgets geometry. Bodies that were made of it keep their shapes. An id that is
    /// no geometry's (any more) is ignored.
    void destroyGeometry(GeometryId geometry);

    [[nodiscard]] bool isValid(GeometryId geometry) const;

    /// The box around geometry, in the space it was given in; the empty box for an id
    /// that is no geometry's.
    [[nodiscard]] Aabb geometryBounds(GeometryId geometry) const;

    /// The edges of geometry, for tools that show it: two points to an edge, in the
    /// space the geometry was given in. Of a hull, the edges of its faces; of a
    /// surface, the sides of its triangles, each once. At most `maxEdges` of them:
    /// where there are more, the result is empty - half a picture says less than
    /// none. Empty for an id that is no geometry's.
    [[nodiscard]] std::vector<Vec3> geometryEdges(GeometryId geometry, size_t maxEdges) const;

    /// Adds a body. Fails - and adds nothing - for a body without shapes or with too
    /// many, a shape without volume or of geometry that is not there, a dynamic body
    /// without mass or with a shape of the type Mesh, a sensor that is static, numbers
    /// that are none, and when the world is full.
    [[nodiscard]] Result<BodyId> createBody(const BodyDesc& desc);

    /// Removes a body, and with it the joints it is held by. What slept against it
    /// wakes: it may have lost what held it. An id that is not a body's (any more) is
    /// ignored.
    void destroyBody(BodyId body);

    /// Joins two bodies, or a body and the world, and wakes them. Fails with
    /// InvalidArgument for bodies that are none or one and the same, for two of which
    /// neither is dynamic, for points and directions that are not numbers, an axis of
    /// no length, and limits that are not what JointDesc says they are.
    [[nodiscard]] Result<JointId> createJoint(const JointDesc& desc);

    /// Takes a joint away, and wakes what it held. An id that is no joint's (any more)
    /// is ignored.
    void destroyJoint(JointId joint);

    [[nodiscard]] bool isValid(JointId joint) const;

    [[nodiscard]] bool isValid(BodyId body) const;

    // What follows takes the id of a body that exists. Any other id is a mistake of the
    // caller, asserted; without assertions, questions are answered with nothing - a
    // body at the origin that does not move - and requests are ignored.

    [[nodiscard]] BodyMotion motion(BodyId body) const;
    [[nodiscard]] uint64_t userData(BodyId body) const;
    /// Whether it only notices what reaches into it: BodyDesc::sensor.
    [[nodiscard]] bool isSensor(BodyId body) const;

    /// Where the body is - BodyDesc::position, moved on - and how it is turned.
    [[nodiscard]] Vec3 position(BodyId body) const;
    [[nodiscard]] Quat rotation(BodyId body) const;

    /// Puts a body somewhere else at once, whatever is there: it does not move through
    /// what lies between, and keeps its speed. Wakes it, what slept against it where
    /// it was, and what sleeps where it is now.
    void setPlacement(BodyId body, const Vec3& position, const Quat& rotation);

    /// Has a kinematic body be at a place after the next step of `seconds`, moving
    /// there at the speed that takes - so that what it meets on the way is pushed, and
    /// what stands on it is carried. Ignored for bodies of another kind.
    void moveKinematic(BodyId body, const Vec3& position, const Quat& rotation, float seconds);

    /// In metres per second, and in radians per second about each axis.
    [[nodiscard]] Vec3 linearVelocity(BodyId body) const;
    [[nodiscard]] Vec3 angularVelocity(BodyId body) const;
    /// Ignored for a static body. Wakes the body.
    void setLinearVelocity(BodyId body, const Vec3& velocity);
    void setAngularVelocity(BodyId body, const Vec3& velocity);

    /// A push at the centre of a dynamic body: a force, in newtons, acts during the
    /// next step and is then forgotten; an impulse, in newton seconds, changes its speed
    /// at once. Ignored for bodies of another kind. Wakes the body.
    void addForce(BodyId body, const Vec3& force);
    void addImpulse(BodyId body, const Vec3& impulse);
    /// A twist about its centre, in newton metres, during the next step.
    void addTorque(BodyId body, const Vec3& torque);

    /// Whether the body is being simulated: false for one that is static or asleep.
    [[nodiscard]] bool isActive(BodyId body) const;
    /// Wakes a body that is asleep.
    void activate(BodyId body);

    /// Lets `seconds` pass in one step. Steps of the same length every time - a sixtieth
    /// of a second is usual - keep a simulation stable and repeatable; no time, less
    /// than none and a time that is no number are no step.
    void step(float seconds);

    /// The bodies that began to touch during the last step(), and those that stopped:
    /// each pair once, however many of their shapes meet, in an order that is the same
    /// every time. Two bodies touch a little before they meet - when they are within a
    /// couple of centimetres and still closing in. Two that come to rest against each
    /// other and fall asleep touch on, and say nothing until one of them is moved away
    /// or destroyed. A sensor says the same of what reaches into it.
    [[nodiscard]] std::span<const ContactEvent> contactEvents() const;

    /// The first thing a ray hits within `maxDistance` metres, or nothing. A ray that
    /// starts inside a body hits it at once, at distance zero. A direction of no length
    /// hits nothing. Sensors are not hit - not by this, and not by what follows.
    [[nodiscard]] std::optional<RayHit> raycast(const Ray& ray, float maxDistance) const;

    /// The first thing a ball of `radius` touches when its middle is moved along a ray,
    /// within `maxDistance` metres, or nothing. Of what is returned, `distance` is how
    /// far the middle of the ball got, `position` the point at which ball and body
    /// touch, and `normal` the direction from the body to the ball there. A ball that
    /// starts in something touches it at once, at distance zero. A direction of no
    /// length, and a ball of no size, touch nothing; a ball of less than a millimetre
    /// across is taken for one of a millimetre.
    [[nodiscard]] std::optional<RayHit> sphereCast(const Ray& ray, float radius, float maxDistance) const;

    /// The bodies that reach into a ball, or into a box of `size` turned by `rotation`
    /// about its middle: each once, in an order that is the same every time. Of a
    /// surface of triangles, it is the triangles that count: a ball inside a closed
    /// one that touches none of them is not in it. None for a ball or a box without
    /// volume, and for numbers that are none; what is thinner than a millimetre is
    /// taken for a millimetre thick.
    [[nodiscard]] std::vector<BodyId> overlapSphere(const Vec3& center, float radius) const;
    [[nodiscard]] std::vector<BodyId> overlapBox(const Vec3& center, const Vec3& size,
                                                 const Quat& rotation = kIdentityQuat) const;

    [[nodiscard]] Vec3 gravity() const;
    /// Takes effect with the next step. A gravity that is not made of numbers is ignored.
    void setGravity(const Vec3& gravity);

    [[nodiscard]] PhysicsStatistics statistics() const;

private:
    struct Impl;

    explicit PhysicsWorld(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite

template <>
struct std::hash<cordite::BodyId> {
    size_t operator()(const cordite::BodyId& body) const noexcept { return std::hash<uint32_t>{}(body.value); }
};

template <>
struct std::hash<cordite::JointId> {
    size_t operator()(const cordite::JointId& joint) const noexcept { return std::hash<uint32_t>{}(joint.value); }
};

template <>
struct std::hash<cordite::GeometryId> {
    size_t operator()(const cordite::GeometryId& geometry) const noexcept {
        return std::hash<uint32_t>{}(geometry.value);
    }
};
