#include "cordite/physics/PhysicsSystem.h"

#include "cordite/core/Log.h"
#include "cordite/physics/PhysicsComponents.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace cordite {
namespace {

/// How far a body that its entity places may be from where its entity is before it is
/// put there, and how much it may be turned: less than this is rounding, not somebody
/// moving the entity. The first is a share of how far from the middle of the world
/// things are; the second is half an angle in radians, about a thousandth of a degree.
constexpr float kPlacementTolerance = 1.0e-5f;
constexpr float kRotationTolerance = 1.0e-5f;

bool isSamePlace(const Vec3& a, const Vec3& b) {
    return glm::distance(a, b) <= kPlacementTolerance * (1.0f + std::max(glm::length(a), glm::length(b)));
}

bool isSameRotation(const Quat& a, const Quat& b) {
    // Two quaternions are the same rotation when they are the same four numbers, or the
    // opposite ones. How far apart they are is half the angle between the two rotations.
    const Vec4 first(a.x, a.y, a.z, a.w);
    const Vec4 second(b.x, b.y, b.z, b.w);
    return std::min(glm::distance(first, second), glm::distance(first, -second)) <= kRotationTolerance;
}

bool isUsable(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool isUsable(const Quat& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

/// The scale Transform::fromMatrix() finds in a matrix that scales by `scale`: every
/// axis as long as it is, and X the other way round where the three together mirror.
/// A body is turned as that function finds its entity turned, and so this is how large
/// - and which way round - the entity is in its body.
Vec3 foldedScale(const Vec3& scale) {
    const Vec3 size = glm::abs(scale);
    const bool mirrors = ((scale.x < 0.0f) != (scale.y < 0.0f)) != (scale.z < 0.0f);
    return mirrors ? Vec3(-size.x, size.y, size.z) : size;
}

/// How a matrix turns what it places, given the scale that is to go with it: its axes,
/// each made as long as one, and the other way round where the scale is negative. An
/// axis without length is that of the world. Shear is discarded.
Quat rotationOf(const Mat4& matrix, const Vec3& scale) {
    Mat3 axes(1.0f);
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        const Vec3 direction(matrix[axis]);
        const float length = glm::length(direction);
        if (length > kEpsilon) {
            axes[axis] = direction / (scale[axis] < 0.0f ? -length : length);
        }
    }
    return glm::normalize(glm::quat_cast(axes));
}

/// The matrix of what an entity hangs under, worked out from the transforms as they are
/// now; that of nothing at all for an entity that hangs under nothing.
Mat4 parentMatrixOf(const World& world, Entity entity) {
    const Entity parent = world.parent(entity);
    return parent.isNull() ? Mat4(1.0f) : world.computeWorldMatrix(parent);
}

/// The shape a collider asks for: as large as its entity is scaled, and placed in the
/// body it is part of. `entityToBody` takes a point of the collider's entity to the
/// space of the body, which is not scaled.
ShapeDesc shapeOf(const ColliderComponent& collider, const Mat4& entityToBody) {
    const Transform placed = Transform::fromMatrix(entityToBody);
    const Vec3 scale = glm::abs(placed.scale);

    ShapeDesc shape;
    shape.type = collider.shape;
    shape.size = collider.size * scale;
    // A ball stays a ball and a capsule round: both grow by the largest scale across.
    const float across =
        collider.shape == ShapeType::Capsule ? std::max(scale.x, scale.z) : std::max({scale.x, scale.y, scale.z});
    shape.radius = collider.radius * across;
    shape.height = collider.height * scale.y;
    shape.offset = Vec3(entityToBody * Vec4(collider.center, 1.0f));
    shape.rotation = placed.rotation;
    return shape;
}

} // namespace

PhysicsSystem::PhysicsSystem(std::unique_ptr<PhysicsWorld> physics, const PhysicsSystemDesc& desc)
    : m_physics(std::move(physics)), m_timestep(desc.stepSeconds, desc.maxStepsPerUpdate) {}

PhysicsSystem::~PhysicsSystem() = default;

Result<std::unique_ptr<PhysicsSystem>> PhysicsSystem::create(const PhysicsSystemDesc& desc) {
    if (!std::isfinite(desc.stepSeconds) || desc.stepSeconds <= 0.0 || desc.maxStepsPerUpdate == 0) {
        return makeError(ErrorCode::InvalidArgument,
                         "the physics needs steps of more than no time, and at least one to an update: {} "
                         "seconds, {} steps",
                         desc.stepSeconds, desc.maxStepsPerUpdate);
    }
    CORDITE_TRY_ASSIGN(std::unique_ptr<PhysicsWorld> physics, PhysicsWorld::create(desc.world));
    return std::unique_ptr<PhysicsSystem>(new PhysicsSystem(std::move(physics), desc));
}

BodyId PhysicsSystem::bodyOf(Entity entity) const {
    const auto found = m_records.find(entity);
    return found != m_records.end() ? found->second.body : BodyId();
}

Entity PhysicsSystem::entityOf(BodyId body) const {
    if (!m_physics->isValid(body)) {
        return Entity{};
    }
    // A body carries the handle of its entity.
    const Entity entity{static_cast<entt::entity>(static_cast<uint32_t>(m_physics->userData(body)))};
    const auto found = m_records.find(entity);
    return found != m_records.end() && found->second.body == body ? entity : Entity{};
}

void PhysicsSystem::clear() {
    for (const auto& [entity, record] : m_records) {
        m_physics->destroyBody(record.body);
    }
    m_records.clear();
    m_order.clear();
    m_wantedCount = 0;
    m_reported.clear();
    m_timestep.reset();
}

void PhysicsSystem::gather(const World& world) {
    m_wantedCount = 0;
    world.traverse([&](Entity entity, uint32_t depth) {
        if (m_levels.size() <= depth) {
            m_levels.resize(depth + 1);
        }
        Level& level = m_levels[depth];
        const Level* above = depth > 0 ? &m_levels[depth - 1] : nullptr;
        level.enabled = (above == nullptr || above->enabled) && world.isEnabled(entity);
        level.body = level.enabled && above != nullptr ? above->body : kNoBody;
        if (!level.enabled) {
            return;
        }
        // How large things are here, and what takes a point from here to the body the
        // colliders here are part of, are worked out from the entities' own transforms,
        // not from where they are in the world: a body that moves and turns is then
        // made of the same numbers every frame, where the world's matrices, taken
        // apart, differ in the last digit from one frame to the next - and every
        // difference is a new body.
        const Transform& local = world.transform(entity);
        level.scale = (above != nullptr ? above->scale : Vec3(1.0f)) * local.scale;
        level.toBody = level.body != kNoBody ? above->toBody * local.toMatrix() : Mat4(1.0f);

        const auto startBody = [&](BodyMotion motion) {
            // The body is where its entity is, and turned as it is. It has no scale:
            // its entity is as large in it as everything above has made it.
            const Vec3 scale = foldedScale(level.scale);
            level.toBody = glm::scale(Mat4(1.0f), scale);
            const Transform placed = Transform::fromMatrix(world.worldMatrix(entity));
            if (m_wantedCount == m_wanted.size()) {
                m_wanted.emplace_back();
            }
            Wanted& wanted = m_wanted[m_wantedCount];
            // Its list of shapes is emptied and kept: what was allocated for the body
            // asked for here at the update before serves again.
            std::vector<ShapeDesc> shapes = std::move(wanted.blueprint.shapes);
            shapes.clear();
            wanted = Wanted{.entity = entity,
                            .blueprint = BodyDesc{.motion = motion,
                                                  .shapes = std::move(shapes),
                                                  .userData = entt::to_integral(entity.handle)},
                            .position = placed.position,
                            .rotation = placed.rotation,
                            .scale = scale};
            return static_cast<int32_t>(m_wantedCount++);
        };

        if (const auto* rigidBody = world.tryGet<RigidBodyComponent>(entity)) {
            level.body = startBody(rigidBody->motion);
            BodyDesc& body = m_wanted[static_cast<size_t>(level.body)].blueprint;
            body.mass = rigidBody->mass;
            body.linearDamping = rigidBody->linearDamping;
            body.angularDamping = rigidBody->angularDamping;
            body.gravityScale = rigidBody->gravityScale;
            body.continuous = rigidBody->continuous;
            body.allowSleep = rigidBody->allowSleep;
        }
        const auto* collider = world.tryGet<ColliderComponent>(entity);
        if (collider == nullptr) {
            return;
        }
        // A collider is part of the nearest rigid body at or above it. With none, it is
        // a body of its own that stands still - and what hangs below it is not part of
        // that one.
        const int32_t index = level.body != kNoBody ? level.body : startBody(BodyMotion::Static);
        BodyDesc& body = m_wanted[static_cast<size_t>(index)].blueprint;
        if (body.shapes.empty()) {
            // A body has one surface: that of its first collider, which is its own
            // entity's where that has one.
            body.friction = collider->friction;
            body.restitution = collider->restitution;
        }
        body.shapes.push_back(shapeOf(*collider, level.toBody));
    });
}

PhysicsSystem::Record* PhysicsSystem::makeBody(const World& world, const Wanted& wanted, const Record* previous) {
    const auto* rigidBody = world.tryGet<RigidBodyComponent>(wanted.entity);
    // What it was doing it goes on doing: the speeds of the body it replaces, unless
    // its RigidBody was given others since those were written.
    Vec3 linearVelocity = rigidBody != nullptr ? rigidBody->linearVelocity : Vec3(0.0f);
    Vec3 angularVelocity = rigidBody != nullptr ? rigidBody->angularVelocity : Vec3(0.0f);
    if (previous != nullptr) {
        if (rigidBody != nullptr && rigidBody->linearVelocity == previous->linearVelocity) {
            linearVelocity = m_physics->linearVelocity(previous->body);
        }
        if (rigidBody != nullptr && rigidBody->angularVelocity == previous->angularVelocity) {
            angularVelocity = m_physics->angularVelocity(previous->body);
        }
        m_physics->destroyBody(previous->body);
        m_records.erase(wanted.entity);
    }

    BodyDesc desc = wanted.blueprint;
    desc.position = wanted.position;
    desc.rotation = wanted.rotation;
    const Result<BodyId> body = m_physics->createBody(desc);
    if (!body) {
        report(world, wanted.entity, fmt::format("has no body: {}", body.error().message));
        return nullptr;
    }

    Record record;
    record.body = *body;
    record.blueprint = wanted.blueprint;
    record.position = m_physics->position(record.body);
    record.rotation = m_physics->rotation(record.body);
    record.local = world.transform(wanted.entity);
    record.parentToWorld = parentMatrixOf(world, wanted.entity);
    record.scale = wanted.scale;
    // Only a dynamic body has speeds to be given: those of a kinematic one follow from
    // how its entity is moved.
    if (wanted.blueprint.motion == BodyMotion::Dynamic && isUsable(linearVelocity) && isUsable(angularVelocity)) {
        m_physics->setLinearVelocity(record.body, linearVelocity);
        m_physics->setAngularVelocity(record.body, angularVelocity);
        record.linearVelocity = linearVelocity;
        record.angularVelocity = angularVelocity;
    }
    return &m_records.insert_or_assign(wanted.entity, std::move(record)).first->second;
}

void PhysicsSystem::placeBody(const World& world, const Wanted& wanted, Record& record, uint32_t steps,
                              Authority authority) {
    record.scale = wanted.scale;
    if (!isUsable(wanted.position) || !isUsable(wanted.rotation)) {
        report(world, wanted.entity,
               "is nowhere - its place in the world is not made of numbers - and its body stays where it was");
        return;
    }

    const BodyMotion motion = wanted.blueprint.motion;
    if (motion == BodyMotion::Kinematic && authority == Authority::Simulation) {
        // Moved there through the steps that follow, so that it pushes what is in its
        // way - and brought to a halt there, where it is already. A frame without a
        // step leaves it where it is: put there at once it would push nothing, and
        // the next step takes it all the way.
        if (steps > 0) {
            m_physics->moveKinematic(record.body, wanted.position, wanted.rotation,
                                     static_cast<float>(m_timestep.stepSeconds()) * static_cast<float>(steps));
            record.position = wanted.position;
            record.rotation = wanted.rotation;
        }
        return;
    }

    if (motion == BodyMotion::Dynamic) {
        // The simulation moves it, and its entity follows - unless the entity was given
        // another transform since, which takes the body along. While no time passes,
        // so does whatever else has moved the entity: something above it.
        const bool edited = !(world.transform(wanted.entity) == record.local);
        if (!edited && authority == Authority::Simulation) {
            return;
        }
        const Mat4 parentToWorld = parentMatrixOf(world, wanted.entity);
        if (!edited && parentToWorld == record.parentToWorld) {
            return;
        }
        record.local = world.transform(wanted.entity);
        record.parentToWorld = parentToWorld;
    } else if (isSamePlace(wanted.position, record.position) && isSameRotation(wanted.rotation, record.rotation)) {
        // Static, or kinematic while no time passes: it is where its entity is. What
        // is compared with is where the body is, not where its entity was the last
        // time: an entity that creeps is followed as soon as it has gone far enough.
        return;
    }
    m_physics->setPlacement(record.body, wanted.position, wanted.rotation);
    record.position = m_physics->position(record.body);
    record.rotation = m_physics->rotation(record.body);
}

void PhysicsSystem::applyVelocities(const World& world, Entity entity, Record& record) {
    const auto* rigidBody = world.tryGet<RigidBodyComponent>(entity);
    if (rigidBody == nullptr || record.blueprint.motion != BodyMotion::Dynamic) {
        return;
    }
    if (rigidBody->linearVelocity != record.linearVelocity && isUsable(rigidBody->linearVelocity)) {
        m_physics->setLinearVelocity(record.body, rigidBody->linearVelocity);
        record.linearVelocity = rigidBody->linearVelocity;
    }
    if (rigidBody->angularVelocity != record.angularVelocity && isUsable(rigidBody->angularVelocity)) {
        m_physics->setAngularVelocity(record.body, rigidBody->angularVelocity);
        record.angularVelocity = rigidBody->angularVelocity;
    }
}

void PhysicsSystem::reconcile(const World& world, uint32_t steps, Authority authority) {
    for (auto& [entity, record] : m_records) {
        record.seen = false;
    }
    m_order.clear();

    for (const Wanted& wanted : std::span(m_wanted).first(m_wantedCount)) {
        if (wanted.blueprint.shapes.empty()) {
            // A rigid body without a collider has no shape, and is no body.
            continue;
        }
        const auto found = m_records.find(wanted.entity);
        Record* record = found != m_records.end() ? &found->second : nullptr;
        if (record == nullptr || !(record->blueprint == wanted.blueprint)) {
            record = makeBody(world, wanted, record);
        } else {
            placeBody(world, wanted, *record, steps, authority);
            applyVelocities(world, wanted.entity, *record);
        }
        if (record != nullptr) {
            record->seen = true;
            m_order.push_back(wanted.entity);
        }
    }

    // What the world no longer asks for goes.
    std::erase_if(m_records, [&](const auto& entry) {
        if (!entry.second.seen) {
            m_physics->destroyBody(entry.second.body);
        }
        return !entry.second.seen;
    });
    // What no longer holds of an entity is forgotten, and said again should it hold again.
    for (auto said = m_reported.begin(); said != m_reported.end();) {
        if (said->second.current) {
            said->second.current = false;
            ++said;
        } else {
            said = m_reported.erase(said);
        }
    }
}

void PhysicsSystem::writeBack(World& world) {
    // Parents before what is below them: an entity is placed against what it hangs
    // under, which may itself just have been moved.
    for (const Entity entity : m_order) {
        Record& record = m_records.at(entity);
        auto* rigidBody = world.tryGet<RigidBodyComponent>(entity);
        if (rigidBody == nullptr || record.blueprint.motion == BodyMotion::Static) {
            continue;
        }
        record.linearVelocity = m_physics->linearVelocity(record.body);
        record.angularVelocity = m_physics->angularVelocity(record.body);
        rigidBody->linearVelocity = record.linearVelocity;
        rigidBody->angularVelocity = record.angularVelocity;
        if (record.blueprint.motion == BodyMotion::Dynamic) {
            followBody(world, entity, record);
        }
    }
}

void PhysicsSystem::followBody(World& world, Entity entity, Record& record) {
    const Vec3 position = m_physics->position(record.body);
    const Quat rotation = m_physics->rotation(record.body);
    const Mat4 parentToWorld = parentMatrixOf(world, entity);
    if (position == record.position && rotation == record.rotation && parentToWorld == record.parentToWorld) {
        // At rest, under something at rest: the entity is where it was put the last time.
        return;
    }
    record.position = position;
    record.rotation = rotation;
    record.parentToWorld = parentToWorld;

    Transform& local = world.transform(entity);
    Transform placed = local;
    if (parentToWorld == Mat4(1.0f) && record.scale == local.scale) {
        // Nothing above it and nothing mirrored: the entity is where its body is, and
        // turned as its body is.
        placed.position = position;
        placed.rotation = rotation;
    } else {
        // The entity in its body - as large as it is scaled, and the way round it is -
        // seen from what it hangs under. Its scale stays its own; only where it is,
        // and how it is turned, is the body's to say.
        const Mat4 inParent = glm::inverse(parentToWorld) * glm::translate(Mat4(1.0f), position) *
                              glm::mat4_cast(rotation) * glm::scale(Mat4(1.0f), record.scale);
        placed.position = Vec3(inParent[3]);
        placed.rotation = rotationOf(inParent, local.scale);
    }
    if (!isUsable(placed.position) || !isUsable(placed.rotation)) {
        // Under something scaled to nothing there is no place that is where the body
        // is: the entity stays as it is.
        return;
    }
    local = placed;
    record.local = local;
}

void PhysicsSystem::report(const World& world, Entity entity, std::string_view problem) {
    Report& said = m_reported[entity];
    said.current = true;
    if (said.problem != problem) {
        said.problem = problem;
        CORDITE_LOG_ERROR("Physics", "'{}' {}", world.name(entity), problem);
    }
}

PhysicsFrameStatistics PhysicsSystem::statistics(uint32_t steps) const {
    const PhysicsStatistics simulation = m_physics->statistics();
    return {.steps = steps, .bodies = simulation.bodies, .activeBodies = simulation.activeBodies};
}

PhysicsFrameStatistics PhysicsSystem::update(World& world, float deltaSeconds) {
    world.updateWorldTransforms();
    gather(world);
    const uint32_t steps = m_timestep.advance(static_cast<double>(deltaSeconds));
    reconcile(world, steps, Authority::Simulation);
    for (uint32_t step = 0; step < steps; ++step) {
        m_physics->step(static_cast<float>(m_timestep.stepSeconds()));
    }
    // Also after no step at all: an entity that something above it has carried off is
    // put back where its body is.
    writeBack(world);
    return statistics(steps);
}

PhysicsFrameStatistics PhysicsSystem::synchronize(World& world) {
    world.updateWorldTransforms();
    gather(world);
    reconcile(world, 0, Authority::World);
    return statistics(0);
}

} // namespace cordite
