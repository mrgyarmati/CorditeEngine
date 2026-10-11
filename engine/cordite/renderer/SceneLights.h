#pragma once

/// @file
/// The lights of a world, gathered for the renderer.
///
/// @code
/// SceneLights lights;
/// lights.collect(world);
///
/// LightClusters clusters;
/// buildLightClusters(view, lights.lights(), clusters);
/// @endcode
///
/// Brightness is converted here from what a person sets to what the shaders compute
/// with. A light of intensity 1 is to light a white, matte surface that faces it to
/// white; a matte surface throws back one part in pi of the light that reaches it per
/// unit of solid angle, so the light is handed on pi times as bright.

#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cordite {

struct SceneLightsDesc {
    /// Light a world that has no light components at all - not one Light or
    /// AmbientLight, switched on or off - with a key light from above and soft light
    /// from all around, so that what is in it can be seen. Editors do, for a scene that
    /// is being started; a game shows what its scene says.
    bool defaultLighting = false;
};

/// A light as the renderer works with it: `placement` is the world matrix of its
/// entity. The light is at its origin and shines along its -Z axis; the brightness is
/// converted as the file comment says, and the cone angles are brought into order.
[[nodiscard]] SceneLight makeSceneLight(const LightComponent& component, const Mat4& placement);

/// The lights of a world, in the form the renderer works with. Keep one and collect
/// into it every frame: it reuses its memory.
class SceneLights {
public:
    /// Gathers the lights of the enabled entities of `world`, replacing what was held.
    /// The world transforms must be up to date.
    void collect(const World& world, const SceneLightsDesc& desc = {});

    /// Every enabled light, in the order of the ids of their entities. That order is
    /// the same on every run and does not change when entities are edited, so neither
    /// do the sums of the lights, nor which of them are left out when a view has more
    /// than it can hold.
    [[nodiscard]] std::span<const SceneLight> lights() const { return m_lights; }

    /// Ambient light arriving from above and from below: all enabled AmbientLights
    /// added up, each times its intensity. Black without one.
    [[nodiscard]] const Vec3& skyColor() const { return m_skyColor; }

    [[nodiscard]] const Vec3& groundColor() const { return m_groundColor; }

    /// True when the lights are the stand-ins of SceneLightsDesc::defaultLighting.
    [[nodiscard]] bool isDefault() const { return m_isDefault; }

private:
    std::vector<SceneLight> m_lights;
    /// Working memory of collect(): the lights as they were found, and the ids of
    /// their entities with their places in that list, which is what gets sorted.
    std::vector<SceneLight> m_found;
    std::vector<std::pair<Uuid, uint32_t>> m_order;
    Vec3 m_skyColor{0.0f};
    Vec3 m_groundColor{0.0f};
    bool m_isDefault = false;
};

} // namespace cordite
