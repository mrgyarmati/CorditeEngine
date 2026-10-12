#include "cordite/physics/PhysicsSystem.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Mesh.h"
#include "cordite/core/Hash.h"
#include "cordite/core/Log.h"
#include "cordite/physics/PhysicsComponents.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
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

/// A body carries the handle of its entity, and whether it is the entity's trigger.
constexpr uint64_t kTriggerBit = 1ull << 32u;

uint64_t userDataOf(Entity entity, bool trigger) {
    return static_cast<uint64_t>(entt::to_integral(entity.handle)) | (trigger ? kTriggerBit : 0ull);
}

Entity entityOfUserData(uint64_t userData) {
    return Entity{static_cast<entt::entity>(static_cast<uint32_t>(userData))};
}

bool isEnd(CollisionEventType type) {
    return type == CollisionEventType::CollisionEnded || type == CollisionEventType::TriggerLeft;
}

/// Whether an event says of the same two entities that they have begun what `ended`
/// says they have stopped. Two bodies that touch are a pair either way round; a
/// trigger is always the first.
bool isBeginningOf(const CollisionEvent& event, const CollisionEvent& ended) {
    if (ended.type == CollisionEventType::TriggerLeft) {
        return event.type == CollisionEventType::TriggerEntered && event.first == ended.first &&
               event.second == ended.second;
    }
    return event.type == CollisionEventType::CollisionBegan &&
           ((event.first == ended.first && event.second == ended.second) ||
            (event.first == ended.second && event.second == ended.first));
}

bool isMadeOfMesh(ShapeType shape) {
    return shape == ShapeType::ConvexHull || shape == ShapeType::Mesh;
}

/// Makes of a mesh asset what colliders of `shape` are made of.
Result<GeometryId> makeGeometry(PhysicsWorld& physics, AssetManager& assets, const Uuid& meshId, ShapeType shape) {
    if (meshId.isNil()) {
        return makeError(ErrorCode::InvalidArgument, "a collider of the shape {} needs a mesh, and names none",
                         nameOf(shape));
    }
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const MeshData> mesh, assets.loadMesh(meshId));
    std::vector<Vec3> positions;
    positions.reserve(mesh->vertices.size());
    for (const MeshVertex& vertex : mesh->vertices) {
        positions.push_back(vertex.position);
    }
    // A mesh that joints move collides as it is at rest.
    return shape == ShapeType::ConvexHull ? physics.createConvexHull(positions)
                                          : physics.createTriangleMesh(positions, mesh->indices);
}

/// The shape a collider asks for: as large as its entity is scaled, and placed in the
/// body it is part of. `entityToBody` takes a point of the collider's entity to the
/// space of the body, which is not scaled. `geometry` is what a collider that is made
/// of a mesh is made of.
ShapeDesc shapeOf(const ColliderComponent& collider, const Mat4& entityToBody, GeometryId geometry) {
    const Transform placed = Transform::fromMatrix(entityToBody);
    const Vec3 scale = glm::abs(placed.scale);

    ShapeDesc shape;
    shape.type = collider.shape;
    if (isMadeOfMesh(collider.shape)) {
        // As large as its entity along each axis, and mirrored where the entity is:
        // the scale found in the matrix has its sign, to go with the rotation found.
        shape.geometry = geometry;
        shape.scale = placed.scale;
    }
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

size_t PhysicsSystem::BodyKeyHash::operator()(const BodyKey& key) const noexcept {
    return static_cast<size_t>(hashCombine(std::hash<Entity>{}(key.entity), key.trigger ? 1u : 0u));
}

size_t PhysicsSystem::ReportKeyHash::operator()(const ReportKey& key) const noexcept {
    return static_cast<size_t>(hashCombine(std::hash<Entity>{}(key.entity), static_cast<uint64_t>(key.about)));
}

size_t PhysicsSystem::GeometryKeyHash::operator()(const GeometryKey& key) const noexcept {
    return static_cast<size_t>(hashCombine(std::hash<Uuid>{}(key.mesh), static_cast<uint64_t>(key.shape)));
}

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
    const auto found = m_records.find(BodyKey{.entity = entity, .trigger = false});
    return found != m_records.end() ? found->second.body : BodyId();
}

BodyId PhysicsSystem::triggerOf(Entity entity) const {
    const auto found = m_records.find(BodyKey{.entity = entity, .trigger = true});
    return found != m_records.end() ? found->second.body : BodyId();
}

Entity PhysicsSystem::entityOf(BodyId body) const {
    if (!m_physics->isValid(body)) {
        return Entity{};
    }
    const uint64_t userData = m_physics->userData(body);
    const Entity entity = entityOfUserData(userData);
    const auto found = m_records.find(BodyKey{.entity = entity, .trigger = (userData & kTriggerBit) != 0});
    return found != m_records.end() && found->second.body == body ? entity : Entity{};
}

GeometryId PhysicsSystem::geometryOf(const Uuid& mesh, ShapeType shape) const {
    const auto found = m_geometries.find(GeometryKey{.mesh = mesh, .shape = shape});
    return found != m_geometries.end() ? found->second.id : GeometryId();
}

const PhysicsSystem::MeshGeometry& PhysicsSystem::prepareGeometry(AssetManager& assets, const Uuid& mesh,
                                                                  ShapeType shape) {
    const auto [entry, isNew] = m_geometries.try_emplace(GeometryKey{.mesh = mesh, .shape = shape});
    if (isNew) {
        const Result<GeometryId> made = makeGeometry(*m_physics, assets, mesh, shape);
        if (made) {
            entry->second.id = *made;
        } else {
            entry->second.problem = made.error().message;
        }
    }
    return entry->second;
}

void PhysicsSystem::forgetGeometry() {
    for (const auto& [key, geometry] : m_geometries) {
        m_physics->destroyGeometry(geometry.id);
    }
    m_geometries.clear();
}

void PhysicsSystem::followAssets(const AssetManager& assets) {
    if (const uint64_t revision = assets.database().revision(); revision != m_assetRevision) {
        // What is prepared anew has other ids, and so the bodies made of it are made
        // anew as well.
        forgetGeometry();
        m_assetRevision = revision;
    }
}

void PhysicsSystem::clear() {
    for (const auto& [entity, record] : m_records) {
        m_physics->destroyBody(record.body);
    }
    forgetGeometry();
    m_records.clear();
    m_order.clear();
    m_events.clear();
    m_remade.clear();
    m_wantedCount = 0;
    m_reported.clear();
    m_timestep.reset();
}

void PhysicsSystem::gather(const World& world, AssetManager& assets) {
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

        // A body of the entity's own is where the entity is, and turned as it is. It
        // has no scale: the entity is as large in it as everything above has made it.
        const Vec3 ownScale = foldedScale(level.scale);
        const Mat4 ownToBody = glm::scale(Mat4(1.0f), ownScale);
        const auto startBody = [&](BodyMotion motion, bool trigger) {
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
                            .trigger = trigger,
                            .blueprint = BodyDesc{.motion = motion,
                                                  .shapes = std::move(shapes),
                                                  .sensor = trigger,
                                                  .userData = userDataOf(entity, trigger)},
                            .position = placed.position,
                            .rotation = placed.rotation,
                            .scale = ownScale};
            return static_cast<int32_t>(m_wantedCount++);
        };

        if (const auto* rigidBody = world.tryGet<RigidBodyComponent>(entity)) {
            level.body = startBody(rigidBody->motion, false);
            level.toBody = ownToBody;
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
        // that one. A trigger is a body of its own wherever it is: one that follows its
        // entity, moved by hand, so that it notices what sleeps in it too.
        const bool alone = collider->trigger || level.body == kNoBody;
        const int32_t index =
            !alone ? level.body
                   : startBody(collider->trigger ? BodyMotion::Kinematic : BodyMotion::Static, collider->trigger);
        GeometryId geometry;
        if (isMadeOfMesh(collider->shape)) {
            const MeshGeometry& made = prepareGeometry(assets, collider->mesh, collider->shape);
            if (made.id.isNull()) {
                // A collider that is no shape is left out of its body. A body that is
                // left without any is none.
                report(world, entity, About::Collider,
                       fmt::format("has a collider that is no shape: {}", made.problem));
                return;
            }
            geometry = made.id;
        }
        BodyDesc& body = m_wanted[static_cast<size_t>(index)].blueprint;
        if (body.shapes.empty()) {
            // A body has one surface: that of its first collider, which is its own
            // entity's where that has one.
            body.friction = collider->friction;
            body.restitution = collider->restitution;
        }
        body.shapes.push_back(shapeOf(*collider, alone ? ownToBody : level.toBody, geometry));
    });
}

PhysicsSystem::Record* PhysicsSystem::makeBody(const World& world, const Wanted& wanted, const Record* previous) {
    const BodyKey key{.entity = wanted.entity, .trigger = wanted.trigger};
    // The speeds of an entity's rigid body are those of the body that collides, not
    // of the trigger that its collider may be.
    const auto* rigidBody = wanted.trigger ? nullptr : world.tryGet<RigidBodyComponent>(wanted.entity);
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
        m_records.erase(key);
        m_remade.insert(wanted.entity);
    }

    BodyDesc desc = wanted.blueprint;
    desc.position = wanted.position;
    desc.rotation = wanted.rotation;
    const Result<BodyId> body = m_physics->createBody(desc);
    if (!body) {
        report(world, wanted.entity, About::Body, fmt::format("has no body: {}", body.error().message));
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
    return &m_records.insert_or_assign(key, std::move(record)).first->second;
}

void PhysicsSystem::placeBody(const World& world, const Wanted& wanted, Record& record, uint32_t steps,
                              Authority authority) {
    record.scale = wanted.scale;
    if (!isUsable(wanted.position) || !isUsable(wanted.rotation)) {
        report(world, wanted.entity, About::Body,
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
    for (auto& [key, record] : m_records) {
        record.seen = false;
    }
    m_order.clear();

    for (const Wanted& wanted : std::span(m_wanted).first(m_wantedCount)) {
        if (wanted.blueprint.shapes.empty()) {
            // A rigid body without a collider has no shape, and is no body.
            continue;
        }
        const BodyKey key{.entity = wanted.entity, .trigger = wanted.trigger};
        const auto found = m_records.find(key);
        Record* record = found != m_records.end() ? &found->second : nullptr;
        if (record == nullptr || !(record->blueprint == wanted.blueprint)) {
            record = makeBody(world, wanted, record);
        } else {
            placeBody(world, wanted, *record, steps, authority);
            if (!wanted.trigger) {
                applyVelocities(world, wanted.entity, *record);
            }
        }
        if (record != nullptr) {
            record->seen = true;
            m_order.push_back(key);
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
    for (const BodyKey& key : m_order) {
        const Entity entity = key.entity;
        Record& record = m_records.at(key);
        // A trigger follows its entity and has nothing to say to it.
        auto* rigidBody = key.trigger ? nullptr : world.tryGet<RigidBodyComponent>(entity);
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

void PhysicsSystem::report(const World& world, Entity entity, About about, std::string_view problem) {
    Report& said = m_reported[ReportKey{.entity = entity, .about = about}];
    said.current = true;
    if (said.problem != problem) {
        said.problem = problem;
        CORDITE_LOG_ERROR("Physics", "'{}' {}", world.name(entity), problem);
    }
}

PhysicsFrameStatistics PhysicsSystem::statistics(uint32_t steps) const {
    const PhysicsStatistics simulation = m_physics->statistics();
    return {.steps = steps,
            .bodies = simulation.bodies,
            .activeBodies = simulation.activeBodies,
            .geometries = simulation.geometries};
}

void PhysicsSystem::hearContacts() {
    for (const ContactEvent& contact : m_physics->contactEvents()) {
        const bool began = contact.phase == ContactPhase::Began;
        CollisionEvent event;
        if (contact.sensor) {
            event.type = began ? CollisionEventType::TriggerEntered : CollisionEventType::TriggerLeft;
        } else {
            event.type = began ? CollisionEventType::CollisionBegan : CollisionEventType::CollisionEnded;
        }
        // By what the bodies carry, which says whose they were when they are gone.
        event.first = entityOfUserData(contact.firstUserData);
        event.second = entityOfUserData(contact.secondUserData);
        event.position = contact.position;
        event.normal = contact.normal;
        event.speed = contact.speed;
        m_events.push_back(event);
    }
}

void PhysicsSystem::dropEchoesOfRemaking() {
    // A body that is made anew is another body to the simulation: the old one stops
    // touching what it touched, and the new one begins to. Nobody who listens for
    // things that meet and part wants to hear that.
    const auto wasRemade = [&](Entity entity) { return m_remade.contains(entity); };
    for (size_t index = 0; !m_remade.empty() && index < m_events.size();) {
        const CollisionEvent ended = m_events[index];
        if (!isEnd(ended.type) || !(wasRemade(ended.first) || wasRemade(ended.second))) {
            ++index;
            continue;
        }
        const auto beginning =
            std::ranges::find_if(m_events, [&](const CollisionEvent& event) { return isBeginningOf(event, ended); });
        if (beginning == m_events.end()) {
            // It touches no more what it touched: that is to be heard.
            ++index;
            continue;
        }
        // Both go. What follows the two moves up, to this place or the one before it.
        const auto other = static_cast<size_t>(beginning - m_events.begin());
        m_events.erase(m_events.begin() + static_cast<std::ptrdiff_t>(std::max(index, other)));
        m_events.erase(m_events.begin() + static_cast<std::ptrdiff_t>(std::min(index, other)));
        if (other < index) {
            --index;
        }
    }
}

PhysicsFrameStatistics PhysicsSystem::update(World& world, AssetManager& assets, float deltaSeconds) {
    m_events.clear();
    world.updateWorldTransforms();
    followAssets(assets);
    gather(world, assets);
    const uint32_t steps = m_timestep.advance(static_cast<double>(deltaSeconds));
    reconcile(world, steps, Authority::Simulation);
    for (uint32_t step = 0; step < steps; ++step) {
        m_physics->step(static_cast<float>(m_timestep.stepSeconds()));
        hearContacts();
    }
    if (steps > 0) {
        // What was made anew in a frame without a step is heard of in the next that
        // has one, and remembered until then.
        dropEchoesOfRemaking();
        m_remade.clear();
    }
    // Also after no step at all: an entity that something above it has carried off is
    // put back where its body is.
    writeBack(world);
    return statistics(steps);
}

PhysicsFrameStatistics PhysicsSystem::synchronize(World& world, AssetManager& assets) {
    m_events.clear();
    world.updateWorldTransforms();
    followAssets(assets);
    gather(world, assets);
    reconcile(world, 0, Authority::World);
    // No step follows, here or perhaps ever: an editor only synchronizes. What is
    // remembered for the next step is kept to the entities there still are.
    std::erase_if(m_remade, [&](Entity entity) { return !world.isValid(entity); });
    return statistics(0);
}

} // namespace cordite
