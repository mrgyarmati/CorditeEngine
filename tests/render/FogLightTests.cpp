#include "cordite/assets/Image.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/renderer/VolumetricFog.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::kBackground;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

/// A picture that shows the light as it was worked out, with what lights throw into the fog
/// worked out at `quality`.
SceneRenderSettings litFogSettings(ScreenEffectsQuality quality = ScreenEffectsQuality::Medium) {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.screenEffectsQuality = quality;
    return settings;
}

/// Four half floats a texel, read back.
struct HalfPicture {
    TexturePixels pixels;

    [[nodiscard]] Vec3 at(uint32_t x, uint32_t y) const {
        REQUIRE(pixels.format == nvrhi::Format::RGBA16_FLOAT);
        std::array<uint16_t, 3> halves{};
        std::memcpy(halves.data(), pixels.data.data() + (static_cast<size_t>(y) * pixels.width + x) * 8,
                    sizeof(halves));
        return {glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]), glm::unpackHalf1x16(halves[2])};
    }
};

/// The light of the scene color of the picture last drawn, as it was worked out.
HalfPicture sceneLight(SceneFixture& scene) {
    return {unwrap(readTexture(*scene.context.graphics, scene.targets.sceneColor()))};
}

/// Checks a light against what it is to be, to what a half float holds.
void checkLight(const Vec3& actual, const Vec3& expected) {
    CHECK_NEAR(actual, expected, 2.0e-3f * std::max(glm::length(expected), 0.05f));
}

/// Puts a fog into a world, as dense everywhere, and lit by lights.
Entity addLitFog(SceneFixture& scene, const FogComponent& fog) {
    const Entity entity = scene.world.createEntity("Fog");
    scene.world.add<FogComponent>(entity) = fog;
    return entity;
}

FogComponent evenLitFog(float density, float volumetricDistance) {
    return {.color = Vec3(0.1f, 0.12f, 0.15f),
            .density = density,
            .heightFalloff = 0.0f,
            .volumetric = true,
            .scattering = Vec3(1.0f, 0.9f, 0.8f),
            .anisotropy = 0.4f,
            .volumetricDistance = volumetricDistance};
}

/// The fog such a component makes at height zero, put together by hand.
FogSettings fogOf(const FogComponent& component) {
    FogSettings fog;
    fog.color = component.color;
    fog.density = component.density;
    fog.heightFalloff = component.heightFalloff;
    fog.startDistance = component.startDistance;
    fog.maxOpacity = component.maxOpacity;
    fog.volumetric = component.volumetric;
    fog.scattering = component.scattering;
    fog.anisotropy = component.anisotropy;
    fog.volumetricDistance = component.volumetricDistance;
    return fog;
}

/// What the GPU is given of a light, as far as the fog goes: its color is pi times its
/// intensity (SceneLights.h), and the fog's that times its volumetricIntensity.
GpuLight fogLightOf(const LightComponent& component, const Vec3& position, const Vec3& direction) {
    GpuLight light;
    light.typeAndChannels = static_cast<uint32_t>(component.type == LightType::Directional ? GpuLightType::Directional
                                                  : component.type == LightType::Spot      ? GpuLightType::Spot
                                                                                           : GpuLightType::Point);
    light.position = position;
    light.direction = glm::normalize(direction);
    light.range = component.range;
    light.color = component.color * component.intensity * kPi;
    light.fogColor = light.color * component.volumetricIntensity;
    return light;
}

/// A camera without perspective, five metres in front of the origin, looking along -Z and
/// seeing three metres either way: a pixel of a picture 64 wide is 3/64 of a metre.
CameraView frontView(const SceneFixture& scene) {
    return scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 3.0f);
}

/// Where the line of sight of the point `uv` of the front view sets out.
Vec3 frontViewerAt(const Vec2& uv) {
    return {(uv.x - 0.5f) * 3.0f, (0.5f - uv.y) * 3.0f, 5.0f};
}

/// Where that of the middle of the pixel `x`, `y` of a picture `size` pixels a side sets out.
Vec3 frontViewerOf(uint32_t x, uint32_t y, uint32_t size = 64) {
    return frontViewerAt((Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / static_cast<float>(size));
}

FogSight frontSight(const Vec3& viewer, float distance, uint32_t slices) {
    return {.viewer = viewer,
            .direction = Vec3(0.0f, 0.0f, -1.0f),
            .forward = Vec3(0.0f, 0.0f, -1.0f),
            .distance = distance,
            .slices = slices};
}

/// Four walls side by side across the top half of the front view, each 0.75 m wide and
/// of its own color, their fronts 7, 15, 35 and 60 metres from the camera; below them the
/// view looks past everything.
struct Walls {
    static constexpr std::array<float, 4> kDistances = {7.0f, 15.0f, 35.0f, 60.0f};
    static constexpr std::array<uint32_t, 4> kColumns = {8, 24, 40, 56};
    static constexpr uint32_t kRow = 16;
    static constexpr uint32_t kSkyRow = 48;
    static constexpr std::array<Vec3, 4> kColors = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                                    Vec3(0.0f, 0.0f, 1.0f), Vec3(0.5f, 0.5f, 0.0f)};

    static void write(SceneFixture& scene) {
        for (size_t wall = 0; wall < kColors.size(); ++wall) {
            scene.writeFlatMaterial(fmt::format("wall{}", wall), kColors[wall]);
        }
    }

    static void place(SceneFixture& scene) {
        for (size_t wall = 0; wall < kColors.size(); ++wall) {
            const float x = -1.125f + 0.75f * static_cast<float>(wall);
            scene.add(BuiltinMesh::Cube, scene.material(fmt::format("wall{}", wall)),
                      at(Vec3(x, 0.75f, 5.0f - kDistances[wall] - 0.5f), Vec3(0.75f, 1.5f, 1.0f)));
        }
    }
};

/// A sun that shines down and towards the camera, nothing in its way.
LightComponent sunComponent() {
    return {.type = LightType::Directional,
            .color = Vec3(1.0f, 0.9f, 0.7f),
            .intensity = 1.5f,
            .castsShadows = false,
            .volumetricIntensity = 0.8f};
}

const Vec3 kSunDirection = glm::normalize(Vec3(0.5f, -1.0f, 1.0f));

} // namespace

TEST_SUITE("FogLight") {
    TEST_CASE(
        "a sun that nothing shadows lights the fog by how much fog there is: before walls near and far, and the sky") {
        SceneFixture scene;
        Walls::write(scene);
        scene.open();
        Walls::place(scene);
        const FogComponent component = evenLitFog(0.04f, 40.0f);
        addLitFog(scene, component);
        scene.addLight(sunComponent(), Vec3(0.0f), kSunDirection);

        scene.render(frontView(scene), litFogSettings());
        const HalfPicture light = sceneLight(scene);
        const FogSettings fog = fogOf(component);
        const std::array<GpuLight, 1> lights = {fogLightOf(sunComponent(), Vec3(0.0f), kSunDirection)};
        const uint32_t slices = fogVolumeGridFor(UVec2(64), ScreenEffectsQuality::Medium).slices;
        for (size_t wall = 0; wall < Walls::kColors.size(); ++wall) {
            CAPTURE(wall);
            const Vec3 viewer = frontViewerOf(Walls::kColumns[wall], Walls::kRow);
            const Vec3 front = viewer + Vec3(0.0f, 0.0f, -Walls::kDistances[wall]);
            const Vec3 expected = applyFog(fog, Walls::kColors[wall], viewer, front) +
                                  fogLightAlong(fog, lights, frontSight(viewer, Walls::kDistances[wall], slices));
            checkLight(light.at(Walls::kColumns[wall], Walls::kRow), expected);
        }
        // The sky, through all the fog there is: the closed form takes over at 40 metres.
        const Vec3 viewer = frontViewerOf(32, Walls::kSkyRow);
        const Vec3 sky = applyFogTowards(fog, kBackground, viewer, Vec3(0.0f, 0.0f, -1.0f)) +
                         fogLightAlong(fog, lights, frontSight(viewer, kInfinity, slices));
        checkLight(light.at(32, Walls::kSkyRow), sky);
        // Which, for a fog that hides the sky, is all the sun gives it.
        const Vec3 all = fog.scattering * lights[0].fogColor *
                         henyeyGreenstein(glm::dot(kSunDirection, Vec3(0.0f, 0.0f, 1.0f)), fog.anisotropy);
        checkLight(fogLightAlong(fog, lights, frontSight(viewer, kInfinity, slices)), all);

        // The debug view shows that alone.
        SceneRenderSettings debug = litFogSettings();
        debug.debugView = RenderDebugView::FogLight;
        scene.render(frontView(scene), debug);
        const HalfPicture alone = sceneLight(scene);
        checkLight(alone.at(32, Walls::kSkyRow), all);
        const Vec3 wallViewer = frontViewerOf(Walls::kColumns[1], Walls::kRow);
        checkLight(alone.at(Walls::kColumns[1], Walls::kRow),
                   fogLightAlong(fog, lights, frontSight(wallViewer, Walls::kDistances[1], slices)));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the sun throws its shadows and its cookie into the fog, and beyond the grid shines unhindered") {
        SceneFixture scene;
        scene.writeFlatMaterial("black", Vec3(0.0f));
        Image dark = unwrap(Image::create(8, 8, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < dark.height(); ++y) {
            for (uint32_t x = 0; x < dark.width(); ++x) {
                dark.setPixel(x, y, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            }
        }
        scene.writeImage("dark", dark);
        scene.open();
        const FogComponent component = evenLitFog(0.04f, 40.0f);
        addLitFog(scene, component);
        // Straight down.
        LightComponent sunLight = sunComponent();
        sunLight.shadowDistance = 100.0f;
        const Vec3 down(0.0f, -1.0f, 0.0f);
        const Entity sun = scene.addLight(sunLight, Vec3(0.0f), down);

        // What the sun gives the sky of the front view as nothing is in its way, and of that
        // what comes from beyond the grid alone.
        const FogSettings fog = fogOf(component);
        const std::array<GpuLight, 1> lights = {fogLightOf(sunLight, Vec3(0.0f), down)};
        const uint32_t slices = fogVolumeGridFor(UVec2(64), ScreenEffectsQuality::Medium).slices;
        const Vec3 viewer = frontViewerOf(32, 32);
        const Vec3 behind = applyFogTowards(fog, kBackground, viewer, Vec3(0.0f, 0.0f, -1.0f));
        const Vec3 all = fogLightAlong(fog, lights, frontSight(viewer, kInfinity, slices));
        const Vec3 beyond = all - fogLightAlong(fog, lights, frontSight(viewer, component.volumetricDistance, slices));
        REQUIRE(beyond.x > 0.05f * all.x);

        scene.render(frontView(scene), litFogSettings());
        checkLight(sceneLight(scene).at(32, 32), behind + all);

        // A cookie that lets nothing through: the grid has none of the sun, what is beyond
        // it all of it.
        scene.world.tryGet<LightComponent>(sun)->cookie = scene.asset("dark.png");
        scene.render(frontView(scene), litFogSettings());
        checkLight(sceneLight(scene).at(32, 32), behind + beyond);
        scene.world.tryGet<LightComponent>(sun)->cookie = Uuid{};

        // A roof over all of the view, above what the camera sees: in its shadow the fog of
        // the grid gets nothing - and the shadow ends where the grid does.
        scene.add(BuiltinMesh::Cube, scene.material("black"), at(Vec3(0.0f, 3.0f, 0.0f), Vec3(200.0f, 1.0f, 200.0f)));
        scene.world.tryGet<LightComponent>(sun)->castsShadows = true;
        scene.render(frontView(scene), litFogSettings());
        checkLight(sceneLight(scene).at(32, 32), behind + beyond);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what is nearer than the end of the first slice gets as much of its sum as it is in") {
        SceneFixture scene;
        scene.writeFlatMaterial("black", Vec3(0.0f));
        scene.open();
        // A grid a kilometre deep, of the 48 slices of the lowest quality: the first ends
        // 1000 / 48^2 = 0.43 metres from the camera. A wall 0.2 metres in front of it.
        const FogComponent component = evenLitFog(0.04f, 1000.0f);
        addLitFog(scene, component);
        scene.addLight(sunComponent(), Vec3(0.0f), kSunDirection);
        scene.add(BuiltinMesh::Cube, scene.material("black"), at(Vec3(0.0f, 0.0f, 4.3f), Vec3(10.0f, 10.0f, 1.0f)));

        SceneRenderSettings settings = litFogSettings(ScreenEffectsQuality::Low);
        settings.debugView = RenderDebugView::FogLight;
        scene.render(frontView(scene), settings);
        const FogSettings fog = fogOf(component);
        const std::array<GpuLight, 1> lights = {fogLightOf(sunComponent(), Vec3(0.0f), kSunDirection)};
        const uint32_t slices = fogVolumeGridFor(UVec2(64), ScreenEffectsQuality::Low).slices;
        const float firstEnd = fogSliceDepth(1.0f / static_cast<float>(slices), component.volumetricDistance);
        REQUIRE(firstEnd > 0.4f);
        const Vec3 near = fogLightAlong(fog, lights, frontSight(frontViewerOf(32, 32), 0.2f, slices));
        const Vec3 wholeSlice = fogLightAlong(fog, lights, frontSight(frontViewerOf(32, 32), firstEnd, slices));
        // Not the whole of the first slice's sum.
        REQUIRE(near.x < 0.8f * wholeSlice.x);
        checkLight(sceneLight(scene).at(32, 32), near);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what a point light throws into the fog is worked out for each box, and added up along each column") {
        SceneFixture scene;
        scene.open();
        const FogComponent component = evenLitFog(0.1f, 20.0f);
        addLitFog(scene, component);
        const LightComponent point{.type = LightType::Point,
                                   .color = Vec3(1.0f, 0.8f, 0.6f),
                                   .intensity = 2.0f,
                                   .range = 4.0f,
                                   .castsShadows = false};
        const Vec3 position(0.2f, -0.1f, -3.0f);
        scene.addLight(point, position);
        scene.render(frontView(scene), litFogSettings());

        const FogVolumeGrid grid = fogVolumeGridFor(UVec2(64), ScreenEffectsQuality::Medium);
        REQUIRE(grid == FogVolumeGrid{6, 6, 64});
        nvrhi::ITexture* volume = scene.targets.fogVolume();
        REQUIRE(volume != nullptr);
        CHECK(volume->getDesc().width == grid.columns);
        CHECK(volume->getDesc().height == grid.rows);
        CHECK(volume->getDesc().depth == grid.slices);
        CHECK(scene.renderer->statistics().fogVolumeBytes == uint64_t{6} * 6 * 64 * 8 * 2);

        // Each texel of a slice holds the sum along its column up to the end of the slice.
        const FogSettings fog = fogOf(component);
        const std::array<GpuLight, 1> lights = {fogLightOf(point, position, Vec3(0.0f, -1.0f, 0.0f))};
        uint32_t lit = 0;
        for (const uint32_t slice : {30u, 41u, 45u, 63u}) {
            const HalfPicture texels{unwrap(readTexture(*scene.context.graphics, volume, 0, slice))};
            for (uint32_t row = 0; row < grid.rows; ++row) {
                for (uint32_t column = 0; column < grid.columns; ++column) {
                    CAPTURE(slice);
                    CAPTURE(row);
                    CAPTURE(column);
                    const Vec3 viewer =
                        frontViewerAt((Vec2(static_cast<float>(column), static_cast<float>(row)) + 0.5f) /
                                      Vec2(static_cast<float>(grid.columns), static_cast<float>(grid.rows)));
                    const float end = fogSliceDepth(static_cast<float>(slice + 1) / static_cast<float>(grid.slices),
                                                    component.volumetricDistance);
                    const Vec3 expected = fogLightAlong(fog, lights, frontSight(viewer, end, grid.slices));
                    checkLight(texels.at(column, row), expected);
                    lit += expected.x > 0.01f ? 1 : 0;
                }
            }
        }
        // The light is in it, and not everywhere: some columns pass it by.
        CHECK(lit > 8);
        CHECK(lit < 4 * 36);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("with perspective each column of the grid follows its own line of sight") {
        SceneFixture scene;
        scene.open();
        const FogComponent component = evenLitFog(0.04f, 40.0f);
        addLitFog(scene, component);
        // A wide view, whose columns at the edges look along lines of sight far from the way
        // the camera looks: a depth is a longer way there.
        const CameraView view = scene.perspectiveView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 70.0f);
        const LightComponent point{.type = LightType::Point,
                                   .color = Vec3(1.0f, 0.8f, 0.6f),
                                   .intensity = 3.0f,
                                   .range = 6.0f,
                                   .castsShadows = false};
        const Vec3 position(-2.5f, 1.5f, -2.0f);
        const Entity pointEntity = scene.addLight(point, position);
        const Entity sun = scene.addLight(sunComponent(), Vec3(0.0f), kSunDirection);
        scene.world.tryGet<LightComponent>(sun)->volumetricIntensity = 0.0f;
        scene.render(view, litFogSettings());

        // Every texel of the point light's volume, against the sum along its column's own
        // line of sight up to the end of its slice.
        const FogVolumeGrid grid = fogVolumeGridFor(UVec2(64), ScreenEffectsQuality::Medium);
        const FogSettings fog = fogOf(component);
        const std::array<GpuLight, 1> pointLights = {fogLightOf(point, position, Vec3(0.0f, -1.0f, 0.0f))};
        uint32_t lit = 0;
        for (const uint32_t slice : {35u, 50u, 63u}) {
            const HalfPicture texels{unwrap(readTexture(*scene.context.graphics, scene.targets.fogVolume(), 0, slice))};
            for (uint32_t row = 0; row < grid.rows; ++row) {
                for (uint32_t column = 0; column < grid.columns; ++column) {
                    CAPTURE(slice);
                    CAPTURE(row);
                    CAPTURE(column);
                    const Vec2 middle = (Vec2(static_cast<float>(column), static_cast<float>(row)) + 0.5f) /
                                        Vec2(static_cast<float>(grid.columns), static_cast<float>(grid.rows));
                    const Vec3 direction = screenRay(view, middle * 64.0f, Vec2(64.0f)).direction;
                    const float depth = fogSliceDepth(static_cast<float>(slice + 1) / static_cast<float>(grid.slices),
                                                      component.volumetricDistance);
                    const FogSight sight{.viewer = view.position,
                                         .direction = direction,
                                         .forward = view.forward,
                                         .distance = depth / glm::dot(direction, view.forward),
                                         .slices = grid.slices};
                    const Vec3 expected = fogLightAlong(fog, pointLights, sight);
                    checkLight(texels.at(column, row), expected);
                    lit += expected.x > 0.01f ? 1 : 0;
                }
            }
        }
        CHECK(lit > 3);

        // A sun that nothing shadows, at the corner of the picture, where the line of sight
        // leaves the grid furthest from the camera: as much as its own line of sight gives.
        // (The pixel reads the column nearest to it, whose line of sight is a little off its
        // own: a percent, here.)
        scene.world.tryGet<LightComponent>(sun)->volumetricIntensity = 0.8f;
        scene.world.tryGet<LightComponent>(pointEntity)->volumetricIntensity = 0.0f;
        scene.render(view, litFogSettings());
        const std::array<GpuLight, 1> sunLights = {fogLightOf(sunComponent(), Vec3(0.0f), kSunDirection)};
        const Ray corner = screenRay(view, Vec2(2.5f), Vec2(64.0f));
        const FogSight sky{.viewer = view.position,
                           .direction = corner.direction,
                           .forward = view.forward,
                           .distance = kInfinity,
                           .slices = grid.slices};
        const Vec3 expected =
            applyFogTowards(fog, kBackground, view.position, corner.direction) + fogLightAlong(fog, sunLights, sky);
        CHECK_NEAR(sceneLight(scene).at(2, 2), expected, 0.02f * glm::length(expected));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a spot light lights the fog inside its cone, and outside it adds nothing at all") {
        SceneFixture scene(UVec2(128, 128));
        scene.open();
        addLitFog(scene, evenLitFog(0.1f, 20.0f));
        // Across the view, at the height of the middle row of the picture: a beam of a few
        // boxes of the grid across (boxes of 8 pixels, 0.1875 m).
        LightComponent spot{.type = LightType::Spot,
                            .color = Vec3(1.0f),
                            .intensity = 40.0f,
                            .range = 10.0f,
                            .innerConeDegrees = 8.0f,
                            .outerConeDegrees = 12.0f,
                            .castsShadows = false,
                            .volumetricIntensity = 0.0f};
        const Entity entity = scene.addLight(spot, Vec3(-1.4f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f));
        const SceneRenderSettings settings = litFogSettings(ScreenEffectsQuality::High);
        scene.render(frontView(scene), settings);
        const HalfPicture without = sceneLight(scene);

        scene.world.tryGet<LightComponent>(entity)->volumetricIntensity = 1.0f;
        scene.render(frontView(scene), settings);
        const HalfPicture with = sceneLight(scene);
        // In the beam - 1.4, 1.9 and 2.4 metres from the light - the light it throws
        // towards the viewer.
        for (const uint32_t x : {64u, 85u, 106u}) {
            CAPTURE(x);
            CHECK(with.at(x, 64).x > without.at(x, 64).x + 0.03f);
        }
        // Rows well above and below the cone - more than two boxes away - not a bit of it.
        for (const uint32_t y : {2u, 8u, 14u, 113u, 119u, 125u}) {
            for (uint32_t x = 0; x < 128; x += 9) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(with.at(x, y) == without.at(x, y));
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what stands in a light's way throws its shadow into the fog") {
        SceneFixture scene(UVec2(128, 128));
        scene.writeFlatMaterial("black", Vec3(0.0f));
        scene.open();
        addLitFog(scene, evenLitFog(0.15f, 20.0f));
        // Straight down, past a bar that runs along the line of sight, left of the light.
        LightComponent spot{.type = LightType::Spot,
                            .color = Vec3(1.0f),
                            .intensity = 30.0f,
                            .range = 6.0f,
                            .innerConeDegrees = 25.0f,
                            .outerConeDegrees = 30.0f,
                            .castsShadows = true,
                            .volumetricIntensity = 0.0f};
        const Entity entity = scene.addLight(spot, Vec3(0.0f, 1.4f, -5.0f), Vec3(0.0f, -1.0f, 0.0f));
        scene.add(BuiltinMesh::Cube, scene.material("black"), at(Vec3(-0.2f, 0.9f, -5.0f), Vec3(0.2f, 0.2f, 1.5f)));

        // The bar's shadow is 0.76 m to the left half a metre below the middle of the view;
        // as far to the right the fog is lit.
        const auto added = [&](bool shadows) {
            LightComponent& light = *scene.world.tryGet<LightComponent>(entity);
            light.volumetricIntensity = 0.0f;
            scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
            const HalfPicture before = sceneLight(scene);
            light.volumetricIntensity = 1.0f;
            light.castsShadows = shadows;
            scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
            const HalfPicture after = sceneLight(scene);
            // Pixels 32 and 95 of 128 are 0.76 m either side of the middle, row 85 half a
            // metre below it.
            return std::array<float, 2>{after.at(32, 85).x - before.at(32, 85).x,
                                        after.at(95, 85).x - before.at(95, 85).x};
        };
        const std::array<float, 2> shadowed = added(true);
        CAPTURE(shadowed[0]);
        CAPTURE(shadowed[1]);
        CHECK(shadowed[1] > 0.02f);
        CHECK(shadowed[0] < 0.3f * shadowed[1]);
        // Without shadows the two sides are alike.
        const std::array<float, 2> open = added(false);
        CAPTURE(open[0]);
        CAPTURE(open[1]);
        CHECK(std::abs(open[0] - open[1]) < 0.05f * open[1]);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a spot light's cookie is laid into its beam") {
        SceneFixture scene(UVec2(128, 128));
        // Black on the left half, white on the right.
        Image cookie = unwrap(Image::create(64, 64, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < cookie.height(); ++y) {
            for (uint32_t x = 0; x < cookie.width(); ++x) {
                cookie.setPixel(x, y, x < cookie.width() / 2 ? Vec4(0.0f, 0.0f, 0.0f, 1.0f) : Vec4(1.0f));
            }
        }
        scene.writeImage("halves", cookie);
        // Stripes a pixel wide: much finer than a box of the grid where the beam is.
        for (uint32_t y = 0; y < cookie.height(); ++y) {
            for (uint32_t x = 0; x < cookie.width(); ++x) {
                cookie.setPixel(x, y, x % 2 == 0 ? Vec4(0.0f, 0.0f, 0.0f, 1.0f) : Vec4(1.0f));
            }
        }
        scene.writeImage("stripes", cookie);
        scene.open();
        addLitFog(scene, evenLitFog(0.15f, 20.0f));
        LightComponent spot{.type = LightType::Spot,
                            .color = Vec3(1.0f),
                            .intensity = 30.0f,
                            .range = 6.0f,
                            .innerConeDegrees = 25.0f,
                            .outerConeDegrees = 30.0f,
                            .castsShadows = false,
                            .volumetricIntensity = 0.0f};
        const Entity entity = scene.addLight(spot, Vec3(0.0f, 1.4f, -5.0f));
        // Turned to shine straight down with its right the world's +X, so that the halves of
        // the cookie are left and right in the picture - pointed down by a look at, its right
        // would be any way round.
        scene.world.transform(entity).rotation = glm::angleAxis(-kHalfPi, Vec3(1.0f, 0.0f, 0.0f));
        const auto added = [&](const Uuid& cookieId) {
            LightComponent& light = *scene.world.tryGet<LightComponent>(entity);
            light.cookie = cookieId;
            light.volumetricIntensity = 0.0f;
            scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
            const HalfPicture before = sceneLight(scene);
            light.volumetricIntensity = 1.0f;
            scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
            const HalfPicture after = sceneLight(scene);
            return std::array<float, 2>{after.at(32, 85).x - before.at(32, 85).x,
                                        after.at(95, 85).x - before.at(95, 85).x};
        };
        const std::array<float, 2> open = added(Uuid{});
        const std::array<float, 2> filtered = added(scene.asset("halves.png"));
        CAPTURE(open[0]);
        CAPTURE(open[1]);
        CAPTURE(filtered[0]);
        CAPTURE(filtered[1]);
        REQUIRE(open[0] > 0.02f);
        // One side of the beam dark, the other as it was.
        const float darker = std::min(filtered[0], filtered[1]);
        const float lighter = std::max(filtered[0], filtered[1]);
        CHECK(darker < 0.15f * lighter);
        CHECK(std::abs(lighter - open[0]) < 0.1f * open[0]);

        // A box reads the cookie at the level of its own size: stripes much finer than it
        // are grey to every box across the beam - half the light - not one stripe or the
        // next. Looked at in what the first pass works out, in the slice of the beam's
        // middle, ten metres from the camera, and the row of boxes half a metre below it.
        const FogVolumeGrid grid = fogVolumeGridFor(UVec2(128), ScreenEffectsQuality::High);
        const auto scatteringRow = [&](const Uuid& cookieId) {
            scene.world.tryGet<LightComponent>(entity)->cookie = cookieId;
            scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
            const auto slice = static_cast<uint32_t>(fogSliceShare(10.0f, 20.0f) * static_cast<float>(grid.slices));
            const HalfPicture texels{
                unwrap(readTexture(*scene.context.graphics, scene.targets.fogScattering(), 0, slice))};
            std::vector<float> row;
            for (uint32_t column = 0; column < grid.columns; ++column) {
                row.push_back(texels.at(column, 10).x);
            }
            return row;
        };
        const std::vector<float> openRow = scatteringRow(Uuid{});
        const std::vector<float> greyRow = scatteringRow(scene.asset("stripes.png"));
        const float brightest = *std::max_element(openRow.begin(), openRow.end());
        uint32_t inBeam = 0;
        for (size_t column = 0; column < openRow.size(); ++column) {
            if (openRow[column] > 0.2f * brightest) {
                CAPTURE(column);
                CHECK(std::abs(greyRow[column] / openRow[column] - 0.5f) < 0.03f);
                ++inBeam;
            }
        }
        CHECK(inBeam >= 6);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a fog no light lights, a light that gives it nothing and a view without screen effects draw the fog as "
              "before, to the bit") {
        SceneFixture scene;
        Walls::write(scene);
        scene.open();
        Walls::place(scene);
        const Entity fog = addLitFog(scene, evenLitFog(0.04f, 40.0f));
        scene.world.tryGet<FogComponent>(fog)->volumetric = false;
        const Entity sun = scene.addLight(sunComponent(), Vec3(0.0f), kSunDirection);
        const Entity point =
            scene.addLight({.type = LightType::Point, .intensity = 3.0f, .range = 5.0f, .castsShadows = false},
                           Vec3(0.0f, 0.0f, -4.0f));
        const auto giveFog = [&](float intensity) {
            scene.world.tryGet<LightComponent>(sun)->volumetricIntensity = intensity;
            scene.world.tryGet<LightComponent>(point)->volumetricIntensity = intensity;
        };
        const auto drawn = [&](ScreenEffectsQuality quality) {
            scene.render(frontView(scene), litFogSettings(quality));
            return sceneLight(scene).pixels.data;
        };

        const std::vector<std::byte> plain = drawn(ScreenEffectsQuality::Medium);
        const uint32_t plainPasses = scene.renderer->statistics().screenPasses;
        CHECK(scene.renderer->statistics().fogVolumeBytes == 0);
        CHECK(scene.targets.fogVolume() == nullptr);

        // Lit by lights, but by none that gives it anything.
        scene.world.tryGet<FogComponent>(fog)->volumetric = true;
        giveFog(0.0f);
        CHECK(drawn(ScreenEffectsQuality::Medium) == plain);
        CHECK(scene.renderer->statistics().fogVolumeBytes == 0);
        CHECK(scene.renderer->statistics().screenPasses == plainPasses);

        // Given something, in a view whose quality has no screen effects.
        giveFog(1.0f);
        CHECK(drawn(ScreenEffectsQuality::Off) == plain);
        CHECK(scene.renderer->statistics().fogVolumeBytes == 0);

        // And then in one that has them: two passes more, and the volumes of the grid.
        CHECK_FALSE(drawn(ScreenEffectsQuality::Medium) == plain);
        CHECK(scene.renderer->statistics().screenPasses == plainPasses + 2);
        CHECK(scene.renderer->statistics().fogVolumeBytes == uint64_t{6} * 6 * 64 * 8 * 2);
        // As fine as the quality says.
        scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::Low));
        CHECK(scene.renderer->statistics().fogVolumeBytes == uint64_t{4} * 4 * 48 * 8 * 2);
        scene.render(frontView(scene), litFogSettings(ScreenEffectsQuality::High));
        CHECK(scene.renderer->statistics().fogVolumeBytes == uint64_t{8} * 8 * 80 * 8 * 2);
        // A debug view that shows no light lets go of them.
        SceneRenderSettings normals = litFogSettings();
        normals.debugView = RenderDebugView::Normals;
        scene.render(frontView(scene), normals);
        CHECK(scene.targets.fogVolume() == nullptr);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the volumes of a view follow the depth of its grid, also where its columns stay") {
        // A picture so small that it is one box across at every quality: only the slices
        // change with it.
        SceneFixture scene(UVec2(8, 8));
        scene.open();
        addLitFog(scene, evenLitFog(0.05f, 20.0f));
        scene.addLight(sunComponent(), Vec3(0.0f), kSunDirection);
        for (const ScreenEffectsQuality quality :
             {ScreenEffectsQuality::Low, ScreenEffectsQuality::High, ScreenEffectsQuality::Medium}) {
            CAPTURE(nameOf(quality));
            const FogVolumeGrid grid = fogVolumeGridFor(UVec2(8), quality);
            REQUIRE(grid.columns == 1);
            REQUIRE(grid.rows == 1);
            scene.render(frontView(scene), litFogSettings(quality));
            REQUIRE(scene.targets.fogVolume() != nullptr);
            CHECK(scene.targets.fogVolume()->getDesc().depth == grid.slices);
            CHECK(scene.targets.fogScattering()->getDesc().depth == grid.slices);
            CHECK(scene.renderer->statistics().fogVolumeBytes == uint64_t{grid.slices} * 8 * 2);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the fog in front of a translucent surface is lit as the fog in front of a solid one") {
        SceneFixture scene;
        scene.writeFlatMaterial("solid", Vec3(0.0f));
        scene.writeFlatMaterial("glass", Vec3(0.0f), R"("alphaMode": "blended")");
        scene.open();
        addLitFog(scene, evenLitFog(0.1f, 20.0f));
        // Two panes side by side, alike but for how they are drawn, and a light before
        // the middle of them.
        scene.add(BuiltinMesh::Cube, scene.material("solid"), at(Vec3(-0.75f, 0.0f, -6.0f), Vec3(1.5f, 3.0f, 0.1f)));
        scene.add(BuiltinMesh::Cube, scene.material("glass"), at(Vec3(0.75f, 0.0f, -6.0f), Vec3(1.5f, 3.0f, 0.1f)));
        scene.addLight({.type = LightType::Point, .intensity = 2.0f, .range = 5.0f, .castsShadows = false},
                       Vec3(0.0f, 0.3f, -3.0f));
        scene.render(frontView(scene), litFogSettings());
        const HalfPicture light = sceneLight(scene);
        uint32_t lit = 0;
        for (const uint32_t y : {10u, 20u, 28u, 40u, 54u}) {
            for (const uint32_t x : {4u, 12u, 20u, 28u}) {
                CAPTURE(x);
                CAPTURE(y);
                // The pixel across the middle of the picture: the solid pane's, mirrored.
                const Vec3 solid = light.at(x, y);
                checkLight(light.at(63 - x, y), solid);
                lit += solid.x > 0.02f ? 1 : 0;
            }
        }
        CHECK(lit > 4);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
