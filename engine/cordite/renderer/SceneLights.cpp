#include "cordite/renderer/SceneLights.h"

#include "cordite/world/Components.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

namespace cordite {
namespace {

/// The lighting of a world without lights: a slightly warm key light from above, over
/// the right shoulder of a viewer who looks along -Z, and soft light from a cool sky
/// and a dark ground.
constexpr Vec3 kDefaultKeyDirection(-0.45f, -0.80f, -0.40f);
constexpr Vec3 kDefaultKeyColor(2.75f, 2.60f, 2.40f);
constexpr Vec3 kDefaultSkyColor(0.26f, 0.31f, 0.40f);
constexpr Vec3 kDefaultGroundColor(0.10f, 0.09f, 0.08f);

GpuLightType gpuTypeOf(LightType type) {
    switch (type) {
        case LightType::Directional:
            return GpuLightType::Directional;
        case LightType::Point:
            return GpuLightType::Point;
        case LightType::Spot:
            return GpuLightType::Spot;
    }
    return GpuLightType::Point;
}

float resolutionScaleOf(ShadowResolution resolution) {
    switch (resolution) {
        case ShadowResolution::Low:
            return 0.5f;
        case ShadowResolution::Medium:
            return 1.0f;
        case ShadowResolution::High:
            return 2.0f;
    }
    return 1.0f;
}

/// The sun among `lights`: the brightest light that shines from one direction; of
/// several that are as bright, the first. (The shadows find theirs the same way, among
/// the lights that cast any: ShadowPlan.cpp.)
std::optional<SceneSun> findSun(std::span<const SceneLight> lights) {
    std::optional<SceneSun> sun;
    float brightest = 0.0f;
    for (const SceneLight& light : lights) {
        const float brightness = std::max({light.color.r, light.color.g, light.color.b});
        if (light.type == GpuLightType::Directional && std::isfinite(brightness) && brightness > brightest) {
            brightest = brightness;
            // What makeSceneLight() multiplied in is taken out again.
            sun = SceneSun{.towards = -light.direction, .color = light.color / kPi};
        }
    }
    return sun;
}

/// The fog of a world: that of the enabled Fog whose entity has the lowest id - which
/// of several is not left to the order they are stored in - at the height of that
/// entity. `sun` is the sun of the same world.
FogSettings findFog(const World& world, const std::optional<SceneSun>& sun) {
    FogSettings fog;
    Uuid chosen;
    const auto fogs = world.registry().view<const FogComponent, const WorldTransform>();
    for (const auto [handle, component, placement] : fogs.each()) {
        const Entity entity{handle};
        if (!world.isEnabledInHierarchy(entity)) {
            continue;
        }
        const Uuid id = world.id(entity);
        if (chosen.isNil() || id < chosen) {
            chosen = id;
            fog = {.color = component.color,
                   .density = component.density,
                   .height = placement.matrix[3].y,
                   .heightFalloff = component.heightFalloff,
                   .startDistance = component.startDistance,
                   .maxOpacity = component.maxOpacity,
                   .sunColor = component.sunColor,
                   .sunExponent = component.sunExponent};
        }
    }

    // A fog without a sun has none of its light to throw on.
    if (sun) {
        fog.towardsSun = sun->towards;
    } else {
        fog.sunColor = Vec3(0.0f);
    }
    return sanitized(fog);
}

} // namespace

SceneLight makeSceneLight(const LightComponent& component, const Mat4& placement) {
    SceneLight light;
    light.type = gpuTypeOf(component.type);
    light.position = Vec3(placement[3]);
    // A light shines along its entity's -Z axis, as a camera looks.
    light.direction = normalizeOr(-Vec3(placement[2]), -kWorldUp);
    // See the file comment of the header for the factor.
    light.color = glm::max(component.color, Vec3(0.0f)) * (std::max(component.intensity, 0.0f) * kPi);
    light.range = component.range;
    const float outer = std::clamp(component.outerConeDegrees, 0.0f, 89.0f);
    light.outerConeAngle = toRadians(outer);
    light.innerConeAngle = toRadians(std::clamp(component.innerConeDegrees, 0.0f, outer));
    light.channels = component.channels & kAllLightChannels;

    light.shadow.enabled = component.castsShadows;
    light.shadow.bias = std::max(component.shadowBias, 0.0f);
    light.shadow.normalOffset = std::max(component.shadowNormalOffset, 0.0f);
    light.shadow.resolutionScale = resolutionScaleOf(component.shadowResolution);
    light.shadow.distance = component.shadowDistance;
    light.shadow.cascades = component.shadowCascades;
    light.shadow.cascadeDistribution = component.shadowCascadeDistribution;
    return light;
}

void SceneLights::collect(const World& world, const SceneLightsDesc& desc) {
    m_lights.clear();
    m_found.clear();
    m_order.clear();
    m_skyColor = Vec3(0.0f);
    m_groundColor = Vec3(0.0f);
    m_environment = {};
    m_fog = {};
    m_sun.reset();
    m_isDefault = false;

    // Found in whatever order the components are stored in, which depends on what the
    // entities have been through; then put in the order of their ids.
    const auto lights = world.registry().view<const LightComponent, const WorldTransform>();
    for (const auto [handle, light, placement] : lights.each()) {
        const Entity entity{handle};
        if (world.isEnabledInHierarchy(entity)) {
            m_order.emplace_back(world.id(entity), static_cast<uint32_t>(m_found.size()));
            m_found.push_back(makeSceneLight(light, placement.matrix));
        }
    }
    // Ids are unique, so the order is the same however the sort goes about it.
    std::ranges::sort(m_order, {}, &std::pair<Uuid, uint32_t>::first);
    m_lights.reserve(m_found.size());
    for (const auto& [id, index] : m_order) {
        m_lights.push_back(m_found[index]);
    }

    // Light adds up, in whatever order.
    const auto ambientLights = world.registry().view<const AmbientLightComponent>();
    for (const auto [handle, ambient] : ambientLights.each()) {
        if (world.isEnabledInHierarchy(Entity{handle})) {
            const float intensity = std::max(ambient.intensity, 0.0f);
            m_skyColor += glm::max(ambient.skyColor, Vec3(0.0f)) * intensity;
            m_groundColor += glm::max(ambient.groundColor, Vec3(0.0f)) * intensity;
        }
    }

    // One environment lights a world. Which of several is not left to the order they
    // are stored in: the one whose entity has the lowest id.
    Uuid chosen;
    const auto environments = world.registry().view<const EnvironmentComponent>();
    for (const auto [handle, environment] : environments.each()) {
        const Entity entity{handle};
        if (!world.isEnabledInHierarchy(entity)) {
            continue;
        }
        const Uuid id = world.id(entity);
        if (chosen.isNil() || id < chosen) {
            chosen = id;
            m_environment = {
                .texture = environment.texture,
                .intensity = std::isfinite(environment.intensity) ? std::max(environment.intensity, 0.0f) : 0.0f,
                .rotation =
                    std::isfinite(environment.rotationDegrees) ? glm::radians(environment.rotationDegrees) : 0.0f,
                .visible = environment.visible};
        }
    }

    // A world whose lights are all switched off is dark on purpose; only one that has
    // none to switch gets the stand-ins.
    const auto lightComponents = world.registry().view<const LightComponent>();
    const bool hasLightComponents = lightComponents.begin() != lightComponents.end() ||
                                    ambientLights.begin() != ambientLights.end() ||
                                    environments.begin() != environments.end();
    if (desc.defaultLighting && !hasLightComponents) {
        m_lights.push_back({.type = GpuLightType::Directional,
                            .direction = glm::normalize(kDefaultKeyDirection),
                            .color = kDefaultKeyColor,
                            .channels = kAllLightChannels});
        m_skyColor = kDefaultSkyColor;
        m_groundColor = kDefaultGroundColor;
        m_isDefault = true;
    }

    // The sun and the fog, once the lights are known: the stand-in may be the sun as
    // well.
    m_sun = findSun(m_lights);
    m_fog = findFog(world, m_sun);
}

} // namespace cordite
