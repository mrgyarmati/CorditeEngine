#pragma once

/// @file
/// The components that make an entity visible, and the ones that light it.
///
/// @code
/// const Entity crate = world.createEntity("Crate");
/// MeshRendererComponent& renderer = world.add<MeshRendererComponent>(crate);
/// renderer.mesh = builtinMeshId(BuiltinMesh::Cube);
/// renderer.materials = {paintId};
///
/// const Entity lamp = world.createEntity("Lamp");
/// world.transform(lamp).position = Vec3(0.0f, 3.0f, 0.0f);
/// world.add<LightComponent>(lamp) = {.type = LightType::Point, .color = Vec3(1.0f, 0.8f, 0.6f),
///                                    .intensity = 5.0f, .range = 8.0f};
/// @endcode
///
/// In a scene file:
///
/// @code{.json}
/// "MeshRenderer": {
///     "mesh": "0a56c2f1-21e0-819d-8bac-567d1e1c4345",
///     "materials": ["5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79"]
/// },
/// "Light": {"type": "spot", "color": [1, 0.8, 0.6], "intensity": 5, "range": 8,
///           "innerConeDegrees": 20, "outerConeDegrees": 30}
/// @endcode
///
/// Lighting channels: every mesh renderer and every light belongs to some of eight
/// channels, given as a number with one bit per channel (1 is the first channel, 3 the
/// first two, 255 all). A light shines on a surface when they share a channel. Left
/// alone, everything is in the first channel and every light shines on everything.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/ComponentRegistry.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cordite {

/// All eight lighting channels.
inline constexpr uint32_t kAllLightChannels = 0xFF;

/// Draws a mesh at the entity's place in the world.
struct MeshRendererComponent {
    /// The mesh asset. Nil draws nothing.
    Uuid mesh;
    /// One material asset per submesh of the mesh, in order. A submesh without an entry,
    /// or with a nil one, is drawn with the default material.
    std::vector<Uuid> materials;
    /// The lighting channels the mesh belongs to, one bit each.
    uint32_t lightChannels = 1;
};

enum class LightType : uint8_t {
    /// Light from one direction, the same everywhere: the sun, the moon.
    Directional,
    /// Light from a point, in all directions.
    Point,
    /// Light from a point, within a cone.
    Spot,
};

std::string_view nameOf(LightType type);

/// A light. It shines from where the entity is; a directional or a spot light shines
/// along the entity's -Z axis.
struct LightComponent {
    LightType type = LightType::Point;
    /// Linear color.
    Vec3 color{1.0f};
    /// Brightness. 1 lights a white, matte surface that faces the light to white: at
    /// any distance for a directional light, and one metre away for a point or a spot
    /// light, whose light thins out with the square of the distance.
    float intensity = 1.0f;
    /// Point and spot: the distance at which the light has faded to nothing, in metres.
    /// A light costs what it can reach; keep the range as small as the look allows.
    float range = 10.0f;
    /// Spot: the angle from the axis up to which the light is at full brightness, and
    /// the angle at which it ends, in degrees. The outer one is never used smaller than
    /// the inner.
    float innerConeDegrees = 25.0f;
    float outerConeDegrees = 35.0f;
    /// The lighting channels the light shines on, one bit each.
    uint32_t channels = 1;
};

/// Light that arrives from all around: from a sky above and from a ground below. It
/// reaches every surface, whatever its lighting channels. Several add up; without
/// one, a scene has no light but that of its lights.
struct AmbientLightComponent {
    /// Linear color of the light from straight above.
    Vec3 skyColor{0.26f, 0.31f, 0.40f};
    /// Linear color of the light from straight below: what the ground throws back.
    Vec3 groundColor{0.10f, 0.09f, 0.08f};
    /// Scales both. With 1, a white, matte surface that faces up takes on the sky color.
    float intensity = 1.0f;
};

/// Registers the renderer's components: "MeshRenderer", "Light" and "AmbientLight".
[[nodiscard]] Status registerRendererComponents(ComponentRegistry& registry);

} // namespace cordite
