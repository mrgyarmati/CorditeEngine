#include "cordite/physics/PhysicsWorld.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/physics/JoltSupport.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cordite {
namespace {

// Bodies are sorted into layers that decide what is checked against what: what never
// moves is not checked against itself, and what only notices is checked against what
// moves and nothing else - a sensor that reaches into the ground would otherwise be
// told so in every step, at the cost of every triangle it reaches.
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;
constexpr JPH::ObjectLayer kSensorLayer = 2;
constexpr JPH::uint kObjectLayerCount = 3;
constexpr JPH::BroadPhaseLayer kStaticBroadPhase(0);
constexpr JPH::BroadPhaseLayer kMovingBroadPhase(1);
constexpr JPH::BroadPhaseLayer kSensorBroadPhase(2);
constexpr JPH::uint kBroadPhaseLayerCount = 3;

/// What a ray, a ball or an overlap looks at: everything that can be run into, which
/// a sensor cannot.
class SolidBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::BroadPhaseLayer layer) const override { return layer != kSensorBroadPhase; }
};

class SolidObjectFilter final : public JPH::ObjectLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer != kSensorLayer; }
};

/// Memory a step works in and gives back at its end. A step that needs more takes it
/// from the heap, which is slower but not an error.
constexpr JPH::uint kStepMemoryBytes = 8u * 1024u * 1024u;

/// The smallest a shape may be along any of its axes, in metres. Below this the
/// arithmetic of collisions has no digits left to work with.
constexpr float kMinShapeExtent = 0.001f;
/// How round the edges of a box are, in metres, where the box is large enough for it:
/// boxes with slightly round edges collide faster and come to rest more steadily.
constexpr float kBoxEdgeRadius = 0.05f;
/// How many mutexes guard the bodies. Zero lets the library choose.
constexpr JPH::uint kBodyMutexCount = 0;

JPH::BodyID joltIdOf(BodyId body) {
    return JPH::BodyID(body.value);
}

JPH::EMotionType joltMotionOf(BodyMotion motion) {
    switch (motion) {
        case BodyMotion::Static:
            return JPH::EMotionType::Static;
        case BodyMotion::Kinematic:
            return JPH::EMotionType::Kinematic;
        case BodyMotion::Dynamic:
            return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

BodyMotion motionOf(JPH::EMotionType motion) {
    switch (motion) {
        case JPH::EMotionType::Static:
            return BodyMotion::Static;
        case JPH::EMotionType::Kinematic:
            return BodyMotion::Kinematic;
        case JPH::EMotionType::Dynamic:
            return BodyMotion::Dynamic;
    }
    return BodyMotion::Static;
}

// --- The library as a whole ---------------------------------------------------------------

/// What Jolt writes about itself goes to the engine's log. The signature - printf's - is
/// Jolt's.
void traceToLog(const char* format, ...) {
    std::array<char, 1024> text{};
    va_list arguments;
    va_start(arguments, format);
    // What does not fit is cut off, which is all there is to do about it here.
    (void)std::vsnprintf(text.data(), text.size(), format, arguments);
    va_end(arguments);
    CORDITE_LOG_INFO("Physics", "{}", text.data());
}

#if defined(JPH_ENABLE_ASSERTS)
/// Jolt asserts that a step had room for everything, on its way to returning that it
/// had not. That is no mistake in how Jolt is used but something a full scene does,
/// and PhysicsWorld::step() deals with what is returned - so this one assertion is let
/// pass. A test pins the text: should Jolt word it otherwise, the test says so.
constexpr std::string_view kStepHadRoomAssertion = "errors == EPhysicsUpdateError::None";

/// What Jolt finds wrong with how it is used is reported like a failed assertion of the
/// engine. Returning false tells Jolt not to stop in a debugger of its own.
bool reportAssertion(const char* expression, const char* message, const char* file, JPH::uint line) {
    if (expression != nullptr && kStepHadRoomAssertion == expression) {
        return false;
    }
    detail::reportAssertionFailure(expression != nullptr ? expression : "", file != nullptr ? file : "",
                                   static_cast<int>(line), message != nullptr ? message : "");
    return false;
}
#endif

/// Jolt keeps a registry of its types for the process. It is set up when the first
/// world is made and taken down with the last, so that a program that has stopped
/// simulating holds none of it.
class JoltRuntime {
public:
    static void acquire() {
        const std::scoped_lock lock(mutex());
        if (users()++ > 0) {
            return;
        }
        JPH::RegisterDefaultAllocator();
        JPH::Trace = traceToLog;
#if defined(JPH_ENABLE_ASSERTS)
        JPH::AssertFailed = reportAssertion;
#endif
        // Jolt's to hold, and ours to take away again in release().
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    static void release() {
        const std::scoped_lock lock(mutex());
        if (--users() > 0) {
            return;
        }
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

private:
    static std::mutex& mutex() {
        static std::mutex instance;
        return instance;
    }

    static uint32_t& users() {
        static uint32_t count = 0;
        return count;
    }
};

// --- Shapes ---------------------------------------------------------------------------------

/// Geometry that was prepared: the shape Jolt has made of it, at the size it was given
/// in, and what kind of shape that is.
struct Geometry {
    JPH::ShapeRefC shape;
    ShapeType type = ShapeType::ConvexHull;
};

/// By the value of its GeometryId.
using GeometryTable = std::unordered_map<uint32_t, Geometry>;

/// A shape of geometry: the geometry itself, or as many times as large as its scale says.
Result<JPH::ShapeRefC> makeGeometryShape(const ShapeDesc& desc, const GeometryTable& geometries) {
    const auto found = geometries.find(desc.geometry.value);
    if (found == geometries.end() || found->second.type != desc.type) {
        return makeError(ErrorCode::InvalidArgument,
                         "a shape of the type {} needs geometry that was prepared for one, and has none",
                         nameOf(desc.type));
    }
    const JPH::ShapeRefC& shape = found->second.shape;
    const Vec3 scale = glm::abs(desc.scale);
    const Vec3 size = fromJolt(shape->GetLocalBounds().GetSize()) * scale;
    // A hull needs room along every axis; a surface may be flat, but not a point - and
    // a scale of nothing is none that Jolt takes.
    const float smallest = std::min({size.x, size.y, size.z});
    const float largest = std::max({size.x, size.y, size.z});
    if (!isFinite(desc.scale) || std::min({scale.x, scale.y, scale.z}) <= kEpsilon ||
        (desc.type == ShapeType::ConvexHull ? smallest : largest) < kMinShapeExtent) {
        return makeError(ErrorCode::InvalidArgument,
                         "a {} scaled by {} by {} by {} is {} by {} by {} metres, and has no {} to collide with",
                         nameOf(desc.type), desc.scale.x, desc.scale.y, desc.scale.z, size.x, size.y, size.z,
                         desc.type == ShapeType::ConvexHull ? "volume" : "size");
    }
    if (desc.scale == Vec3(1.0f)) {
        return shape;
    }
    const JPH::ShapeSettings::ShapeResult result = JPH::ScaledShapeSettings(shape, toJolt(desc.scale)).Create();
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "cannot scale a {} to collide with: {}", nameOf(desc.type),
                         reason);
    }
    return result.Get();
}

Result<JPH::ShapeRefC> makeShape(const ShapeDesc& desc, const GeometryTable& geometries) {
    JPH::ShapeSettings::ShapeResult result;
    switch (desc.type) {
        case ShapeType::Box: {
            if (!isFinite(desc.size) || std::min({desc.size.x, desc.size.y, desc.size.z}) < kMinShapeExtent) {
                return makeError(ErrorCode::InvalidArgument,
                                 "a box of {} by {} by {} metres has no volume to collide with", desc.size.x,
                                 desc.size.y, desc.size.z);
            }
            const Vec3 halfExtents = 0.5f * desc.size;
            const float edgeRadius = std::min({kBoxEdgeRadius, halfExtents.x, halfExtents.y, halfExtents.z});
            result = JPH::BoxShapeSettings(toJolt(halfExtents), edgeRadius).Create();
            break;
        }
        case ShapeType::Sphere:
            if (!isFinite(desc.radius) || desc.radius < 0.5f * kMinShapeExtent) {
                return makeError(ErrorCode::InvalidArgument, "a sphere of radius {} has no volume to collide with",
                                 desc.radius);
            }
            result = JPH::SphereShapeSettings(desc.radius).Create();
            break;
        case ShapeType::Capsule: {
            if (!isFinite(desc.radius) || !isFinite(desc.height) || desc.radius < 0.5f * kMinShapeExtent ||
                desc.height < kMinShapeExtent) {
                return makeError(ErrorCode::InvalidArgument,
                                 "a capsule of radius {} and height {} has no volume to collide with", desc.radius,
                                 desc.height);
            }
            // Between the two half balls is a cylinder; where there is none left, the
            // capsule is a ball.
            const float halfCylinder = 0.5f * desc.height - desc.radius;
            if (halfCylinder < 0.5f * kMinShapeExtent) {
                result = JPH::SphereShapeSettings(desc.radius).Create();
            } else {
                result = JPH::CapsuleShapeSettings(halfCylinder, desc.radius).Create();
            }
            break;
        }
        case ShapeType::ConvexHull:
        case ShapeType::Mesh:
            return makeGeometryShape(desc, geometries);
    }
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "cannot make a {} to collide with: {}", nameOf(desc.type), reason);
    }
    return result.Get();
}

/// The shape of a whole body: its one shape, or its shapes put together.
Result<JPH::ShapeRefC> makeBodyShape(std::span<const ShapeDesc> shapes, const GeometryTable& geometries) {
    if (shapes.empty() || shapes.size() > kMaxBodyShapes) {
        return makeError(ErrorCode::InvalidArgument, "a body is put together of 1 to {} shapes, not {}", kMaxBodyShapes,
                         shapes.size());
    }
    for (const ShapeDesc& shape : shapes) {
        if (!isFinite(shape.offset) || !isRotation(shape.rotation)) {
            return makeError(ErrorCode::InvalidArgument,
                             "a {} cannot be placed in its body: its offset or rotation is not made of numbers",
                             nameOf(shape.type));
        }
    }
    const ShapeDesc& first = shapes.front();
    if (shapes.size() == 1 && first.offset == Vec3(0.0f) && glm::normalize(first.rotation) == kIdentityQuat) {
        return makeShape(first, geometries);
    }
    JPH::StaticCompoundShapeSettings compound;
    for (const ShapeDesc& shape : shapes) {
        CORDITE_TRY_ASSIGN(const JPH::ShapeRefC part, makeShape(shape, geometries));
        compound.AddShape(toJolt(shape.offset), toJolt(glm::normalize(shape.rotation)), part);
    }
    const JPH::ShapeSettings::ShapeResult result = compound.Create();
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "cannot put {} shapes together to collide with: {}", shapes.size(),
                         reason);
    }
    return result.Get();
}

Status validateBody(const BodyDesc& desc) {
    if (!isFinite(desc.position) || !isRotation(desc.rotation)) {
        return makeError(ErrorCode::InvalidArgument,
                         "a body cannot be placed: its position or rotation is not made of numbers");
    }
    if (desc.motion == BodyMotion::Dynamic && (!isFinite(desc.mass) || desc.mass <= 0.0f)) {
        return makeError(ErrorCode::InvalidArgument, "a dynamic body needs a mass of more than nothing, not {}",
                         desc.mass);
    }
    if (desc.sensor && desc.motion == BodyMotion::Static) {
        return makeError(ErrorCode::InvalidArgument,
                         "a sensor has to notice what has fallen asleep in it, and a static body does not: make "
                         "it kinematic");
    }
    const auto isShare = [](float value) { return isFinite(value) && value >= 0.0f; };
    if (!isShare(desc.friction) || !isShare(desc.restitution) || !isShare(desc.linearDamping) ||
        !isShare(desc.angularDamping) || !isFinite(desc.gravityScale)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the friction, restitution and damping of a body are numbers that are not negative, and "
                         "its gravity scale is a number");
    }
    return {};
}

// --- Contacts ---------------------------------------------------------------------------------

/// Two bodies that touch, as the world remembers them from step to step.
struct Touch {
    /// What was said when it began. The same, with the other phase, is said when it ends.
    ContactEvent began;
    /// The step in which Jolt last spoke of the two.
    uint64_t heardAtStep = 0;
    /// Both have stopped moving, and so Jolt says nothing of them any more.
    bool dormant = false;
};

/// Where a body is among the bodies of the world: what events are sorted by.
uint32_t placeOf(BodyId body) {
    return JPH::BodyID(body.value).GetIndex();
}

/// What is said of two bodies that have begun to touch.
ContactEvent describeContact(const JPH::Body& one, const JPH::Body& other, const JPH::ContactManifold& manifold) {
    // The middle of the points at which they touch.
    JPH::RVec3 middle = manifold.mBaseOffset;
    const JPH::uint points = manifold.mRelativeContactPointsOn1.size();
    for (JPH::uint point = 0; point < points; ++point) {
        middle += manifold.mRelativeContactPointsOn1[point] / static_cast<float>(points);
    }
    // The normal points from the one to the other. Their speeds are those they came
    // with: the step has not pushed them apart yet.
    const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
    const float closing = (one.GetPointVelocity(middle) - other.GetPointVelocity(middle)).Dot(normal);

    // Jolt names the body with the smaller id first. A sensor comes first here.
    const bool swap = other.IsSensor() && !one.IsSensor();
    const JPH::Body& first = swap ? other : one;
    const JPH::Body& second = swap ? one : other;
    ContactEvent event;
    event.first = BodyId{first.GetID().GetIndexAndSequenceNumber()};
    event.second = BodyId{second.GetID().GetIndexAndSequenceNumber()};
    event.firstUserData = first.GetUserData();
    event.secondUserData = second.GetUserData();
    event.sensor = one.IsSensor() || other.IsSensor();
    event.position = fromJolt(middle);
    event.normal = normalizeOr(fromJolt(swap ? -normal : normal), kWorldUp);
    event.speed = isFinite(closing) ? std::max(closing, 0.0f) : 0.0f;
    return event;
}

/// Hears from Jolt, while a step is taken, which bodies touch - and works out from
/// that, after the step, which have begun to and which have stopped.
///
/// Jolt speaks of two bodies in every step in which they touch and one of them is
/// awake, once for each pair of their shapes that meet. It says nothing of two that
/// have both stopped moving, and it calls a contact removed when a body falls asleep
/// - which is not what anybody who waits for two things to part wants to be told. So
/// what Jolt calls removed is not listened to: two bodies have parted when a step
/// goes by in which nothing is heard of them although one of them is awake, or when
/// one of them is gone.
class ContactRecorder final : public JPH::ContactListener, public JPH::BodyActivationListener {
public:
    void OnContactAdded(const JPH::Body& one, const JPH::Body& other, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*settings*/) override {
        hear(one, other, manifold);
    }

    void OnContactPersisted(const JPH::Body& one, const JPH::Body& other, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& /*settings*/) override {
        hear(one, other, manifold);
    }

    void OnBodyActivated(const JPH::BodyID& /*body*/, JPH::uint64 /*userData*/) override { m_wokeOrGone = true; }

    void OnBodyDeactivated(const JPH::BodyID& /*body*/, JPH::uint64 /*userData*/) override {}

    /// A body was destroyed: what it touched, it touches no more.
    void noteGone() { m_wokeOrGone = true; }

    void beginStep(uint64_t step) {
        const std::scoped_lock lock(m_mutex);
        m_step = step;
        m_began.clear();
    }

    /// Fills `events` with what began and what ended in the step that was just taken.
    void endStep(const JPH::BodyInterface& bodies, std::vector<ContactEvent>& events) {
        const std::scoped_lock lock(m_mutex);
        events.clear();
        for (const uint64_t pair : m_began) {
            events.push_back(m_touches.at(pair).began);
        }
        // Two that sleep cannot have parted unless something was woken or destroyed.
        const bool lookAtDormant = m_wokeOrGone.exchange(false);
        for (auto entry = m_touches.begin(); entry != m_touches.end();) {
            Touch& touch = entry->second;
            if (touch.heardAtStep == m_step) {
                touch.dormant = false;
                ++entry;
                continue;
            }
            if (touch.dormant && !lookAtDormant) {
                ++entry;
                continue;
            }
            const JPH::BodyID first(touch.began.first.value);
            const JPH::BodyID second(touch.began.second.value);
            // Two that have both stopped moving - asleep, or never moving - are where
            // they were, and touch on.
            if (bodies.IsAdded(first) && bodies.IsAdded(second) && !bodies.IsActive(first) &&
                !bodies.IsActive(second)) {
                touch.dormant = true;
                ++entry;
                continue;
            }
            ContactEvent ended = touch.began;
            ended.phase = ContactPhase::Ended;
            ended.position = Vec3(0.0f);
            ended.normal = kWorldUp;
            ended.speed = 0.0f;
            events.push_back(ended);
            entry = m_touches.erase(entry);
        }
        // Jolt hears of contacts in whatever order its work is done in; what is said of
        // them is in the order of the bodies.
        std::ranges::sort(events, [](const ContactEvent& left, const ContactEvent& right) {
            const std::array leftKey{placeOf(left.first), placeOf(left.second), static_cast<uint32_t>(left.phase)};
            const std::array rightKey{placeOf(right.first), placeOf(right.second), static_cast<uint32_t>(right.phase)};
            return leftKey < rightKey;
        });
    }

private:
    void hear(const JPH::Body& one, const JPH::Body& other, const JPH::ContactManifold& manifold) {
        // Jolt may call from several threads at once.
        const std::scoped_lock lock(m_mutex);
        const uint64_t low =
            std::min(one.GetID().GetIndexAndSequenceNumber(), other.GetID().GetIndexAndSequenceNumber());
        const uint64_t high =
            std::max(one.GetID().GetIndexAndSequenceNumber(), other.GetID().GetIndexAndSequenceNumber());
        const uint64_t pair = (low << 32u) | high;
        const auto [entry, isNew] = m_touches.try_emplace(pair);
        if (isNew) {
            entry->second.began = describeContact(one, other, manifold);
            m_began.push_back(pair);
        }
        entry->second.heardAtStep = m_step;
    }

    std::mutex m_mutex;
    /// The pairs of bodies that touch, by their two ids.
    std::unordered_map<uint64_t, Touch> m_touches;
    /// Those of them that were first heard of in the step that is being taken.
    std::vector<uint64_t> m_began;
    uint64_t m_step = 0;
    /// Since dormant pairs were last looked at. True at first, for what was there before.
    std::atomic<bool> m_wokeOrGone{true};
};

// --- Edges, for tools that show geometry -----------------------------------------------------

/// An edge whatever way round it is given: its two ends in an order of their own.
struct EdgeKey {
    std::array<float, 6> ends{};

    EdgeKey(const Vec3& from, const Vec3& to) {
        const bool swap = std::array{to.x, to.y, to.z} < std::array{from.x, from.y, from.z};
        const Vec3& first = swap ? to : from;
        const Vec3& second = swap ? from : to;
        ends = {first.x, first.y, first.z, second.x, second.y, second.z};
    }

    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash {
    size_t operator()(const EdgeKey& edge) const noexcept {
        // FNV-1a over the bits of the six numbers. The two ends of an edge come out of
        // the same table of corners whichever triangle names them, bit for bit.
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const float value : edge.ends) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            hash = (hash ^ bits) * 0x100000001b3ull;
        }
        return static_cast<size_t>(hash);
    }
};

/// Gathers edges, each once, until there are more than may be: then it is full, and
/// what it holds is of no use.
class EdgeList {
public:
    explicit EdgeList(size_t maxEdges) : m_maxEdges(maxEdges) {}

    void add(const Vec3& from, const Vec3& to) {
        if (m_full || from == to || !m_seen.emplace(from, to).second) {
            return;
        }
        if (m_seen.size() > m_maxEdges) {
            m_full = true;
            return;
        }
        m_points.push_back(from);
        m_points.push_back(to);
    }

    [[nodiscard]] bool isFull() const { return m_full; }

    [[nodiscard]] std::vector<Vec3> take() { return m_full ? std::vector<Vec3>() : std::move(m_points); }

private:
    size_t m_maxEdges;
    bool m_full = false;
    std::unordered_set<EdgeKey, EdgeKeyHash> m_seen;
    std::vector<Vec3> m_points;
};

/// The edges of the faces of a hull.
void addHullEdges(const JPH::ConvexHullShape& hull, EdgeList& edges) {
    // Jolt keeps the corners of a hull against its centre of mass.
    const Vec3 middle = fromJolt(hull.GetCenterOfMass());
    std::array<JPH::uint, JPH::ConvexHullShape::cMaxPointsInHull> corners{};
    for (JPH::uint face = 0; face < hull.GetNumFaces() && !edges.isFull(); ++face) {
        const JPH::uint count =
            std::min(hull.GetFaceVertices(face, static_cast<JPH::uint>(corners.size()), corners.data()),
                     static_cast<JPH::uint>(corners.size()));
        for (JPH::uint corner = 0; corner < count; ++corner) {
            edges.add(middle + fromJolt(hull.GetPoint(corners.at(corner))),
                      middle + fromJolt(hull.GetPoint(corners.at((corner + 1) % count))));
        }
    }
}

/// The sides of the triangles of a surface.
void addTriangleEdges(const JPH::Shape& surface, EdgeList& edges) {
    constexpr int kTrianglesAtATime = 256;
    static_assert(kTrianglesAtATime >= JPH::Shape::cGetTrianglesMinTrianglesRequested);
    std::vector<JPH::Float3> corners(static_cast<size_t>(kTrianglesAtATime) * 3);
    const auto cornerAt = [&](size_t index) { return Vec3(corners[index].x, corners[index].y, corners[index].z); };

    JPH::Shape::GetTrianglesContext context{};
    // Everything, where it was given: not moved, not turned, not scaled.
    surface.GetTrianglesStart(context, JPH::AABox::sBiggest(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(),
                              JPH::Vec3::sOne());
    while (!edges.isFull()) {
        const int count = surface.GetTrianglesNext(context, kTrianglesAtATime, corners.data());
        if (count <= 0) {
            break;
        }
        for (size_t triangle = 0; triangle < static_cast<size_t>(count); ++triangle) {
            const Vec3 a = cornerAt(triangle * 3);
            const Vec3 b = cornerAt(triangle * 3 + 1);
            const Vec3 c = cornerAt(triangle * 3 + 2);
            edges.add(a, b);
            edges.add(b, c);
            edges.add(c, a);
        }
    }
}

} // namespace

std::string_view nameOf(BodyMotion motion) {
    switch (motion) {
        case BodyMotion::Static:
            return "static";
        case BodyMotion::Kinematic:
            return "kinematic";
        case BodyMotion::Dynamic:
            return "dynamic";
    }
    return "unknown";
}

std::string_view nameOf(ShapeType type) {
    switch (type) {
        case ShapeType::Box:
            return "box";
        case ShapeType::Sphere:
            return "sphere";
        case ShapeType::Capsule:
            return "capsule";
        case ShapeType::ConvexHull:
            return "convexHull";
        case ShapeType::Mesh:
            return "mesh";
    }
    return "unknown";
}

struct PhysicsWorld::Impl {
    Impl() : broadPhaseLayers(kObjectLayerCount, kBroadPhaseLayerCount), objectLayerPairs(kObjectLayerCount) {
        broadPhaseLayers.MapObjectToBroadPhaseLayer(kStaticLayer, kStaticBroadPhase);
        broadPhaseLayers.MapObjectToBroadPhaseLayer(kMovingLayer, kMovingBroadPhase);
        broadPhaseLayers.MapObjectToBroadPhaseLayer(kSensorLayer, kSensorBroadPhase);
        // What moves collides with everything; what does not, only with what moves;
        // and what only notices, notices what moves.
        objectLayerPairs.EnableCollision(kMovingLayer, kMovingLayer);
        objectLayerPairs.EnableCollision(kMovingLayer, kStaticLayer);
        objectLayerPairs.EnableCollision(kMovingLayer, kSensorLayer);
    }

    [[nodiscard]] const JPH::BodyInterface& bodies() const { return system.GetBodyInterface(); }

    [[nodiscard]] JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }

    /// The bodies that reach into a shape that is put somewhere: each once, by the
    /// place it has among the bodies of the world.
    [[nodiscard]] std::vector<BodyId> bodiesIn(const JPH::Shape& shape, const Vec3& center,
                                               const Quat& rotation) const {
        JPH::CollideShapeSettings settings;
        // A surface of triangles is found from behind as well, as a ray finds it.
        settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        system.GetNarrowPhaseQuery().CollideShape(
            &shape, JPH::Vec3::sOne(), JPH::RMat44::sRotationTranslation(toJolt(rotation), toJolt(center)), settings,
            toJolt(center), collector, SolidBroadPhaseFilter(), SolidObjectFilter());
        // A body of several shapes is met once for each of them that is reached.
        std::vector<JPH::BodyID> met;
        met.reserve(collector.mHits.size());
        for (const JPH::CollideShapeResult& hit : collector.mHits) {
            met.push_back(hit.mBodyID2);
        }
        std::ranges::sort(met, {}, [](const JPH::BodyID& body) { return body.GetIndex(); });
        const auto repeated = std::ranges::unique(met);
        met.erase(repeated.begin(), repeated.end());
        std::vector<BodyId> found;
        found.reserve(met.size());
        for (const JPH::BodyID& body : met) {
            found.push_back(BodyId{body.GetIndexAndSequenceNumber()});
        }
        return found;
    }

    /// Keeps a shape as geometry, under an id that no geometry of this world has had.
    [[nodiscard]] GeometryId addGeometry(JPH::ShapeRefC shape, ShapeType type) {
        // Counted up, and so used once each - until the count comes round after four
        // thousand million of them, when those still in use are passed over.
        do {
            ++lastGeometry;
        } while (lastGeometry == GeometryId::kNone || geometries.contains(lastGeometry));
        geometries.emplace(lastGeometry, Geometry{.shape = std::move(shape), .type = type});
        return GeometryId{lastGeometry};
    }

    // Declared in the order the library needs them made, and unmade in reverse.
    JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers;
    JPH::ObjectLayerPairFilterTable objectLayerPairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    std::unique_ptr<JPH::TempAllocator> stepMemory;
    std::unique_ptr<JPH::JobSystem> jobs;
    JPH::PhysicsSystem system;

    ContactRecorder contacts;
    /// What began to touch and what stopped in the last step.
    std::vector<ContactEvent> contactEvents;

    GeometryTable geometries;
    uint32_t lastGeometry = GeometryId::kNone;
    size_t maxGeometryPoints = 0;
    size_t maxGeometryTriangles = 0;

    uint64_t steps = 0;
    uint64_t overflowedSteps = 0;
    bool reportedOverflow = false;
};

PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

PhysicsWorld::~PhysicsWorld() {
    if (m_impl) {
        // The bodies go with the system; the library's registry after the last world.
        m_impl.reset();
        JoltRuntime::release();
    }
}

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::create(const PhysicsWorldDesc& desc) {
    if (desc.maxBodies == 0 || desc.maxBodyPairs == 0 || desc.maxContactConstraints == 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "a physics world needs room for bodies, pairs of bodies and contacts: {} / {} / {}",
                         desc.maxBodies, desc.maxBodyPairs, desc.maxContactConstraints);
    }
    if (desc.maxGeometryPoints == 0 || desc.maxGeometryTriangles == 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "a physics world needs geometry to be allowed points and triangles: {} / {}",
                         desc.maxGeometryPoints, desc.maxGeometryTriangles);
    }
    if (!isFinite(desc.gravity)) {
        return makeError(ErrorCode::InvalidArgument, "the gravity of a physics world is not made of numbers");
    }

    JoltRuntime::acquire();
    auto impl = std::make_unique<Impl>();
    impl->objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        impl->broadPhaseLayers, kBroadPhaseLayerCount, impl->objectLayerPairs, kObjectLayerCount);
    impl->stepMemory = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(kStepMemoryBytes);
    // Jolt's jobs are run where they are made, one after the other: the simulation is
    // the same however many processors there are.
    impl->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
    impl->system.Init(desc.maxBodies, kBodyMutexCount, desc.maxBodyPairs, desc.maxContactConstraints,
                      impl->broadPhaseLayers, *impl->objectVsBroadPhase, impl->objectLayerPairs);
    impl->system.SetGravity(toJolt(desc.gravity));
    impl->system.SetContactListener(&impl->contacts);
    impl->system.SetBodyActivationListener(&impl->contacts);
    impl->maxGeometryPoints = desc.maxGeometryPoints;
    impl->maxGeometryTriangles = desc.maxGeometryTriangles;
    return std::unique_ptr<PhysicsWorld>(new PhysicsWorld(std::move(impl)));
}

Result<GeometryId> PhysicsWorld::createConvexHull(std::span<const Vec3> points) {
    constexpr size_t kFewestPoints = 4;
    if (points.size() < kFewestPoints || points.size() > m_impl->maxGeometryPoints) {
        return makeError(ErrorCode::InvalidArgument, "a convex hull is found around {} to {} points, not {}",
                         kFewestPoints, m_impl->maxGeometryPoints, points.size());
    }
    JPH::Array<JPH::Vec3> corners;
    corners.reserve(points.size());
    for (const Vec3& point : points) {
        if (!isFinite(point)) {
            return makeError(ErrorCode::InvalidArgument, "point {} of {} for a convex hull is not made of numbers",
                             corners.size(), points.size());
        }
        corners.push_back(toJolt(point));
    }
    const JPH::ShapeSettings::ShapeResult result = JPH::ConvexHullShapeSettings(corners).Create();
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "no convex hull was found around {} points: {}", points.size(),
                         reason);
    }
    // Around points that lie in one plane Jolt makes a hull of two faces, back to back.
    // It has no volume, and so no mass to spread: a body of it could not be moved.
    if (!(result.Get()->GetVolume() > 0.0f)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the {} points for a convex hull lie in one plane: there is no volume between them",
                         points.size());
    }
    return m_impl->addGeometry(result.Get(), ShapeType::ConvexHull);
}

Result<GeometryId> PhysicsWorld::createTriangleMesh(std::span<const Vec3> positions,
                                                    std::span<const uint32_t> indices) {
    if (positions.empty() || positions.size() > m_impl->maxGeometryPoints || indices.empty() ||
        indices.size() % 3 != 0 || indices.size() / 3 > m_impl->maxGeometryTriangles) {
        return makeError(ErrorCode::InvalidArgument,
                         "a surface of triangles is made of 1 to {} positions and three indices to each of 1 to {} "
                         "triangles, not of {} positions and {} indices",
                         m_impl->maxGeometryPoints, m_impl->maxGeometryTriangles, positions.size(), indices.size());
    }
    for (size_t index = 0; index < positions.size(); ++index) {
        if (!isFinite(positions[index])) {
            return makeError(ErrorCode::InvalidArgument,
                             "position {} of {} for a surface of triangles is not made of numbers", index,
                             positions.size());
        }
    }
    const auto cornerAt = [&](uint32_t index) {
        const Vec3& position = positions[index];
        return JPH::Float3(position.x, position.y, position.z);
    };
    JPH::TriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (size_t first = 0; first < indices.size(); first += 3) {
        const std::span<const uint32_t> corner = indices.subspan(first, 3);
        if (std::max({corner[0], corner[1], corner[2]}) >= positions.size()) {
            return makeError(ErrorCode::InvalidArgument, "triangle {} of a surface names position {}, and there are {}",
                             first / 3, std::max({corner[0], corner[1], corner[2]}), positions.size());
        }
        triangles.emplace_back(cornerAt(corner[0]), cornerAt(corner[1]), cornerAt(corner[2]));
    }
    // Making the settings takes positions that are in one place for one corner, and
    // leaves out the triangles that have no area or are there twice. The first is what
    // a mesh made for drawing needs: along every seam and sharp edge it has two
    // vertices in one place, and triangles that are not known to meet there are taken
    // for the end of the surface - on which what slides across it catches.
    const JPH::MeshShapeSettings settings(triangles);
    if (settings.mIndexedTriangles.empty()) {
        return makeError(ErrorCode::InvalidArgument, "none of the {} triangles of a surface has an area",
                         indices.size() / 3);
    }
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "no surface was made of {} triangles: {}", indices.size() / 3,
                         reason);
    }
    return m_impl->addGeometry(result.Get(), ShapeType::Mesh);
}

void PhysicsWorld::destroyGeometry(GeometryId geometry) {
    m_impl->geometries.erase(geometry.value);
}

bool PhysicsWorld::isValid(GeometryId geometry) const {
    return m_impl->geometries.contains(geometry.value);
}

Aabb PhysicsWorld::geometryBounds(GeometryId geometry) const {
    const auto found = m_impl->geometries.find(geometry.value);
    if (found == m_impl->geometries.end()) {
        return {};
    }
    // Jolt keeps a shape against its centre of mass, and its bounds with it.
    const JPH::Shape& shape = *found->second.shape;
    const JPH::AABox bounds = shape.GetLocalBounds();
    const Vec3 middle = fromJolt(shape.GetCenterOfMass());
    return {.min = middle + fromJolt(bounds.mMin), .max = middle + fromJolt(bounds.mMax)};
}

std::vector<Vec3> PhysicsWorld::geometryEdges(GeometryId geometry, size_t maxEdges) const {
    const auto found = m_impl->geometries.find(geometry.value);
    if (found == m_impl->geometries.end()) {
        return {};
    }
    EdgeList edges(maxEdges);
    if (found->second.type == ShapeType::ConvexHull) {
        // What createConvexHull() has made, and nothing else, is kept as a hull.
        addHullEdges(static_cast<const JPH::ConvexHullShape&>(*found->second.shape), edges);
    } else {
        addTriangleEdges(*found->second.shape, edges);
    }
    return edges.take();
}

Result<BodyId> PhysicsWorld::createBody(const BodyDesc& desc) {
    CORDITE_TRY(validateBody(desc));
    CORDITE_TRY_ASSIGN(const JPH::ShapeRefC shape, makeBodyShape(desc.shapes, m_impl->geometries));
    // A surface of triangles has no inside, and so no mass that could be spread over it.
    if (desc.motion == BodyMotion::Dynamic && shape->MustBeStatic()) {
        return makeError(ErrorCode::InvalidArgument,
                         "a dynamic body cannot be made of the triangles of a mesh, which have no inside: a convex "
                         "hull has one");
    }

    const bool isStatic = desc.motion == BodyMotion::Static;
    JPH::ObjectLayer layer = isStatic ? kStaticLayer : kMovingLayer;
    if (desc.sensor) {
        layer = kSensorLayer;
    }
    JPH::BodyCreationSettings settings(shape, toJolt(desc.position), toJolt(glm::normalize(desc.rotation)),
                                       joltMotionOf(desc.motion), layer);
    settings.mIsSensor = desc.sensor;
    // A sensor notices kinematic bodies as well as dynamic ones. What stands still is
    // kept from it by its layer.
    settings.mCollideKinematicVsNonDynamic = desc.sensor;
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linearDamping;
    settings.mAngularDamping = desc.angularDamping;
    settings.mGravityFactor = desc.gravityScale;
    settings.mMotionQuality = desc.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    // Asleep, a sensor would notice nothing.
    settings.mAllowSleeping = desc.allowSleep && !desc.sensor;
    settings.mUserData = desc.userData;
    if (desc.motion == BodyMotion::Dynamic) {
        // The mass is what was asked for; how it is spread follows from the shape.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    } else if (desc.motion == BodyMotion::Kinematic && shape->MustBeStatic()) {
        // Nothing that a kinematic body meets moves it, so its mass does not matter -
        // but Jolt wants one of every body that moves, and cannot work it out of a
        // surface. Any will do: that of a box of a metre, of water.
        constexpr float kAnyDensity = 1000.0f;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(JPH::Vec3::sOne(), kAnyDensity);
    }

    const JPH::BodyID body = m_impl->bodies().CreateAndAddBody(settings, isStatic ? JPH::EActivation::DontActivate
                                                                                  : JPH::EActivation::Activate);
    if (body.IsInvalid()) {
        return makeError(ErrorCode::OutOfRange, "the physics world is full: it holds {} bodies",
                         m_impl->system.GetMaxBodies());
    }
    return BodyId{body.GetIndexAndSequenceNumber()};
}

void PhysicsWorld::destroyBody(BodyId body) {
    if (!isValid(body)) {
        return;
    }
    m_impl->bodies().RemoveBody(joltIdOf(body));
    m_impl->bodies().DestroyBody(joltIdOf(body));
    m_impl->contacts.noteGone();
}

bool PhysicsWorld::isValid(BodyId body) const {
    return !body.isNull() && m_impl->bodies().IsAdded(joltIdOf(body));
}

BodyMotion PhysicsWorld::motion(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? motionOf(m_impl->bodies().GetMotionType(joltIdOf(body))) : BodyMotion::Static;
}

bool PhysicsWorld::isSensor(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) && m_impl->bodies().IsSensor(joltIdOf(body));
}

uint64_t PhysicsWorld::userData(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? m_impl->bodies().GetUserData(joltIdOf(body)) : 0;
}

Vec3 PhysicsWorld::position(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? fromJolt(m_impl->bodies().GetPosition(joltIdOf(body))) : Vec3(0.0f);
}

Quat PhysicsWorld::rotation(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? fromJolt(m_impl->bodies().GetRotation(joltIdOf(body))) : kIdentityQuat;
}

void PhysicsWorld::setPlacement(BodyId body, const Vec3& position, const Quat& rotation) {
    const bool usable = isFinite(position) && isRotation(rotation);
    CORDITE_ASSERT(isValid(body) && usable, "a body of this world and a placement made of numbers are needed");
    if (!isValid(body) || !usable) {
        return;
    }
    m_impl->bodies().SetPositionAndRotation(joltIdOf(body), toJolt(position), toJolt(glm::normalize(rotation)),
                                            JPH::EActivation::Activate);
}

void PhysicsWorld::moveKinematic(BodyId body, const Vec3& position, const Quat& rotation, float seconds) {
    const bool usable = isFinite(position) && isRotation(rotation) && isFinite(seconds) && seconds > 0.0f;
    CORDITE_ASSERT(isValid(body) && usable,
                   "a body of this world, a placement made of numbers and a time of more than nothing are needed");
    if (!isValid(body) || !usable || motion(body) != BodyMotion::Kinematic) {
        return;
    }
    m_impl->bodies().MoveKinematic(joltIdOf(body), toJolt(position), toJolt(glm::normalize(rotation)), seconds);
}

Vec3 PhysicsWorld::linearVelocity(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? fromJolt(m_impl->bodies().GetLinearVelocity(joltIdOf(body))) : Vec3(0.0f);
}

Vec3 PhysicsWorld::angularVelocity(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? fromJolt(m_impl->bodies().GetAngularVelocity(joltIdOf(body))) : Vec3(0.0f);
}

void PhysicsWorld::setLinearVelocity(BodyId body, const Vec3& velocity) {
    CORDITE_ASSERT(isValid(body) && isFinite(velocity),
                   "a body of this world and a velocity made of numbers are needed");
    if (!isValid(body) || !isFinite(velocity) || motion(body) == BodyMotion::Static) {
        return;
    }
    m_impl->bodies().SetLinearVelocity(joltIdOf(body), toJolt(velocity));
}

void PhysicsWorld::setAngularVelocity(BodyId body, const Vec3& velocity) {
    CORDITE_ASSERT(isValid(body) && isFinite(velocity),
                   "a body of this world and a velocity made of numbers are needed");
    if (!isValid(body) || !isFinite(velocity) || motion(body) == BodyMotion::Static) {
        return;
    }
    m_impl->bodies().SetAngularVelocity(joltIdOf(body), toJolt(velocity));
}

void PhysicsWorld::addForce(BodyId body, const Vec3& force) {
    CORDITE_ASSERT(isValid(body) && isFinite(force), "a body of this world and a force made of numbers are needed");
    if (!isValid(body) || !isFinite(force) || motion(body) != BodyMotion::Dynamic) {
        return;
    }
    m_impl->bodies().AddForce(joltIdOf(body), toJolt(force));
}

void PhysicsWorld::addImpulse(BodyId body, const Vec3& impulse) {
    CORDITE_ASSERT(isValid(body) && isFinite(impulse),
                   "a body of this world and an impulse made of numbers are needed");
    if (!isValid(body) || !isFinite(impulse) || motion(body) != BodyMotion::Dynamic) {
        return;
    }
    m_impl->bodies().AddImpulse(joltIdOf(body), toJolt(impulse));
}

void PhysicsWorld::addTorque(BodyId body, const Vec3& torque) {
    CORDITE_ASSERT(isValid(body) && isFinite(torque), "a body of this world and a torque made of numbers are needed");
    if (!isValid(body) || !isFinite(torque) || motion(body) != BodyMotion::Dynamic) {
        return;
    }
    m_impl->bodies().AddTorque(joltIdOf(body), toJolt(torque));
}

bool PhysicsWorld::isActive(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) && m_impl->bodies().IsActive(joltIdOf(body));
}

void PhysicsWorld::activate(BodyId body) {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    if (isValid(body)) {
        m_impl->bodies().ActivateBody(joltIdOf(body));
    }
}

void PhysicsWorld::step(float seconds) {
    if (!isFinite(seconds) || seconds <= 0.0f) {
        return;
    }
    m_impl->contacts.beginStep(m_impl->steps + 1);
    const JPH::EPhysicsUpdateError error =
        m_impl->system.Update(seconds, 1, m_impl->stepMemory.get(), m_impl->jobs.get());
    ++m_impl->steps;
    m_impl->contacts.endStep(m_impl->bodies(), m_impl->contactEvents);
    if (error != JPH::EPhysicsUpdateError::None) {
        ++m_impl->overflowedSteps;
        if (!m_impl->reportedOverflow) {
            CORDITE_LOG_WARN("Physics",
                             "a step had more bodies near each other, or more contacts between them, than the "
                             "physics world has room for; some contacts were left out (see PhysicsWorldDesc)");
            m_impl->reportedOverflow = true;
        }
    }
}

std::span<const ContactEvent> PhysicsWorld::contactEvents() const {
    return m_impl->contactEvents;
}

std::optional<RayHit> PhysicsWorld::raycast(const Ray& ray, float maxDistance) const {
    const float length = glm::length(ray.direction);
    if (!isFinite(ray.origin) || !isFinite(length) || length <= kEpsilon || !isFinite(maxDistance) ||
        maxDistance <= 0.0f) {
        return std::nullopt;
    }
    const Vec3 direction = ray.direction / length;
    // Jolt's ray is a segment: where it starts, and how far it goes.
    const JPH::RRayCast segment(toJolt(ray.origin), toJolt(direction * maxDistance));
    JPH::RayCastResult result;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(segment, result, SolidBroadPhaseFilter(), SolidObjectFilter())) {
        return std::nullopt;
    }

    RayHit hit;
    hit.body = BodyId{result.mBodyID.GetIndexAndSequenceNumber()};
    hit.distance = result.mFraction * maxDistance;
    hit.position = ray.origin + direction * hit.distance;
    const JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.userData = body.GetUserData();
        hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, toJolt(hit.position)));
    }
    return hit;
}

std::optional<RayHit> PhysicsWorld::sphereCast(const Ray& ray, float radius, float maxDistance) const {
    const float length = glm::length(ray.direction);
    if (!isFinite(ray.origin) || !isFinite(length) || length <= kEpsilon || !isFinite(maxDistance) ||
        maxDistance <= 0.0f || !isFinite(radius) || radius <= 0.0f) {
        return std::nullopt;
    }
    const Vec3 direction = ray.direction / length;
    // A shape that is not shared, and lives no longer than this call.
    const JPH::SphereShape ball(std::max(radius, 0.5f * kMinShapeExtent));
    ball.SetEmbedded();
    const JPH::RShapeCast sweep = JPH::RShapeCast::sFromWorldTransform(
        &ball, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(toJolt(ray.origin)), toJolt(direction * maxDistance));
    JPH::ShapeCastSettings settings;
    // A surface of triangles is found from behind as well, as a ray finds it.
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    m_impl->system.GetNarrowPhaseQuery().CastShape(sweep, settings, toJolt(ray.origin), collector,
                                                   SolidBroadPhaseFilter(), SolidObjectFilter());
    if (!collector.HadHit()) {
        return std::nullopt;
    }

    const JPH::ShapeCastResult& result = collector.mHit;
    RayHit hit;
    hit.body = BodyId{result.mBodyID2.GetIndexAndSequenceNumber()};
    hit.distance = result.mFraction * maxDistance;
    // Points are given against where the ball started.
    hit.position = ray.origin + fromJolt(result.mContactPointOn2);
    // The axis points from the ball into the body; where they only just touch it has
    // no length, and the ball came the way it was going.
    hit.normal = normalizeOr(-fromJolt(result.mPenetrationAxis), -direction);
    const JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID2);
    if (lock.Succeeded()) {
        hit.userData = lock.GetBody().GetUserData();
    }
    return hit;
}

std::vector<BodyId> PhysicsWorld::overlapSphere(const Vec3& center, float radius) const {
    if (!isFinite(center) || !isFinite(radius) || radius <= 0.0f) {
        return {};
    }
    const JPH::SphereShape ball(std::max(radius, 0.5f * kMinShapeExtent));
    ball.SetEmbedded();
    return m_impl->bodiesIn(ball, center, kIdentityQuat);
}

std::vector<BodyId> PhysicsWorld::overlapBox(const Vec3& center, const Vec3& size, const Quat& rotation) const {
    if (!isFinite(center) || !isFinite(size) || std::min({size.x, size.y, size.z}) <= 0.0f || !isRotation(rotation)) {
        return {};
    }
    // With sharp edges: what is asked about is the box, to its corners.
    const JPH::BoxShape box(toJolt(0.5f * glm::max(size, Vec3(kMinShapeExtent))), 0.0f);
    box.SetEmbedded();
    return m_impl->bodiesIn(box, center, glm::normalize(rotation));
}

Vec3 PhysicsWorld::gravity() const {
    return fromJolt(m_impl->system.GetGravity());
}

void PhysicsWorld::setGravity(const Vec3& gravity) {
    CORDITE_ASSERT(isFinite(gravity), "the gravity of a physics world is made of numbers");
    if (isFinite(gravity)) {
        m_impl->system.SetGravity(toJolt(gravity));
    }
}

PhysicsStatistics PhysicsWorld::statistics() const {
    PhysicsStatistics statistics;
    statistics.bodies = m_impl->system.GetNumBodies();
    statistics.activeBodies = m_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    statistics.geometries = static_cast<uint32_t>(m_impl->geometries.size());
    statistics.steps = m_impl->steps;
    statistics.overflowedSteps = m_impl->overflowedSteps;
    return statistics;
}

} // namespace cordite
