#pragma once

/// @file
/// A world, the assets it uses and a renderer to draw it with: what the tests of the
/// scene renderer and of its lighting are built on.
///
/// @code
/// SceneFixture scene;
/// scene.writeFlatMaterial("red", kRed);
/// scene.open();
/// scene.add(BuiltinMesh::Cube, scene.material("red"));
///
/// const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));
/// checkPixel(image, 32, 32, kRed);
/// checkNoValidationIssues(*scene.context.graphics);
/// @endcode
///
/// Tests state what a pixel must be wherever they can: materials that emit a color and
/// reflect nothing, cameras without perspective, lights of known brightness. lightOn()
/// is the lighting the engine documents, written down a second time on the CPU, for
/// tests that compare whole pictures with it.

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Image.h"
#include "cordite/core/FileSystem.h"
#include "cordite/math/Color.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cordite::test {

/// Linear colors that are easy to tell apart in a rendered image.
inline constexpr Vec3 kRed(1.0f, 0.0f, 0.0f);
inline constexpr Vec3 kGreen(0.0f, 1.0f, 0.0f);
inline constexpr Vec3 kBlue(0.0f, 0.0f, 1.0f);
inline constexpr Vec3 kBackground(0.25f, 0.25f, 0.25f);

/// What a linear color looks like once it has been encoded for display.
inline Vec3 displayed(const Vec3& linear) {
    return linearToSrgb(glm::clamp(linear, Vec3(0.0f), Vec3(1.0f)));
}

/// A device, a project directory to put assets in, and a world to draw.
///
/// Tests write the asset files they need, call open(), fill the world and render it.
class SceneFixture {
public:
    explicit SceneFixture(UVec2 size = UVec2(64, 64)) : m_size(size) {
        REQUIRE(ensureDirectory(m_directory.path()).has_value());
    }

    ~SceneFixture() {
        // GPU objects go before the device that made them.
        renderer.reset();
        targets = SceneTargets();
        context.graphics->waitIdle();
    }

    SceneFixture(const SceneFixture&) = delete;
    SceneFixture& operator=(const SceneFixture&) = delete;

    /// Where a file of the given name is, or would be, in the asset directory.
    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return m_directory.path() / pathFromUtf8(name);
    }

    /// Writes `<name>.material.json` with the given members.
    void writeMaterial(std::string_view name, std::string_view members) const {
        const std::string text =
            fmt::format(R"({{"format": "cordite.material", "version": 1{}{}}})", members.empty() ? "" : ", ", members);
        REQUIRE(writeTextFile(file(fmt::format("{}.material.json", name)), text).has_value());
    }

    /// Writes a material that is perfectly matte and reflects `color` of the light that
    /// reaches it: lit head-on by a light of intensity 1, it shows exactly `color`.
    void writeMatteMaterial(std::string_view name, const Vec3& color = Vec3(1.0f), std::string_view more = "") const {
        writeMaterial(name, fmt::format(R"("baseColor": [{}, {}, {}, 1], "specular": 0, "roughness": 1{}{})", color.r,
                                        color.g, color.b, more.empty() ? "" : ", ", more));
    }

    /// Writes a material that shows `color` exactly, whatever the light: it reflects
    /// nothing and emits the color.
    void writeFlatMaterial(std::string_view name, const Vec3& color, std::string_view more = "") const {
        writeMaterial(name,
                      fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, "emissiveColor": [{}, {}, {}]{}{})",
                                  color.r, color.g, color.b, more.empty() ? "" : ", ", more));
    }

    void writeImage(std::string_view name, const Image& image) const {
        REQUIRE(image.savePng(file(fmt::format("{}.png", name))).has_value());
    }

    /// Writes `<name>.hdr`: an image of light, which may be brighter than white.
    void writeHdrImage(std::string_view name, const Image& image) const {
        REQUIRE(image.saveHdr(file(fmt::format("{}.hdr", name))).has_value());
    }

    /// Indexes the files written so far and creates the renderer.
    void open(const RenderResourcesDesc& resources = {}) {
        assets = unwrap(AssetManager::create({.rootDirectory = m_directory.path()}));
        renderer = unwrap(SceneRenderer::create(
            context.device(), *context.shaders, *assets,
            {.resources = resources, .supportsWireframe = context.graphics->adapter().supportsWireframe}));
        targets = unwrap(SceneTargets::create(context.device(), m_size, "Test"));
    }

    [[nodiscard]] Uuid asset(std::string_view reference) const {
        const AssetInfo* info = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(info != nullptr, "no asset " << std::string(reference));
        return info->id;
    }

    [[nodiscard]] Uuid material(std::string_view name) const { return asset(fmt::format("{}.material.json", name)); }

    Entity add(BuiltinMesh mesh, const Uuid& materialId, const Transform& transform = {}) {
        const Entity entity = world.createEntity(std::string(nameOf(mesh)));
        world.transform(entity) = transform;
        MeshRendererComponent& component = world.add<MeshRendererComponent>(entity);
        component.mesh = builtinMeshId(mesh);
        component.materials = {materialId};
        return entity;
    }

    /// Renders the world and returns the display image.
    Image render(const CameraView& view, const SceneRenderSettings& settings = flatSettings()) {
        world.updateWorldTransforms();
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            renderer->render(commandList, world, view, targets, settings);
        });
        return readToImage(*context.graphics, targets.display());
    }

    /// A grey background and nothing else: in a world without lights, what is seen is
    /// what materials emit - shown as it is, with nothing worked out from the screen
    /// and not finished into a picture, so that a test can say what a pixel must be.
    static SceneRenderSettings flatSettings() {
        return {.backgroundColor = kBackground,
                .screenEffectsQuality = ScreenEffectsQuality::Off,
                .postProcessQuality = PostProcessQuality::Off};
    }

    /// Adds a light at `position` that shines along `direction`, which a point light
    /// has no use for.
    Entity addLight(const LightComponent& light, const Vec3& position, const Vec3& direction = kWorldForward) {
        const Entity entity = world.createEntity("Light");
        world.transform(entity).position = position;
        // A light shines along its -Z axis, the way lookRotation() makes an entity face.
        world.transform(entity).rotation = lookRotation(direction);
        world.add<LightComponent>(entity) = light;
        return entity;
    }

    /// Lights the world with a directional light that arrives from `toLight` - the
    /// direction from a surface towards the light - and is `radiance` bright there: a
    /// white, matte surface that faces it throws back radiance / pi. Calling it again
    /// moves and recolors the same light.
    void setSun(const Vec3& toLight, const Vec3& radiance) {
        if (!world.isValid(m_sun)) {
            m_sun = world.createEntity("Sun");
            world.add<LightComponent>(m_sun);
        }
        world.transform(m_sun).rotation = lookRotation(-toLight);
        LightComponent& light = *world.tryGet<LightComponent>(m_sun);
        light.type = LightType::Directional;
        // An intensity of 1 means radiance pi; see SceneLights.h.
        light.color = radiance / kPi;
        light.intensity = 1.0f;
    }

    /// Lights the world from all around: `sky` from above, `ground` from below.
    void setAmbient(const Vec3& sky, const Vec3& ground) {
        if (!world.isValid(m_ambient)) {
            m_ambient = world.createEntity("Ambient");
            world.add<AmbientLightComponent>(m_ambient);
        }
        AmbientLightComponent& ambient = *world.tryGet<AmbientLightComponent>(m_ambient);
        ambient.skyColor = sky;
        ambient.groundColor = ground;
        ambient.intensity = 1.0f;
    }

    /// Surrounds the world with the image asset `texture`: seen as the sky, as bright
    /// as it is, and not turned. Calling it again changes the same environment. The
    /// reference is good until the next component is added or removed.
    EnvironmentComponent& setEnvironment(const Uuid& texture) {
        if (!world.isValid(m_environment)) {
            m_environment = world.createEntity("Environment");
            world.add<EnvironmentComponent>(m_environment);
        }
        EnvironmentComponent& environment = *world.tryGet<EnvironmentComponent>(m_environment);
        environment = {.texture = texture};
        return environment;
    }

    /// The entity setEnvironment() made; null before it was called.
    [[nodiscard]] Entity environmentEntity() const { return m_environment; }

    /// A camera without perspective that looks from `eye` at `target` and sees
    /// `height` metres from top to bottom.
    [[nodiscard]] CameraView orthographicView(const Vec3& eye, const Vec3& target, float height,
                                              const Vec3& up = kWorldUp) const {
        CameraComponent camera;
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = height;
        camera.nearPlane = 0.1f;
        camera.farPlane = 100.0f;
        return makeCameraView(glm::inverse(lookAt(eye, target, up)), camera,
                              static_cast<float>(m_size.x) / static_cast<float>(m_size.y));
    }

    [[nodiscard]] CameraView perspectiveView(const Vec3& eye, const Vec3& target, float fovDegrees = 50.0f) const {
        CameraComponent camera;
        camera.verticalFovDegrees = fovDegrees;
        return makeCameraView(glm::inverse(lookAt(eye, target)), camera,
                              static_cast<float>(m_size.x) / static_cast<float>(m_size.y));
    }

    RenderContext context;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<SceneRenderer> renderer;
    SceneTargets targets;
    World world;

private:
    TempDirectory m_directory;
    UVec2 m_size;
    Entity m_sun;
    Entity m_ambient;
    Entity m_environment;
};

/// A light and where it is, for tests that work out themselves what it must light.
struct PlacedLight {
    LightComponent light;
    Vec3 position{0.0f};
    /// Unit vector along which a directional or a spot light shines.
    Vec3 direction{0.0f, 0.0f, -1.0f};
};

/// What a white, perfectly matte surface at `point` that faces `normal` shows of a
/// light: the lighting the engine documents (RenderComponents.h, Lights.slang), written
/// down a second time, in the plainest way.
inline Vec3 lightOn(const Vec3& point, const Vec3& normal, const PlacedLight& placed) {
    const LightComponent& light = placed.light;
    // An intensity of one lights such a surface to the color of the light when the
    // surface faces it: head-on at any distance for a directional light, ...
    const Vec3 color = light.color * light.intensity;
    if (light.type == LightType::Directional) {
        return color * std::max(glm::dot(normal, -placed.direction), 0.0f);
    }

    // ... and one metre away for the others, which thin out with the square of the
    // distance and are brought to nothing at their range.
    const Vec3 offset = placed.position - point;
    const float distanceSquared = glm::dot(offset, offset);
    if (distanceSquared >= light.range * light.range) {
        return Vec3(0.0f);
    }
    const Vec3 toLight = offset / std::sqrt(distanceSquared);
    const float ratio = distanceSquared / (light.range * light.range);
    const float window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
    float brightness = window * window / distanceSquared;

    if (light.type == LightType::Spot) {
        // All of it inside the inner cone, none outside the outer one.
        const float cosInner = std::cos(toRadians(light.innerConeDegrees));
        const float cosOuter = std::cos(toRadians(light.outerConeDegrees));
        const float amount =
            std::clamp((glm::dot(-toLight, placed.direction) - cosOuter) / (cosInner - cosOuter), 0.0f, 1.0f);
        brightness *= amount * amount;
    }
    return color * (brightness * std::max(glm::dot(normal, toLight), 0.0f));
}

/// The same where things stand in the light: `casters` are boxes that let none of it
/// through. A point a light cannot see gets nothing of it.
inline Vec3 lightOn(const Vec3& point, const Vec3& normal, const PlacedLight& placed, std::span<const Aabb> casters) {
    const Vec3 unshadowed = lightOn(point, normal, placed);
    if (std::max({unshadowed.r, unshadowed.g, unshadowed.b}) <= 0.0f) {
        return unshadowed;
    }
    const bool isDirectional = placed.light.type == LightType::Directional;
    const Vec3 toLight = isDirectional ? -placed.direction : glm::normalize(placed.position - point);
    const float reach = isDirectional ? std::numeric_limits<float>::max() : glm::distance(placed.position, point);
    // From a hair above the surface, so that the surface a box stands on is not in the
    // box's shadow where the two touch.
    const Ray towardsLight{point + normal * 1.0e-3f, toLight};
    for (const Aabb& caster : casters) {
        const std::optional<float> hit = intersect(towardsLight, caster);
        if (hit && *hit < reach) {
            return Vec3(0.0f);
        }
    }
    return unshadowed;
}

/// Checks one pixel against a linear color, allowing for rounding.
inline void checkPixel(const Image& image, uint32_t x, uint32_t y, const Vec3& linear) {
    CAPTURE(x);
    CAPTURE(y);
    CHECK_NEAR(Vec3(image.pixel(x, y)), displayed(linear), 2.5f / 255.0f);
}

/// Checks one pixel against the value the image itself is to hold there, as the debug
/// views show numbers: 0.25 as the grey a quarter of the way to white.
inline void checkShown(const Image& image, uint32_t x, uint32_t y, const Vec3& value) {
    CAPTURE(x);
    CAPTURE(y);
    CHECK_NEAR(Vec3(image.pixel(x, y)), value, 2.5f / 255.0f);
}

inline Transform at(const Vec3& position, const Vec3& scale = Vec3(1.0f), const Quat& rotation = kIdentityQuat) {
    return {.position = position, .rotation = rotation, .scale = scale};
}

/// A square image of `cells` x `cells` squares in two colors, top-left square first.
inline Image makeChecker(uint32_t size, uint32_t cells, const Vec4& first, const Vec4& second) {
    Image image = unwrap(Image::create(size, size, ImageFormat::Rgba8));
    const uint32_t cellSize = size / cells;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            image.setPixel(x, y, ((x / cellSize) + (y / cellSize)) % 2 == 0 ? first : second);
        }
    }
    return image;
}

/// A normal map of round bumps, one per cell: the surface normal of each bump, encoded
/// the way normal maps are, with +Y up in the image.
inline Image makeBumpNormalMap(uint32_t size, uint32_t cells) {
    Image image = unwrap(Image::create(size, size, ImageFormat::Rgba8));
    const float cellSize = static_cast<float>(size) / static_cast<float>(cells);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            // Position within the cell, from -1 to 1, with y pointing up.
            const float u = std::fmod(static_cast<float>(x) + 0.5f, cellSize) / cellSize * 2.0f - 1.0f;
            const float v = 1.0f - std::fmod(static_cast<float>(y) + 0.5f, cellSize) / cellSize * 2.0f;
            const float radius = std::sqrt(u * u + v * v);
            Vec3 normal(0.0f, 0.0f, 1.0f);
            if (radius < 0.8f) {
                // The slope of a dome that meets the flat surface at radius 0.8.
                const float slope = radius / 0.8f;
                normal = glm::normalize(Vec3(u * slope, v * slope, std::sqrt(std::max(1.0f - slope * slope, 0.05f))));
            }
            image.setPixel(x, y, Vec4(normal * 0.5f + 0.5f, 1.0f));
        }
    }
    return image;
}

} // namespace cordite::test
