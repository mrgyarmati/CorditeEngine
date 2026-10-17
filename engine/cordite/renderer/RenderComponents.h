#pragma once

/// @file
/// The components that make an entity visible, the ones that light it, the one that
/// surrounds a scene with a sky, the one that fills its air with fog, the one that lays
/// a material onto what is drawn, the one that draws what a camera sees into a render
/// target, and the one that says how the picture is finished.
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
/// "Fog": {"color": [0.5, 0.6, 0.7], "density": 0.02, "heightFalloff": 0.2,
///         "sunColor": [1, 0.7, 0.4], "sunExponent": 8},
/// "Decal": {"material": "e967ddcf-f70d-8878-bac3-7975b0d5351c", "opacity": 1, "fadeAngle": 60,
///           "order": 0},
/// "SceneCapture": {"target": "7c9a0e44-1f2b-4c3d-9e8f-0a1b2c3d4e5f", "interval": 0},
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
#include "cordite/world/World.h"

#include <cstdint>
#include <string>
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

/// A camera of the scene that draws into a render target (RenderTarget.h) while the scene
/// is shown, for materials to show what it sees: the screen of a security monitor, a
/// mirror, the preview of a character. The Camera component of the same entity says how
/// it sees - with the width and height of the target - and is never the game's camera:
/// `primary` must be off. Its pictures are finished like a game's, before the views of
/// the frame that show them; one that sees its own picture sees the frame before.
struct SceneCaptureComponent {
    /// The render target it draws into. Nil draws nothing.
    Uuid target;
    /// How long a picture is kept before the next is drawn, in seconds: 0 draws one every
    /// frame, more saves what drawing costs where the picture may lag.
    float interval = 0.0f;
};

/// A decal: a material laid onto whatever solid surfaces are inside the box of the entity
/// - a cube of one metre about its origin, scaled, turned and placed by its transform -
/// projected along the entity's -Z axis, as a light shines and a camera looks. Its image
/// runs from the top-left corner of the box's +Z face: across along +X, down along -Y.
/// Decals.h says how it lands; DecalPass draws it into the G-buffer.
struct DecalComponent {
    /// The material laid on: its base color, normal map, roughness, metallic, specular and
    /// the light it gives off. How much of it there is goes by its alpha mode, as for a
    /// mesh: all of an opaque one, all or nothing of a masked one by its cutoff, and of a
    /// blended one what its alpha says. Nil lays nothing on.
    Uuid material;
    /// How much of it is laid on: 0 nothing, 1 as its material says.
    float opacity = 1.0f;
    /// Surfaces that turn away from the projection by more than this many degrees get none
    /// of it, and it fades over the ten degrees before. From 10 to 90.
    float fadeAngle = 60.0f;
    /// Of decals that overlap, one of a higher order is laid on after - over - one of a
    /// lower; of the same order, by the ids of their entities.
    int32_t order = 0;
};

/// Air that is not clear: haze, mist, smoke that hangs low. The further away something
/// is, the less of its light arrives, and the more of the fog's own color is seen in
/// its place - of the sky too, which is furthest of all. Surfaces are in the fog
/// whatever draws them, solid or translucent.
///
/// The fog is at its densest at the height of the entity and everywhere below, and
/// thins out above: raise or lower the entity to raise or lower the fog. Nothing else
/// of where the entity is matters. A world has one fog: of several that are enabled,
/// that of the entity with the lowest id. Fog.h says how it is worked out.
struct FogComponent {
    /// Linear color of the fog itself: what is seen where nothing shows through it.
    /// It is not lit by anything: a fog in a dark scene is given a dark color.
    Vec3 color{0.5f, 0.6f, 0.7f};
    /// The part of the light that is lost with every metre where the fog is densest.
    /// 0.02 leaves 82% of the light of what is ten metres away, and 14% of what is a
    /// hundred; 0 is no fog.
    float density = 0.02f;
    /// How fast the fog thins out above the entity: to a little more than a third
    /// (1 / e) every 1 / heightFalloff metres. 0 is the same fog at every height, which
    /// hides the sky altogether unless `maxOpacity` keeps some of it.
    float heightFalloff = 0.2f;
    /// The fog begins this far from the viewer, in metres.
    float startDistance = 0.0f;
    /// The most of the fog's color that ever takes the place of what is behind it: 1
    /// lets distant things vanish, less keeps them, and the sky, faintly there.
    float maxOpacity = 1.0f;
    /// Linear color the fog adds to its own where the view looks straight at the sun:
    /// the glow of a sun in haze. The sun is the brightest directional light that is
    /// on; without one, and with black, there is no glow.
    Vec3 sunColor{0.0f};
    /// How closely around the sun the glow stays: it falls off with the cosine of the
    /// angle to the sun, to this power. 1 lights half the sky, 64 a small patch.
    float sunExponent = 8.0f;
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

/// Something wrong with the render components of an entity, found without drawing.
struct RenderProblem {
    Entity entity;
    /// One sentence, naming the component and what is wrong.
    std::string message;
};

/// What can be told of the render components of `world` without drawing anything, in
/// the order of the hierarchy: a SceneCapture whose entity has no Camera draws nothing,
/// and one whose Camera is primary is the view of the game as well (findPrimaryCamera).
[[nodiscard]] std::vector<RenderProblem> findRenderProblems(const World& world);

/// Registers the renderer's components: "MeshRenderer", "Light", "AmbientLight",
/// "Environment", "Fog", "Decal", "SceneCapture" and "PostProcess".
[[nodiscard]] Status registerRendererComponents(ComponentRegistry& registry);

} // namespace cordite
