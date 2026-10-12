#pragma once

/// @file
/// The editor commands that ask the physics what is where.
///
/// | Command           | Does                                                              |
/// |-------------------|-------------------------------------------------------------------|
/// | `physics.raycast` | Says what a ray, or a ball that is moved along it, hits first.    |
/// | `physics.overlap` | Lists the entities whose bodies reach into a ball or a box.       |
///
/// @code
/// CORDITE_TRY(registerPhysicsCommands(commands, context, engine.physics()));
///
/// const Json hit = unwrap(commands.execute(
///     "physics.raycast", {{"origin", {0.0, 10.0, 0.0}}, {"direction", {0.0, -1.0, 0.0}}}));
/// if (hit["hit"] == true) {
///     // hit["entity"] is the path of the entity whose body was hit, hit["distance"] how far away
/// }
/// @endcode
///
/// What is asked about is the scene as it is at that moment: each command brings the
/// bodies in line with the entities first, and lets no time pass. An answer names the
/// entity that has the body - the one with the RigidBody, where a collider is part of
/// the body of an entity above it.

#include "cordite/core/Result.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/physics/PhysicsSystem.h"

namespace cordite {

/// Registers the commands above. The registry must not be used after the context or the
/// physics system is gone: the handlers refer to both.
[[nodiscard]] Status registerPhysicsCommands(CommandRegistry& commands, EditorContext& context, PhysicsSystem& physics);

} // namespace cordite
