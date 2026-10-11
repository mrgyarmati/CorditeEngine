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
    return {};
}

} // namespace cordite
