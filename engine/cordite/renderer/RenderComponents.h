#pragma once

/// @file
/// The components that make an entity visible, the ones that light it, the one that
/// surrounds a scene with a sky, and the one that says how the picture is finished.
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
///           "innerConeDegrees": 20, "outerConeDegrees": 30, "castsShadows": true},
/// "Environment": {"texture": "3f0c8d5e-6f0a-4f38-9d3b-1b2a7c0de481", "intensity": 1.5,
///                 "rotationDegrees": 90, "visible": true},
/// "PostProcess": {"global": true, "toneMapper": "filmic", "exposure": 0.5, "saturation": 0.85,
///                 "vignette": 0.3}
/// @endcode
///
/// Lighting channels: every mesh renderer and every light belongs to some of eight
/// channels, given as a number with one bit per channel (1 is the first channel, 3 the
/// first two, 255 all). A light shines on a surface when they share a channel. Left
/// alone, everything is in the first channel and every light shines on everything.

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/PostProcessSettings.h"
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
    /// Whether the mesh throws shadows, in the light of the lights that cast them and
    /// share a lighting channel with it. Translucent surfaces never do.
    bool castsShadows = true;
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

/// How large the shadow map of a light is, compared with what the shadow quality of a
/// view gives a light: the choice for a single light that matters more, or less, than
/// the others.
enum class ShadowResolution : uint8_t {
    /// Half as many texels a side: a quarter of the memory.
    Low,
    Medium,
    /// Twice as many texels a side.
    High,
};

std::string_view nameOf(ShadowResolution resolution);

/// A light. It shines from where the entity is; a directional or a spot light shines
/// along the entity's -Z axis.
struct LightComponent {
    LightType type = LightType::Point;
    /// Linear color.
    Vec3 color{1.0f};
    /// Brightness. 1 lights a white, matte surface that faces the light to white: at
    /// any distance for a directional light, and one metre away for a point or a spot
    /// light, whose light thins out with the square of the distance. (White is a
    /// brightness of one in the scene. What a screen shows of it is up to the exposure
    /// and the tone mapper of the view: see PostProcessComponent.)
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

    /// Whether what stands in the light throws shadows. Shadows cost: a light that
    /// casts them draws what it reaches once more - a point light six times, the sun
    /// once for each cascade. Of the directional lights that are to cast shadows,
    /// only the brightest does: a scene has one sun. A spot light throws shadows up
    /// to 85 degrees from its axis; a wider one is better a point light.
    bool castsShadows = false;
    /// A shadow map is made of texels, and a lit surface looked up in it exactly where
    /// it is would shadow itself in stripes. It is looked up a little off instead: this
    /// many texels nearer the light, ...
    float shadowBias = 1.0f;
    /// ... and this many texels along the surface's normal, the more the flatter the
    /// light strikes it. Too little shows as stripes on lit surfaces, too much as
    /// shadows that begin a gap away from what casts them, and end short.
    float shadowNormalOffset = 1.0f;
    /// The size of the light's shadow maps against what lights usually get at the
    /// shadow quality of the view. It is what the light asks for: all shadow maps of a
    /// view share one atlas, and a light that does not find the room gets smaller ones.
    ShadowResolution shadowResolution = ShadowResolution::Medium;
    /// Directional: how far from the camera the shadows reach, in metres; towards that
    /// distance they fade out. The shorter, the sharper: the same texels cover less.
    float shadowDistance = 60.0f;
    /// Directional: into how many shadow maps (cascades) that distance is divided, 1
    /// to 4 - small, sharp ones near the camera and large, coarse ones far away.
    uint32_t shadowCascades = 4;
    /// Directional: how the cascades share the distance. 0 makes them equally deep; 1
    /// makes each a constant factor deeper than the one before, which spends most of
    /// the texels close to the camera.
    float shadowCascadeDistribution = 0.75f;
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

/// Everything far away, as an image that lights the scene: a sky with its sun, a
/// horizon, a ground. It reaches every surface, whatever its lighting channels - matte
/// ones with the light of all it shows, shiny ones with its reflection - and it is what
/// is seen where nothing else is. Nothing in the scene blocks it: it lights the inside
/// of a room as it lights a field.
///
/// A world is lit by one environment: of several that are enabled, by the one whose
/// entity has the lowest id. Where the entity is does not matter. AmbientLights add to it.
struct EnvironmentComponent {
    /// The image asset: all there is around, twice as wide as high, the way HDR images
    /// of skies come (.hdr; an image of 8-bit colors does too, as sRGB). Its top row is
    /// straight up, its middle what is seen looking along -Z, and +X a quarter of its
    /// width to the right of that. Nil is no environment.
    Uuid texture;
    /// Scales its light. With 1, a white, matte surface under an image of even
    /// brightness is as bright as the image.
    float intensity = 1.0f;
    /// Turns the image about the vertical axis, counter-clockwise seen from above, in
    /// degrees: 90 brings what was straight ahead (-Z) to the left (-X).
    float rotationDegrees = 0.0f;
    /// Whether the image is drawn where nothing else is, as the sky. Without, it lights
    /// the scene and shows in reflections, and the background stays what the view is
    /// cleared to.
    bool visible = true;
};

/// How the picture is finished - exposure, bloom, tone mapping, grading and the rest of
/// PostProcessSettings - and where.
///
/// Where: everywhere (`global`), or where the camera is inside the box that the
/// entity's transform makes of a cube of one metre, fading in over `blendDistance`
/// outside it. Several are laid over one another from the lowest priority to the
/// highest (ScenePostProcess.h). On an entity that is a camera the component is that
/// camera's own: it applies to what is seen through it and to nothing else, whatever
/// `global` says, and after all the others.
struct PostProcessComponent : PostProcessSettings {
    /// Applies wherever the camera is. Without it, the entity's box says where.
    bool global = true;
    /// Those with a lower priority are applied first, and so are overridden by the
    /// ones with a higher.
    int32_t priority = 0;
    /// How far outside the box the settings fade in, in metres. Nothing makes the box
    /// end sharply.
    float blendDistance = 1.0f;
    /// How much of the settings is applied where they apply in full: 0 none, 1 all.
    float weight = 1.0f;
};

/// Registers the renderer's components: "MeshRenderer", "Light", "AmbientLight",
/// "Environment" and "PostProcess".
[[nodiscard]] Status registerRendererComponents(ComponentRegistry& registry);

} // namespace cordite
