#pragma once

/// @file
/// The components that make an entity visible.
///
/// @code
/// const Entity crate = world.createEntity("Crate");
/// MeshRendererComponent& renderer = world.add<MeshRendererComponent>(crate);
/// renderer.mesh = builtinMeshId(BuiltinMesh::Cube);
/// renderer.materials = {paintId};
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "MeshRenderer": {
///     "mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
///     "materials": ["5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79"]
/// }
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/world/ComponentRegistry.h"

#include <vector>

namespace cordite {

/// Draws a mesh at the entity's place in the world.
struct MeshRendererComponent {
    /// The mesh asset. Nil draws nothing.
    Uuid mesh;
    /// One material asset per submesh of the mesh, in order. A submesh without an entry,
    /// or with a nil one, is drawn with the default material.
    std::vector<Uuid> materials;
};

/// Registers the renderer's components: "MeshRenderer".
[[nodiscard]] Status registerRendererComponents(ComponentRegistry& registry);

} // namespace cordite
