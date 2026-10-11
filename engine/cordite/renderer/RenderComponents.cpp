#include "cordite/renderer/RenderComponents.h"

#include "cordite/core/Reflection.h"
#include "cordite/math/MathReflection.h"

#include <string>
#include <utility>

namespace cordite {
namespace {

PropertyAttributes channelsAttribute(std::string description) {
    return {.description = std::move(description),
            .semantic = PropertySemantic::BitMask,
            .minimum = 0.0,
            .maximum = static_cast<double>(kAllLightChannels)};
}

TypeInfo describeMeshRenderer() {
    return TypeBuilder<MeshRendererComponent>("MeshRenderer")
        .property(
            "mesh", &MeshRendererComponent::mesh,
            {.description = "The mesh to draw", .semantic = PropertySemantic::AssetReference, .referenceType = "Mesh"})
        .property("materials", &MeshRendererComponent::materials,
                  {.description = "One material per submesh of the mesh; missing or empty entries use the default "
                                  "material",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Material"})
        .property("lightChannels", &MeshRendererComponent::lightChannels,
                  channelsAttribute("The lighting channels the mesh belongs to, one bit each (1 is the first, 255 "
                                    "all eight); a light shines on it when they share a channel"))
        .property("castsShadows", &MeshRendererComponent::castsShadows,
                  {.description = "Whether the mesh throws shadows; translucent surfaces never do"})
        .build();
}

TypeInfo describeLight() {
    return TypeBuilder<LightComponent>("Light")
        .enumProperty("type", &LightComponent::type,
                      {{LightType::Directional, std::string(nameOf(LightType::Directional))},
                       {LightType::Point, std::string(nameOf(LightType::Point))},
                       {LightType::Spot, std::string(nameOf(LightType::Spot))}},
                      {.description = "Directional: from one direction, everywhere. Point: from a point, all around. "
                                      "Spot: from a point, within a cone. Directional and spot lights shine along -Z"})
        .property("color", &LightComponent::color,
                  {.description = "Linear color of the light", .semantic = PropertySemantic::Color, .minimum = 0.0})
        .property("intensity", &LightComponent::intensity,
                  {.description = "Brightness; 1 lights a white matte surface facing the light to white (for a point "
                                  "or spot light: one metre away)",
                   .minimum = 0.0})
        .property("range", &LightComponent::range,
                  {.description = "Point and spot: the distance at which the light has faded to nothing",
                   .minimum = 0.01,
                   .unit = "m"})
        .property("innerConeDegrees", &LightComponent::innerConeDegrees,
                  {.description = "Spot: the angle from the axis up to which the light is at full brightness",
                   .minimum = 0.0,
                   .maximum = 89.0,
                   .unit = "deg"})
        .property("outerConeDegrees", &LightComponent::outerConeDegrees,
                  {.description = "Spot: the angle from the axis at which the light ends",
                   .minimum = 0.0,
                   .maximum = 89.0,
                   .unit = "deg"})
        .property("channels", &LightComponent::channels,
                  channelsAttribute("The lighting channels the light shines on, one bit each (1 is the first, 255 "
                                    "all eight)"))
        .property("castsShadows", &LightComponent::castsShadows,
                  {.description = "Whether what stands in the light throws shadows. Of several directional "
                                  "lights only the brightest casts them"})
        .property("shadowBias", &LightComponent::shadowBias,
                  {.description = "How many shadow-map texels nearer the light a surface is looked up; more "
                                  "removes stripes on lit surfaces, less keeps shadows attached to what casts "
                                  "them",
                   .minimum = 0.0,
                   .maximum = 16.0,
                   .step = 0.1})
        .property("shadowNormalOffset", &LightComponent::shadowNormalOffset,
                  {.description = "How many shadow-map texels along its normal a surface is looked up where the "
                                  "light grazes it",
                   .minimum = 0.0,
                   .maximum = 16.0,
                   .step = 0.1})
        .enumProperty("shadowResolution", &LightComponent::shadowResolution,
                      {{ShadowResolution::Low, std::string(nameOf(ShadowResolution::Low))},
                       {ShadowResolution::Medium, std::string(nameOf(ShadowResolution::Medium))},
                       {ShadowResolution::High, std::string(nameOf(ShadowResolution::High))}},
                      {.description = "The size of the light's shadow maps: half, the same or twice what the "
                                      "shadow quality gives a light, as far as there is room for them"})
        .property("shadowDistance", &LightComponent::shadowDistance,
                  {.description = "Directional: how far from the camera the shadows reach; they fade out "
                                  "towards it",
                   .minimum = 1.0,
                   .maximum = 2000.0,
                   .unit = "m"})
        .property("shadowCascades", &LightComponent::shadowCascades,
                  {.description = "Directional: into how many shadow maps the shadow distance is divided",
                   .minimum = 1.0,
                   .maximum = 4.0})
        .property("shadowCascadeDistribution", &LightComponent::shadowCascadeDistribution,
                  {.description = "Directional: 0 makes the cascades equally deep, 1 makes each a constant factor "
                                  "deeper than the one before, which keeps the shadows near the camera sharpest",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .build();
}

TypeInfo describeAmbientLight() {
    return TypeBuilder<AmbientLightComponent>("AmbientLight")
        .property("skyColor", &AmbientLightComponent::skyColor,
                  {.description = "Linear color of the light from above",
                   .semantic = PropertySemantic::Color,
                   .minimum = 0.0})
        .property("groundColor", &AmbientLightComponent::groundColor,
                  {.description = "Linear color of the light from below",
                   .semantic = PropertySemantic::Color,
                   .minimum = 0.0})
        .property("intensity", &AmbientLightComponent::intensity, {.description = "Scales both colors", .minimum = 0.0})
        .build();
}

TypeInfo describePostProcess() {
    using Component = PostProcessComponent;
    return TypeBuilder<Component>("PostProcess")
        .property("global", &Component::global,
                  {.description = "Applies wherever the camera is; without it, only in and around the box the "
                                  "entity's transform makes of a cube of one metre. On a camera: applies to that "
                                  "camera's views alone, whatever this says"})
        .property("priority", &Component::priority,
                  {.description = "Several are applied from the lowest priority to the highest, each over what "
                                  "the ones before left",
                   .minimum = -1000.0,
                   .maximum = 1000.0})
        .property("blendDistance", &Component::blendDistance,
                  {.description = "How far outside its box the settings fade in",
                   .minimum = 0.0,
                   .maximum = 1000.0,
                   .unit = "m"})
        .property("weight", &Component::weight,
                  {.description = "How much of the settings is applied: 0 none, 1 all",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .enumProperty("exposureMode", &Component::exposureMode,
                      {{ExposureMode::Manual, std::string(nameOf(ExposureMode::Manual))},
                       {ExposureMode::Automatic, std::string(nameOf(ExposureMode::Automatic))}},
                      {.description = "Manual: the exposure is what `exposure` says. Automatic: the view sets "
                                      "itself so that the picture is mid-grey on average, and `exposure` is added"})
        .property("exposure", &Component::exposure,
                  {.description = "Exposure in stops: each one doubles the light, each negative one halves it",
                   .minimum = -16.0,
                   .maximum = 16.0,
                   .step = 0.05})
        .property("autoExposureMinimum", &Component::autoExposureMinimum,
                  {.description = "Automatic exposure: the fewest stops the view may choose",
                   .minimum = -16.0,
                   .maximum = 16.0,
                   .step = 0.05})
        .property("autoExposureMaximum", &Component::autoExposureMaximum,
                  {.description = "Automatic exposure: the most stops the view may choose",
                   .minimum = -16.0,
                   .maximum = 16.0,
                   .step = 0.05})
        .property("adaptationSpeedUp", &Component::adaptationSpeedUp,
                  {.description = "Automatic exposure: how fast the view gets used to a brighter picture; what is "
                                  "left of the way shrinks to a third in one second divided by this",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.05})
        .property("adaptationSpeedDown", &Component::adaptationSpeedDown,
                  {.description = "Automatic exposure: how fast the view gets used to a darker picture",
                   .minimum = 0.0,
                   .maximum = 100.0,
                   .step = 0.05})
        .enumProperty("toneMapper", &Component::toneMapper,
                      {{ToneMapper::Filmic, std::string(nameOf(ToneMapper::Filmic))},
                       {ToneMapper::Aces, std::string(nameOf(ToneMapper::Aces))},
                       {ToneMapper::Linear, std::string(nameOf(ToneMapper::Linear))}},
                      {.description = "How light without an upper limit becomes the colors of a display. Filmic: "
                                      "soft contrast, slow highlights. Aces: more contrast and color. Linear: no "
                                      "curve; what is brighter than white is cut off"})
        .property("bloomIntensity", &Component::bloomIntensity,
                  {.description = "How much of the bloom is added to the picture; 0 for none",
                   .minimum = 0.0,
                   .maximum = 4.0,
                   .step = 0.01})
        .property("bloomThreshold", &Component::bloomThreshold,
                  {.description = "Light brighter than this blooms; 1 is white, 0 lets everything bloom a little",
                   .minimum = 0.0,
                   .maximum = 64.0,
                   .step = 0.05})
        .property("bloomScatter", &Component::bloomScatter,
                  {.description = "How far the bloom spreads: 0 tight around what is bright, 1 over much of the "
                                  "picture",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("temperature", &Component::temperature,
                  {.description = "White balance: positive is warmer (more red, less blue), negative cooler",
                   .minimum = -1.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("tint", &Component::tint,
                  {.description = "White balance: positive towards magenta, negative towards green",
                   .minimum = -1.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("contrast", &Component::contrast,
                  {.description = "1 leaves the picture as it is; less is flatter, more is harder; mid-grey stays",
                   .minimum = 0.5,
                   .maximum = 2.0,
                   .step = 0.01})
        .property("saturation", &Component::saturation,
                  {.description = "1 leaves the picture as it is; 0 is black and white",
                   .minimum = 0.0,
                   .maximum = 2.0,
                   .step = 0.01})
        .property("lift", &Component::lift,
                  {.description = "Raises the shadows, per color; 0 leaves them",
                   .minimum = -1.0,
                   .maximum = 1.0,
                   .step = 0.005})
        .property("gamma", &Component::gamma,
                  {.description = "Bends the mid-tones, per color; 1 leaves them, more is brighter",
                   .minimum = 0.2,
                   .maximum = 5.0,
                   .step = 0.01})
        .property("gain", &Component::gain,
                  {.description = "Scales the highlights, per color; 1 leaves them",
                   .minimum = 0.0,
                   .maximum = 4.0,
                   .step = 0.01})
        .property("colorLut", &Component::colorLut,
                  {.description = "A lookup table for the colors of the finished picture: an image of N * N by N "
                                  "pixels, such as 256 by 16",
                   .semantic = PropertySemantic::AssetReference,
                   .referenceType = "Texture"})
        .property("colorLutIntensity", &Component::colorLutIntensity,
                  {.description = "How much of the lookup table's colors is taken: 0 none, 1 all",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("vignette", &Component::vignette,
                  {.description = "How dark the corners of the picture get: 0 not at all, 1 black",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("chromaticAberration", &Component::chromaticAberration,
                  {.description = "How far red and blue part in the corners, in hundredths of the picture's size",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .property("filmGrain", &Component::filmGrain,
                  {.description = "How much film grain is laid over the picture: 0 none, 1 a lot",
                   .minimum = 0.0,
                   .maximum = 1.0,
                   .step = 0.01})
        .build();
}

} // namespace

std::string_view nameOf(LightType type) {
    switch (type) {
        case LightType::Directional:
            return "directional";
        case LightType::Point:
            return "point";
        case LightType::Spot:
            return "spot";
    }
    return "unknown";
}

std::string_view nameOf(ShadowResolution resolution) {
    switch (resolution) {
        case ShadowResolution::Low:
            return "low";
        case ShadowResolution::Medium:
            return "medium";
        case ShadowResolution::High:
            return "high";
    }
    return "unknown";
}

Status registerRendererComponents(ComponentRegistry& registry) {
    CORDITE_TRY(registry.add<MeshRendererComponent>(
        describeMeshRenderer(), {.description = "Draws a mesh at the entity's place in the world."}));
    CORDITE_TRY(registry.add<LightComponent>(
        describeLight(), {.description = "A light at the entity's place: directional, point or spot."}));
    CORDITE_TRY(registry.add<AmbientLightComponent>(
        describeAmbientLight(),
        {.description = "Light from all around, from a sky above and a ground below. Several add up."}));
    CORDITE_TRY(registry.add<PostProcessComponent>(
        describePostProcess(),
        {.description = "How the picture is finished - exposure, bloom, tone mapping, grading - everywhere, in a "
                        "box, or for the camera it is on."}));
    return {};
}

} // namespace cordite
