#pragma once

/// @file
/// The fixture shared by the tests of the physics system: a world, and the physics that
/// goes with it.
///
/// @code
/// PhysicsStage stage;
/// stage.addFloor();
/// const Entity crate = stage.addCrate(Vec3(0.0f, 4.0f, 0.0f));
/// stage.run(120);                                      // two seconds, a sixtieth at a time
/// CHECK(stage.positionOf(crate).y == doctest::Approx(0.5f).epsilon(0.01));
/// @endcode

#include "cordite/assets/AssetManager.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <memory>
#include <optional>
#include <string>

namespace cordite::test {

/// The frame every test of the physics system lets pass: a sixtieth of a second, which
/// is one step.
inline constexpr float kPhysicsStep = 1.0f / 60.0f;

/// A world, and the physics that goes with it.
struct PhysicsStage {
    PhysicsStage() : assets(unwrap(AssetManager::create())), physics(unwrap(PhysicsSystem::create())) {}

    /// A floor whose top is the plane y = 0: a collider alone, which stands still.
    Entity addFloor() {
        const Entity floor = world.createEntity("Floor");
        world.add<ColliderComponent>(floor) = {.size = Vec3(50.0f, 1.0f, 50.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
        return floor;
    }

    /// A box of one metre that the simulation moves.
    Entity addCrate(const Vec3& position, const char* name = "Crate") {
        const Entity crate = world.createEntity(name);
        world.transform(crate).position = position;
        world.add<ColliderComponent>(crate);
        world.add<RigidBodyComponent>(crate);
        return crate;
    }

    /// Lets frames of a sixtieth of a second pass.
    PhysicsFrameStatistics run(int frames) {
        PhysicsFrameStatistics last;
        for (int frame = 0; frame < frames; ++frame) {
            last = physics->update(world, *assets, kPhysicsStep);
        }
        return last;
    }

    /// Where an entity is in the world, worked out from the transforms as they are now.
    [[nodiscard]] Vec3 positionOf(Entity entity) const { return Vec3(world.computeWorldMatrix(entity)[3]); }

    /// What a ray straight down from high above (x, z) hits, as the name of its entity.
    [[nodiscard]] std::string under(float x, float z) const {
        const std::optional<RayHit> hit =
            physics->simulation().raycast(Ray{Vec3(x, 100.0f, z), Vec3(0.0f, -1.0f, 0.0f)}, 1000.0f);
        if (!hit) {
            return "(nothing)";
        }
        const Entity entity = physics->entityOf(hit->body);
        return entity.isNull() ? "(no entity)" : world.name(entity);
    }

    /// How high above the ground a ray straight down from high above (x, z) hits.
    [[nodiscard]] float heightAt(float x, float z) const {
        const std::optional<RayHit> hit =
            physics->simulation().raycast(Ray{Vec3(x, 100.0f, z), Vec3(0.0f, -1.0f, 0.0f)}, 1000.0f);
        return hit ? hit->position.y : -1000.0f;
    }

    World world;
    /// The meshes the engine brings along, and no others.
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<PhysicsSystem> physics;
};

} // namespace cordite::test
