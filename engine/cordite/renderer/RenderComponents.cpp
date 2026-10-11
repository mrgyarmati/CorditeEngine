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
