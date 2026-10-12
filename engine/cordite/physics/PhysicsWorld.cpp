#include "cordite/physics/PhysicsWorld.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/physics/JoltSupport.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

// Bodies are sorted into layers that decide what is checked against what. Two are
// enough for now: what never moves is not checked against itself.
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;
constexpr JPH::uint kObjectLayerCount = 2;
constexpr JPH::BroadPhaseLayer kStaticBroadPhase(0);
constexpr JPH::BroadPhaseLayer kMovingBroadPhase(1);
constexpr JPH::uint kBroadPhaseLayerCount = 2;

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

Result<JPH::ShapeRefC> makeShape(const ShapeDesc& desc) {
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
    }
    if (!result.IsValid()) {
        const std::string_view reason = result.HasError() ? std::string_view(result.GetError()) : "no shape";
        return makeError(ErrorCode::InvalidArgument, "cannot make a {} to collide with: {}", nameOf(desc.type), reason);
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
    const auto isShare = [](float value) { return isFinite(value) && value >= 0.0f; };
    if (!isShare(desc.friction) || !isShare(desc.restitution) || !isShare(desc.linearDamping) ||
        !isShare(desc.angularDamping) || !isFinite(desc.gravityScale)) {
        return makeError(ErrorCode::InvalidArgument,
                         "the friction, restitution and damping of a body are numbers that are not negative, and "
                         "its gravity scale is a number");
    }
    return {};
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
    }
    return "unknown";
}

struct PhysicsWorld::Impl {
    Impl() : broadPhaseLayers(kObjectLayerCount, kBroadPhaseLayerCount), objectLayerPairs(kObjectLayerCount) {
        broadPhaseLayers.MapObjectToBroadPhaseLayer(kStaticLayer, kStaticBroadPhase);
        broadPhaseLayers.MapObjectToBroadPhaseLayer(kMovingLayer, kMovingBroadPhase);
        // What moves collides with everything; what does not, only with what moves.
        objectLayerPairs.EnableCollision(kMovingLayer, kMovingLayer);
        objectLayerPairs.EnableCollision(kMovingLayer, kStaticLayer);
    }

    [[nodiscard]] const JPH::BodyInterface& bodies() const { return system.GetBodyInterface(); }

    [[nodiscard]] JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }

    // Declared in the order the library needs them made, and unmade in reverse.
    JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers;
    JPH::ObjectLayerPairFilterTable objectLayerPairs;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    std::unique_ptr<JPH::TempAllocator> stepMemory;
    std::unique_ptr<JPH::JobSystem> jobs;
    JPH::PhysicsSystem system;

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
    return std::unique_ptr<PhysicsWorld>(new PhysicsWorld(std::move(impl)));
}

Result<BodyId> PhysicsWorld::createBody(const BodyDesc& desc) {
    CORDITE_TRY(validateBody(desc));
    CORDITE_TRY_ASSIGN(const JPH::ShapeRefC shape, makeShape(desc.shape));

    const bool isStatic = desc.motion == BodyMotion::Static;
    JPH::BodyCreationSettings settings(shape, toJolt(desc.position), toJolt(glm::normalize(desc.rotation)),
                                       joltMotionOf(desc.motion), isStatic ? kStaticLayer : kMovingLayer);
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mLinearDamping = desc.linearDamping;
    settings.mAngularDamping = desc.angularDamping;
    settings.mGravityFactor = desc.gravityScale;
    settings.mMotionQuality = desc.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    settings.mAllowSleeping = desc.allowSleep;
    settings.mUserData = desc.userData;
    if (desc.motion == BodyMotion::Dynamic) {
        // The mass is what was asked for; how it is spread follows from the shape.
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
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
}

bool PhysicsWorld::isValid(BodyId body) const {
    return !body.isNull() && m_impl->bodies().IsAdded(joltIdOf(body));
}

BodyMotion PhysicsWorld::motion(BodyId body) const {
    CORDITE_ASSERT(isValid(body), "{} is not a body of this world", body.value);
    return isValid(body) ? motionOf(m_impl->bodies().GetMotionType(joltIdOf(body))) : BodyMotion::Static;
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
    const JPH::EPhysicsUpdateError error =
        m_impl->system.Update(seconds, 1, m_impl->stepMemory.get(), m_impl->jobs.get());
    ++m_impl->steps;
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
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(segment, result)) {
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
    statistics.steps = m_impl->steps;
    statistics.overflowedSteps = m_impl->overflowedSteps;
    return statistics;
}

} // namespace cordite
