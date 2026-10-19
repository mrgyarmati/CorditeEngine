#include "cordite/assets/ImageCompare.h"
#include "cordite/math/Color.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Random.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/RectLights.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::checkShown;
using cordite::test::displayed;
using cordite::test::lightOn;
using cordite::test::LogCapture;
using cordite::test::PlacedLight;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The wall the lights of these tests shine on is in the plane z = 0 and faces +Z. A
/// camera without perspective looks at it head-on; the picture is 64 pixels a side and
/// shows four metres of the wall, so a pixel is a sixteenth of a metre.
constexpr float kWallView = 4.0f;
constexpr uint32_t kPictureSize = 64;
constexpr Vec3 kWallNormal(0.0f, 0.0f, 1.0f);

/// Where the centre of a pixel of the picture is on the wall.
Vec3 onWall(uint32_t x, uint32_t y) {
    const float metresPerPixel = kWallView / static_cast<float>(kPictureSize);
    const float half = 0.5f * static_cast<float>(kPictureSize);
    return {(static_cast<float>(x) + 0.5f - half) * metresPerPixel,
            (half - (static_cast<float>(y) + 0.5f)) * metresPerPixel, 0.0f};
}

/// A white, matte wall that fills the view of a camera looking at it head-on.
struct WallScene {
    WallScene() {
        scene.writeMatteMaterial("matte");
        scene.open();
        // The built-in plane lies flat; stood up, it faces +Z.
        wall = scene.add(BuiltinMesh::Plane, scene.material("matte"),
                         at(Vec3(0.0f), Vec3(8.0f), glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f))));
        view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), kWallView);
    }

    Entity add(const PlacedLight& placed) { return scene.addLight(placed.light, placed.position, placed.direction); }

    Image render(const SceneRenderSettings& settings = SceneFixture::flatSettings()) {
        return scene.render(view, settings);
    }

    /// Checks pixels all over the picture against what `lights` must light there.
    void checkLit(const Image& image, const std::vector<PlacedLight>& lights) const {
        for (uint32_t y = 2; y < kPictureSize; y += 6) {
            for (uint32_t x = 2; x < kPictureSize; x += 6) {
                Vec3 expected(0.0f);
                for (const PlacedLight& placed : lights) {
                    expected += lightOn(onWall(x, y), kWallNormal, placed);
                }
                checkPixel(image, x, y, expected);
            }
        }
    }

    SceneFixture scene{UVec2(kPictureSize, kPictureSize)};
    Entity wall;
    CameraView view;
};

/// The linear brightness of a pixel of a picture that is grey there.
float brightnessAt(const Image& image, uint32_t x, uint32_t y) {
    const Vec4 pixel = image.pixel(x, y);
    CHECK(pixel.r == doctest::Approx(pixel.g).epsilon(0.02));
    CHECK(pixel.r == doctest::Approx(pixel.b).epsilon(0.02));
    return srgbToLinear(pixel.r);
}

/// Where the line of sight through `pixel` - in pixels from the top left - of a view of
/// kPictureSize pixels a side meets the wall, z = 0; nothing where it passes the wall by.
std::optional<Vec3> wallSeenAt(const CameraView& view, const Vec2& pixel) {
    const auto size = static_cast<float>(kPictureSize);
    const Vec2 clip(pixel.x / size * 2.0f - 1.0f, 1.0f - pixel.y / size * 2.0f);
    // Depth is reversed: 1 at the near plane.
    const Vec4 near = view.inverseViewProjection * Vec4(clip, 1.0f, 1.0f);
    const Vec4 deeper = view.inverseViewProjection * Vec4(clip, 0.5f, 1.0f);
    const Vec3 from = Vec3(near) / near.w;
    const Vec3 to = Vec3(deeper) / deeper.w;
    if (!(from.z > to.z)) {
        return std::nullopt;
    }
    const Vec3 point = glm::mix(from, to, from.z / (from.z - to.z));
    // The wall is eight metres a side.
    if (std::max(std::abs(point.x), std::abs(point.y)) > 3.9f) {
        return std::nullopt;
    }
    return point;
}

/// A material as the lighting sees it: what it scatters, what it reflects head-on and at
/// grazing angles, and how rough it is (Brdf.slang).
struct Reflectances {
    Vec3 diffuse{0.0f};
    Vec3 f0{0.0f};
    float roughness = 1.0f;
};

Reflectances reflectancesOf(const Vec3& baseColor, float metallic, float specular, float roughness) {
    return {.diffuse = baseColor * (1.0f - metallic),
            .f0 = glm::mix(Vec3(0.08f * specular), baseColor, metallic),
            .roughness = roughness};
}

/// What a surface of `material` at `point` that faces `normal` sends towards a viewer along
/// `toViewer` of the rect light of `component`, placed by `placement`: the arithmetic of
/// Lights.slang, written with the functions of RectLights.h.
Vec3 rectLightShading(const Vec3& point, const Vec3& normal, const Vec3& toViewer, const Reflectances& material,
                      const LightComponent& component, const Mat4& placement) {
    const SceneLight light = makeSceneLight(component, placement);
    const RectLightShape shape = rectShapeOf(light);
    const float distanceSquared = glm::dot(shape.center - point, shape.center - point);
    if (distanceSquared >= light.range * light.range) {
        return Vec3(0.0f);
    }
    const float ratio = distanceSquared / (light.range * light.range);
    const float window = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
    const float area = 4.0f * glm::length(shape.right) * glm::length(shape.up);
    const Vec3 radiance = light.color * (window / area);

    const RectLightIrradiance matte = rectLightIrradiance(shape, point, normal);
    const RectLightSpecular shine = rectLightSpecular(shape, point, normal, toViewer, material.roughness);
    const float f90 = std::clamp(50.0f * material.f0.g, 0.0f, 1.0f);
    const Vec3 halfway = normalizeOr(matte.direction + toViewer, normal);
    const float grazing = std::pow(1.0f - std::clamp(glm::dot(toViewer, halfway), 0.0f, 1.0f), 5.0f);
    const Vec3 fresnel = material.f0 + (Vec3(f90) - material.f0) * grazing;
    const Vec3 diffuse = (Vec3(1.0f) - fresnel) * material.diffuse * matte.formFactor;
    const Vec3 specular = material.f0 * (shine.lobe - shine.grazing) + Vec3(f90 * shine.grazing);
    return (diffuse + specular) * radiance;
}

} // namespace

TEST_SUITE("SceneLighting") {
    TEST_CASE("a point light of intensity one lights a white wall a metre away to white, and less with the square "
              "of the distance") {
        WallScene wall;
        PlacedLight lamp{.light = {.type = LightType::Point, .intensity = 1.0f, .range = 100.0f},
                         .position = Vec3(0.0f, 0.0f, 1.0f)};
        const Entity entity = wall.add(lamp);

        // Straight in front of the light, one metre away: white.
        Image image = wall.render();
        CHECK(brightnessAt(image, 32, 32) == doctest::Approx(1.0f).epsilon(0.02));
        // A metre to the side the light has 1.41 metres to go and arrives at 45 degrees:
        // half as bright for the distance, and 0.71 of that for the angle.
        CHECK(brightnessAt(image, 48, 32) == doctest::Approx(0.5f * std::sqrt(0.5f)).epsilon(0.05));

        // Twice as far away: a quarter. Three times: a ninth.
        wall.scene.world.transform(entity).position = Vec3(0.0f, 0.0f, 2.0f);
        CHECK(brightnessAt(wall.render(), 32, 32) == doctest::Approx(0.25f).epsilon(0.02));
        wall.scene.world.transform(entity).position = Vec3(0.0f, 0.0f, 3.0f);
        CHECK(brightnessAt(wall.render(), 32, 32) == doctest::Approx(1.0f / 9.0f).epsilon(0.02));

        // Brightness is intensity times color, and all of the picture is as the model says.
        lamp.light.intensity = 0.8f;
        lamp.light.color = Vec3(1.0f, 0.5f, 0.25f);
        lamp.position = Vec3(0.4f, -0.3f, 1.5f);
        wall.scene.world.transform(entity).position = lamp.position;
        *wall.scene.world.tryGet<LightComponent>(entity) = lamp.light;
        image = wall.render();
        wall.checkLit(image, {lamp});
        const Vec4 nearest = image.pixel(38, 36);
        CHECK(srgbToLinear(nearest.g) == doctest::Approx(0.5f * srgbToLinear(nearest.r)).epsilon(0.03));
        CHECK(srgbToLinear(nearest.b) == doctest::Approx(0.25f * srgbToLinear(nearest.r)).epsilon(0.05));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a point light fades to nothing at its range and lights nothing beyond it") {
        WallScene wall;
        // One metre from the wall and reaching two: a disc of 1.73 metres on the wall.
        PlacedLight lamp{.light = {.type = LightType::Point, .intensity = 1.0f, .range = 2.0f},
                         .position = Vec3(0.0f, 0.0f, 1.0f)};
        const Entity entity = wall.add(lamp);
        Image image = wall.render();

        // At half its range the light has lost a little to the fade: (1 - (1/2)^4)^2.
        CHECK(brightnessAt(image, 32, 32) == doctest::Approx(0.8789f).epsilon(0.02));
        wall.checkLit(image, {lamp});
        // Beyond the range there is no light at all, not a little.
        checkPixel(image, 62, 32, Vec3(0.0f));
        checkPixel(image, 1, 32, Vec3(0.0f));
        checkPixel(image, 2, 2, Vec3(0.0f));
        checkPixel(image, 61, 61, Vec3(0.0f));

        // However bright the light: the range is where it ends.
        wall.scene.world.tryGet<LightComponent>(entity)->intensity = 1000.0f;
        image = wall.render();
        checkPixel(image, 32, 32, Vec3(1.0f));
        checkPixel(image, 62, 32, Vec3(0.0f));

        // A light that does not reach the wall lights nothing of it.
        wall.scene.world.tryGet<LightComponent>(entity)->range = 0.9f;
        image = wall.render();
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a spot light is at full brightness inside its inner cone and dark outside its outer one") {
        WallScene wall;
        // Two metres from the wall, shining straight at it: the inner cone is a disc of
        // 0.54 metres on the wall, the outer one of 1.15.
        PlacedLight spot{.light = {.type = LightType::Spot,
                                   .intensity = 4.0f,
                                   .range = 100.0f,
                                   .innerConeDegrees = 15.0f,
                                   .outerConeDegrees = 30.0f},
                         .position = Vec3(0.0f, 0.0f, 2.0f),
                         .direction = Vec3(0.0f, 0.0f, -1.0f)};
        const Entity entity = wall.add(spot);
        Image image = wall.render();

        // In the middle, as bright as a point light would be: intensity over distance squared.
        CHECK(brightnessAt(image, 32, 32) == doctest::Approx(1.0f).epsilon(0.02));
        // Inside the inner cone the cone takes nothing away.
        PlacedLight point = spot;
        point.light.type = LightType::Point;
        checkPixel(image, 36, 32, lightOn(onWall(36, 32), kWallNormal, point));
        checkPixel(image, 32, 26, lightOn(onWall(32, 26), kWallNormal, point));
        // Between the cones it fades, and all of the picture is as the model says.
        const float between = brightnessAt(image, 46, 32);
        CHECK(between > 0.02f);
        CHECK(between < 0.5f * lightOn(onWall(46, 32), kWallNormal, point).r);
        wall.checkLit(image, {spot});
        // Outside the outer cone there is no light at all.
        checkPixel(image, 54, 32, Vec3(0.0f));
        checkPixel(image, 32, 8, Vec3(0.0f));
        checkPixel(image, 3, 3, Vec3(0.0f));

        // Cones of the same angle have a hard edge, and no trouble dividing by nothing.
        LightComponent& light = *wall.scene.world.tryGet<LightComponent>(entity);
        light.innerConeDegrees = 20.0f;
        light.outerConeDegrees = 20.0f;
        image = wall.render();
        // The edge is 0.73 metres from the middle: pixel 43 is inside, pixel 45 outside.
        checkPixel(image, 43, 32, lightOn(onWall(43, 32), kWallNormal, point));
        checkPixel(image, 45, 32, Vec3(0.0f));

        // An inner cone wider than the outer one counts as being as wide: the same picture.
        light.innerConeDegrees = 60.0f;
        const Image swapped = wall.render();
        CHECK(unwrap(compareImages(image, swapped, {.channelTolerance = 1})).differingPixels == 0);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a spot light shines along the -Z axis of its entity, wherever a parent turns it") {
        WallScene wall;
        const PlacedLight spot{.light = {.type = LightType::Spot,
                                         .intensity = 4.0f,
                                         .range = 100.0f,
                                         .innerConeDegrees = 10.0f,
                                         .outerConeDegrees = 14.0f},
                               .position = Vec3(0.0f, 0.0f, 2.0f)};
        const Entity entity = wall.add(spot);
        World& world = wall.scene.world;

        // The spot is about a metre wide on the wall. Aimed at a point a metre to the
        // right, it lights that point and leaves the middle and the left dark.
        const auto lit = [&](uint32_t x, uint32_t y) { return wall.render().pixel(x, y).r > 0.3f; };
        world.transform(entity).rotation = lookRotation(Vec3(1.0f, 0.0f, 0.0f) - spot.position);
        CHECK(lit(48, 32));
        CHECK_FALSE(lit(32, 32));
        CHECK_FALSE(lit(16, 32));

        // Aimed up instead.
        world.transform(entity).rotation = lookRotation(Vec3(0.0f, 1.0f, 0.0f) - spot.position);
        CHECK(lit(32, 16));
        CHECK_FALSE(lit(32, 32));
        CHECK_FALSE(lit(48, 32));

        // Facing away from the wall, it lights none of it.
        world.transform(entity).rotation = lookRotation(Vec3(0.0f, 0.0f, 1.0f));
        checkPixel(wall.render(), 32, 32, Vec3(0.0f));

        // A parent that turns takes the light with it. The light sits two metres along
        // the -Z axis of its mount and looks on along it. A mount turned a quarter turn
        // to the left makes that the -X axis: the light ends up at (-3, 0, 0), in the
        // plane of the wall, shining away along it.
        const Entity mount = world.createEntity("Mount");
        REQUIRE(world.setParent(entity, mount).has_value());
        world.transform(entity) = {.position = Vec3(0.0f, 0.0f, -2.0f)};
        world.transform(mount) = {.position = Vec3(-1.0f, 0.0f, 0.0f),
                                  .rotation = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f))};
        checkPixel(wall.render(), 32, 32, Vec3(0.0f));
        checkPixel(wall.render(), 4, 32, Vec3(0.0f));

        // A mount in front of the wall's right half, turned an eighth of a turn to the
        // left: the light is carried to (0.59, 0, 1.09) and shines to the left and at
        // the wall, which it meets half a metre left of the middle.
        world.transform(mount) = {.position = Vec3(2.0f, 0.0f, 2.5f),
                                  .rotation = glm::angleAxis(toRadians(45.0f), Vec3(0.0f, 1.0f, 0.0f))};
        const Image image = wall.render();
        CHECK(image.pixel(24, 32).r > 0.5f);
        CHECK(image.pixel(52, 32).r < 0.05f);
        // All of the picture is what the model makes of a light placed like that.
        const Mat4 placement = world.worldMatrix(entity);
        PlacedLight placed = spot;
        placed.position = Vec3(placement[3]);
        placed.direction = glm::normalize(-Vec3(placement[2]));
        CHECK_NEAR(placed.position, Vec3(2.0f - std::sqrt(2.0f), 0.0f, 2.5f - std::sqrt(2.0f)), 1.0e-4f);
        CHECK_NEAR(placed.direction, Vec3(-std::sqrt(0.5f), 0.0f, -std::sqrt(0.5f)), 1.0e-4f);
        wall.checkLit(image, {placed});
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a rect light lights a white wall with all of its face, and from far away as a point light does") {
        WallScene wall;
        // A metre wide, half a metre high, 0.8 metres from the wall and facing it, aside:
        // what it lights is as wide and as high, which the whole picture is held to.
        PlacedLight panel{
            .light = {.type = LightType::Rect, .intensity = 0.5f, .range = 100.0f, .width = 1.0f, .height = 0.5f},
            .position = Vec3(0.3f, -0.2f, 0.8f)};
        const Entity entity = wall.add(panel);
        World& world = wall.scene.world;
        Image image = wall.render();
        wall.checkLit(image, {panel});
        // Close to the face the light comes from all over it, much of it at a slant: the
        // wall in front of the middle gets less than of a point light there, 0.5 / 0.64.
        const float ahead = brightnessAt(image, 37, 35);
        CHECK(ahead > 0.5f);
        CHECK(ahead < 0.5f / 0.64f);

        // Reaching a metre from its middle, it fades to nothing on a disc of 0.6 metres of
        // the wall, and lights nothing beyond.
        panel.light.range = 1.0f;
        world.tryGet<LightComponent>(entity)->range = 1.0f;
        image = wall.render();
        wall.checkLit(image, {panel});
        checkPixel(image, 37 + 12, 35, Vec3(0.0f));
        checkPixel(image, 37, 35 + 12, Vec3(0.0f));
        CHECK(brightnessAt(image, 37, 35) < 0.75f * ahead);

        // Smaller and further, colored, at a slant: as the model says.
        panel.light.width = 0.3f;
        panel.light.height = 0.6f;
        panel.light.color = Vec3(1.0f, 0.6f, 0.3f);
        panel.light.intensity = 3.0f;
        panel.position = Vec3(-0.5f, 0.4f, 1.5f);
        panel.direction = glm::normalize(Vec3(0.3f, -0.4f, -1.0f));
        *world.tryGet<LightComponent>(entity) = panel.light;
        world.transform(entity) = {.position = panel.position, .rotation = lookRotation(panel.direction)};
        wall.checkLit(wall.render(), {panel});

        // Five centimetres a side, two metres away: the point light of intensity one there,
        // a quarter, within what the 8 bits of the picture tell apart.
        panel.light = {.type = LightType::Rect, .intensity = 1.0f, .range = 100.0f, .width = 0.05f, .height = 0.05f};
        *world.tryGet<LightComponent>(entity) = panel.light;
        world.transform(entity) = {.position = Vec3(0.0f, 0.0f, 2.0f)};
        CHECK(std::abs(brightnessAt(wall.render(), 32, 32) / 0.25f - 1.0f) < 0.02f);

        // Turned away from the wall, it lights none of it.
        world.transform(entity).rotation = lookRotation(Vec3(0.0f, 0.0f, 1.0f));
        image = wall.render();
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkPixel(image, 4, 60, Vec3(0.0f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a rect light that reaches behind the wall lights it with what of its face is in front") {
        WallScene wall;
        // Turned to shine at the wall at forty-five degrees, its left edge behind the wall:
        // the face goes through the wall where x = -0.15, between the pixels that are
        // checked. To the right the wall sees what of the face is in front of it, at a
        // slant; to the left it is behind the face.
        PlacedLight panel{
            .light = {.type = LightType::Rect, .intensity = 2.0f, .range = 100.0f, .width = 1.0f, .height = 0.6f},
            .position = Vec3(0.0f, 0.1f, 0.15f),
            .direction = glm::normalize(Vec3(1.0f, 0.0f, -1.0f))};
        const Entity entity = wall.add(panel);
        Image image = wall.render();
        wall.checkLit(image, {panel});
        CHECK(brightnessAt(image, 44, 30) > 0.05f);
        checkPixel(image, 20, 30, Vec3(0.0f));
        checkPixel(image, 2, 2, Vec3(0.0f));

        // With its middle behind the wall, what of the face is in front still lights it -
        // where the wall faces away from the middle too. It goes through the wall where
        // x = 0.22 now.
        panel.position = Vec3(0.17f, 0.1f, -0.05f);
        wall.scene.world.transform(entity).position = panel.position;
        image = wall.render();
        wall.checkLit(image, {panel});
        CHECK(brightnessAt(image, 44, 30) > 0.05f);
        checkPixel(image, 30, 30, Vec3(0.0f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a shiny wall reflects a rect light as the fit of its lobe says, at a slant and at a grazing angle") {
        WallScene wall;
        SceneFixture& scene = wall.scene;
        scene.writeMaterial("glossy", R"("baseColor": [0.6, 0.6, 0.6, 1], "roughness": 0.3, "specular": 0.5)");
        scene.writeMaterial("copper", R"("baseColor": [0.95, 0.64, 0.54, 1], "roughness": 0.55, "metallic": 1)");
        // Half a metal: it reflects much head-on and scatters much, and what it scatters is
        // what Fresnel leaves at the way the light of the face arrives.
        scene.writeMaterial("alloy", R"("baseColor": [0.8, 0.8, 0.8, 1], "roughness": 0.4, "metallic": 0.5)");
        REQUIRE(scene.assets->refresh().has_value());
        const std::vector<std::pair<std::string, Reflectances>> materials = {
            {"glossy", reflectancesOf(Vec3(0.6f), 0.0f, 0.5f, 0.3f)},
            {"copper", reflectancesOf(Vec3(0.95f, 0.64f, 0.54f), 1.0f, 0.5f, 0.55f)},
            {"alloy", reflectancesOf(Vec3(0.8f), 0.5f, 0.5f, 0.4f)}};
        const LightComponent component{.type = LightType::Rect,
                                       .color = Vec3(1.0f, 0.95f, 0.9f),
                                       .intensity = 0.8f,
                                       .range = 8.0f,
                                       .width = 0.8f,
                                       .height = 0.4f};
        const Entity entity = scene.addLight(component, Vec3(0.0f, 0.0f, 1.0f));

        // With perspective, so that every pixel sees the wall another way.
        struct Look {
            std::string name;
            Vec3 eye;
            Vec3 target;
            Vec3 light;
            Vec3 shines;
        };

        const std::vector<Look> looks = {
            // At a slant, the light in front of the wall.
            {"slant", Vec3(0.5f, 0.7f, 2.2f), Vec3(0.0f, -0.1f, 0.0f), Vec3(-0.3f, -0.2f, 1.0f),
             Vec3(0.2f, 0.1f, -1.0f)},
            // The light through the wall at forty-five degrees: what the lobe reflects of it
            // is cut at the wall's horizon, and again at the cosine's.
            {"through", Vec3(0.5f, 0.7f, 2.2f), Vec3(0.0f, -0.1f, 0.0f), Vec3(0.0f, 0.1f, 0.15f),
             Vec3(1.0f, 0.0f, -1.0f)},
            // Along the wall, eighty degrees from its normal, the light where the wall
            // mirrors the camera: Schlick's factor gives most of what is reflected there to
            // the reflectance at grazing angles.
            {"grazing", Vec3(1.6f, 0.3f, 0.45f), Vec3(-0.8f, 0.0f, 0.0f), Vec3(-1.8f, -0.1f, 0.3f),
             Vec3(1.0f, 0.1f, -0.25f)},
        };
        // Where a pixel is seen at a grazing angle, the model is asked a little around its
        // middle: where in the pixel the GPU takes its sample is not exactly there.
        constexpr std::array<Vec2, 5> kSamples = {Vec2(0.0f, 0.0f), Vec2(0.125f, 0.0f), Vec2(-0.125f, 0.0f),
                                                  Vec2(0.0f, 0.125f), Vec2(0.0f, -0.125f)};
        for (const Look& look : looks) {
            CAPTURE(look.name);
            scene.world.transform(entity) = {.position = look.light, .rotation = lookRotation(look.shines)};
            scene.world.updateWorldTransforms();
            const Mat4 placement = scene.world.worldMatrix(entity);
            const CameraView view = scene.perspectiveView(look.eye, look.target);
            for (const std::pair<std::string, Reflectances>& material : materials) {
                CAPTURE(material.first);
                scene.world.tryGet<MeshRendererComponent>(wall.wall)->materials = {scene.material(material.first)};
                const Image image = scene.render(view);
                uint32_t telling = 0;
                for (uint32_t y = 1; y < kPictureSize; y += 3) {
                    for (uint32_t x = 1; x < kPictureSize; x += 3) {
                        Vec3 least(1.0f);
                        Vec3 most(0.0f);
                        bool onWall = true;
                        for (const Vec2& offset : kSamples) {
                            const Vec2 pixel = Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f + offset;
                            const std::optional<Vec3> point = wallSeenAt(view, pixel);
                            if (!point) {
                                onWall = false;
                                break;
                            }
                            const Vec3 toViewer = glm::normalize(view.position - *point);
                            const Vec3 shown = displayed(
                                rectLightShading(*point, kWallNormal, toViewer, material.second, component, placement));
                            least = glm::min(least, shown);
                            most = glm::max(most, shown);
                        }
                        if (!onWall) {
                            continue;
                        }
                        CAPTURE(x);
                        CAPTURE(y);
                        const Vec3 actual(image.pixel(x, y));
                        CHECK_NEAR(glm::clamp(actual, least, most), actual, 2.5f / 255.0f);
                        const float brightest = std::max({most.r, most.g, most.b});
                        telling += brightest > 0.1f && brightest < 0.95f ? 1 : 0;
                    }
                }
                // Enough pixels are neither black nor white to say what the light did.
                CHECK(telling > 30);
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a directional light is the same everywhere, and only the way it is turned counts") {
        WallScene wall;
        PlacedLight sun{.light = {.type = LightType::Directional, .color = Vec3(1.0f, 0.8f, 0.6f), .intensity = 0.9f},
                        .position = Vec3(0.0f, 0.0f, 3.0f),
                        .direction = Vec3(0.0f, 0.0f, -1.0f)};
        const Entity entity = wall.add(sun);

        // Facing the wall: the color of the light times its intensity, on every pixel.
        Image image = wall.render();
        for (const uint32_t x : std::initializer_list<uint32_t>{1, 32, 62}) {
            checkPixel(image, x, 32, Vec3(0.9f, 0.72f, 0.54f));
            checkPixel(image, 32, x, Vec3(0.9f, 0.72f, 0.54f));
        }

        // Where the light is makes no difference, not even behind the wall or far away;
        // its range and its cone angles mean nothing to it.
        wall.scene.world.transform(entity).position = Vec3(900.0f, -40.0f, -70.0f);
        LightComponent& light = *wall.scene.world.tryGet<LightComponent>(entity);
        light.range = 0.01f;
        light.innerConeDegrees = 0.0f;
        light.outerConeDegrees = 0.0f;
        image = wall.render();
        checkPixel(image, 32, 32, Vec3(0.9f, 0.72f, 0.54f));
        checkPixel(image, 3, 60, Vec3(0.9f, 0.72f, 0.54f));

        // Arriving at 60 degrees from the wall's normal, it is half as bright.
        sun.direction = glm::normalize(Vec3(std::sin(toRadians(60.0f)), 0.0f, -std::cos(toRadians(60.0f))));
        wall.scene.world.transform(entity).rotation = lookRotation(sun.direction);
        image = wall.render();
        checkPixel(image, 32, 32, Vec3(0.45f, 0.36f, 0.27f));
        wall.checkLit(image, {sun});

        // From behind the wall it lights nothing of its front.
        wall.scene.world.transform(entity).rotation = lookRotation(Vec3(0.0f, 0.0f, 1.0f));
        checkPixel(wall.render(), 32, 32, Vec3(0.0f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("lights add up, and a light that is switched off adds nothing") {
        WallScene wall;
        const std::vector<PlacedLight> lights = {
            {.light = {.type = LightType::Point, .color = Vec3(1.0f, 0.2f, 0.2f), .intensity = 0.5f, .range = 3.0f},
             .position = Vec3(-0.8f, 0.3f, 0.8f)},
            {.light = {.type = LightType::Point, .color = Vec3(0.2f, 0.2f, 1.0f), .intensity = 0.6f, .range = 2.5f},
             .position = Vec3(0.9f, -0.2f, 1.1f)},
            {.light = {.type = LightType::Spot,
                       .color = Vec3(0.3f, 1.0f, 0.3f),
                       .intensity = 1.5f,
                       .range = 8.0f,
                       .innerConeDegrees = 12.0f,
                       .outerConeDegrees = 25.0f},
             .position = Vec3(0.0f, 1.5f, 2.0f),
             .direction = glm::normalize(Vec3(0.1f, -0.6f, -1.0f))},
            {.light = {.type = LightType::Directional, .color = Vec3(1.0f), .intensity = 0.1f},
             .direction = glm::normalize(Vec3(0.3f, -0.2f, -1.0f))},
        };
        std::vector<Entity> entities;
        for (const PlacedLight& placed : lights) {
            entities.push_back(wall.add(placed));
        }
        wall.checkLit(wall.render(), lights);
        CHECK(wall.scene.renderer->statistics().lights == 4);

        // Without the spot light: by its entity being disabled, or the one above it.
        std::vector<PlacedLight> remaining = lights;
        remaining.erase(remaining.begin() + 2);
        wall.scene.world.setEnabled(entities[2], false);
        wall.checkLit(wall.render(), remaining);
        CHECK(wall.scene.renderer->statistics().lights == 3);

        wall.scene.world.setEnabled(entities[2], true);
        const Entity group = wall.scene.world.createEntity("Group");
        REQUIRE(wall.scene.world.setParent(entities[2], group).has_value());
        wall.scene.world.setEnabled(group, false);
        wall.checkLit(wall.render(), remaining);

        // A light without brightness is not even sent to the GPU.
        wall.scene.world.tryGet<LightComponent>(entities[0])->intensity = 0.0f;
        wall.scene.world.tryGet<LightComponent>(entities[1])->color = Vec3(0.0f);
        wall.checkLit(wall.render(), {lights[3]});
        CHECK(wall.scene.renderer->statistics().lights == 1);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("ambient light comes from the sky above and the ground below, and several add up") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.open();
        // A cube seen from the front, from above and from below: three of its faces.
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(2.0f)));
        const CameraView front = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const CameraView above =
            scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f));
        const CameraView below =
            scene.orthographicView(Vec3(0.0f, -5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, 1.0f));

        const Entity sky = scene.world.createEntity("Sky");
        scene.world.add<AmbientLightComponent>(sky) = {
            .skyColor = Vec3(0.2f, 0.4f, 0.8f), .groundColor = Vec3(0.4f, 0.2f, 0.0f), .intensity = 1.0f};
        // What faces up takes on the sky's color, what faces down the ground's, and what
        // faces sideways half of each.
        checkPixel(scene.render(above), 32, 32, Vec3(0.2f, 0.4f, 0.8f));
        checkPixel(scene.render(below), 32, 32, Vec3(0.4f, 0.2f, 0.0f));
        checkPixel(scene.render(front), 32, 32, Vec3(0.3f, 0.3f, 0.4f));

        // Its intensity scales both.
        scene.world.tryGet<AmbientLightComponent>(sky)->intensity = 0.5f;
        checkPixel(scene.render(above), 32, 32, Vec3(0.1f, 0.2f, 0.4f));
        checkPixel(scene.render(below), 32, 32, Vec3(0.2f, 0.1f, 0.0f));

        // A second one adds its light to that of the first ...
        const Entity second = scene.world.createEntity("Second sky");
        scene.world.add<AmbientLightComponent>(second) = {
            .skyColor = Vec3(0.3f, 0.2f, 0.1f), .groundColor = Vec3(0.0f, 0.1f, 0.5f), .intensity = 2.0f};
        checkPixel(scene.render(above), 32, 32, Vec3(0.7f, 0.6f, 0.6f));
        checkPixel(scene.render(below), 32, 32, Vec3(0.2f, 0.3f, 1.0f));
        // ... and is all there is when the first is switched off.
        scene.world.setEnabled(sky, false);
        checkPixel(scene.render(above), 32, 32, Vec3(0.6f, 0.4f, 0.2f));
        checkPixel(scene.render(below), 32, 32, Vec3(0.0f, 0.2f, 1.0f));

        // With none, there is no light from around at all.
        scene.world.setEnabled(second, false);
        checkPixel(scene.render(above), 32, 32, Vec3(0.0f));
        checkPixel(scene.render(front), 32, 32, Vec3(0.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a light shines only on what shares a lighting channel with it") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeMatteMaterial("sheer", Vec3(1.0f), R"("alphaMode": "blended")");
        scene.open();
        // Two halves of a wall with the same mesh and material - one instanced draw call -
        // that differ in nothing but their lighting channels.
        const Quat upright = glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f));
        const Entity left = scene.add(BuiltinMesh::Plane, scene.material("matte"),
                                      at(Vec3(-1.0f, 0.0f, 0.0f), Vec3(2.0f, 1.0f, 4.0f), upright));
        const Entity right = scene.add(BuiltinMesh::Plane, scene.material("matte"),
                                       at(Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f, 1.0f, 4.0f), upright));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), kWallView);
        World& world = scene.world;

        const Entity lamp = scene.addLight({.type = LightType::Directional}, Vec3(0.0f, 0.0f, 3.0f));
        const auto channels = [&](uint32_t ofLeft, uint32_t ofRight, uint32_t ofLight) {
            world.tryGet<MeshRendererComponent>(left)->lightChannels = ofLeft;
            world.tryGet<MeshRendererComponent>(right)->lightChannels = ofRight;
            world.tryGet<LightComponent>(lamp)->channels = ofLight;
        };
        const auto check = [&](bool leftIsLit, bool rightIsLit) {
            const Image image = scene.render(view);
            checkPixel(image, 16, 32, Vec3(leftIsLit ? 1.0f : 0.0f));
            checkPixel(image, 4, 60, Vec3(leftIsLit ? 1.0f : 0.0f));
            checkPixel(image, 48, 32, Vec3(rightIsLit ? 1.0f : 0.0f));
            checkPixel(image, 60, 4, Vec3(rightIsLit ? 1.0f : 0.0f));
        };
        const auto checkAll = [&] {
            // Left as it is, everything is in the first channel and lit by every light.
            channels(1, 1, 1);
            check(true, true);
            channels(1, 2, 1);
            check(true, false);
            channels(1, 2, 2);
            check(false, true);
            // Sharing one channel of several is enough.
            channels(1, 2, 3);
            check(true, true);
            channels(0x80, 0x06, 0x84);
            check(true, true);
            channels(0x80, 0x06, 0x41);
            check(false, false);
            // What is in no channel is lit by no light, and a light of no channel lights nothing.
            channels(0, 1, 0xFF);
            check(false, true);
            channels(0xFF, 0xFF, 0);
            check(false, false);
        };

        // A directional light, on surfaces that are lit from the G-buffer.
        checkAll();
        CHECK(scene.renderer->statistics().basePass.drawCalls == 1);
        CHECK(scene.renderer->statistics().basePass.instances == 2);

        // A point light, which is found through the clusters. Far enough away to light
        // the wall almost evenly, and bright enough to light all of it to white.
        *world.tryGet<LightComponent>(lamp) = {.type = LightType::Point, .intensity = 2000.0f, .range = 100.0f};
        world.transform(lamp).position = Vec3(0.0f, 0.0f, 30.0f);
        checkAll();

        // The same for surfaces drawn by the forward pass.
        world.tryGet<MeshRendererComponent>(left)->materials = {scene.material("sheer")};
        world.tryGet<MeshRendererComponent>(right)->materials = {scene.material("sheer")};
        checkAll();
        CHECK(scene.renderer->statistics().forwardPass.instances == 2);
        *world.tryGet<LightComponent>(lamp) = {.type = LightType::Directional};
        checkAll();

        // Ambient light is everywhere: it does not ask for channels.
        scene.setAmbient(Vec3(0.5f), Vec3(0.5f));
        channels(0, 0, 0);
        const Image ambient = scene.render(view);
        checkPixel(ambient, 16, 32, Vec3(0.5f));
        checkPixel(ambient, 48, 32, Vec3(0.5f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("hundreds of lights light a floor as adding them up one by one does, with and without perspective") {
        const UVec2 size(160, 90);
        SceneFixture scene(size);
        scene.writeMatteMaterial("matte");
        scene.writeMatteMaterial("sheer", Vec3(1.0f), R"("alphaMode": "blended")");
        scene.open();
        const Entity floor = scene.add(BuiltinMesh::Plane, scene.material("matte"), at(Vec3(0.0f), Vec3(60.0f)));

        // Seeded: the same lights on every run. Small and low, so that each lights a
        // patch of the floor and most of the floor is lit by a few of them.
        Random random(20240611);
        std::vector<PlacedLight> lights;
        for (uint32_t index = 0; index < 400; ++index) {
            PlacedLight placed;
            placed.light.type = index % 3 == 0 ? LightType::Spot : LightType::Point;
            placed.light.color = Vec3(random.range(0.1f, 1.0f), random.range(0.1f, 1.0f), random.range(0.1f, 1.0f));
            placed.light.intensity = random.range(0.02f, 0.25f);
            placed.light.range = random.range(0.8f, 3.5f);
            placed.light.innerConeDegrees = random.range(5.0f, 35.0f);
            placed.light.outerConeDegrees = placed.light.innerConeDegrees + random.range(1.0f, 30.0f);
            placed.position = Vec3(random.range(-12.0f, 12.0f), random.range(0.3f, 1.6f), random.range(-12.0f, 12.0f));
            placed.direction = glm::normalize(Vec3(random.range(-0.7f, 0.7f), -1.0f, random.range(-0.7f, 0.7f)));
            scene.addLight(placed.light, placed.position, placed.direction);
            lights.push_back(placed);
        }

        CameraComponent wideLens;
        wideLens.verticalFovDegrees = 70.0f;
        wideLens.nearPlane = 0.05f;
        wideLens.farPlane = 300.0f;
        const float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
        const std::vector<std::pair<std::string, CameraView>> views = {
            {"perspective, across the floor", scene.perspectiveView(Vec3(0.0f, 3.0f, 13.0f), Vec3(0.0f, 0.0f, 2.0f))},
            {"perspective, wide and low",
             makeCameraView(glm::inverse(lookAt(Vec3(-9.0f, 0.8f, 9.0f), Vec3(2.0f, 0.0f, -3.0f))), wideLens, aspect)},
            {"orthographic, from above at an angle",
             scene.orthographicView(Vec3(0.0f, 12.0f, 9.0f), Vec3(0.0f), 16.0f)},
        };

        // What the model says the floor shows in a pixel: the sum of what each light does
        // where the ray through the pixel meets the floor. Nothing when it misses the floor.
        const auto modelAt = [&](const CameraView& view, Vec2 pixel) -> std::optional<Vec3> {
            const Ray ray = screenRay(view, pixel, Vec2(size));
            if (ray.direction.y > -1.0e-3f) {
                return std::nullopt;
            }
            const Vec3 point = ray.at(-ray.origin.y / ray.direction.y);
            if (std::abs(point.x) > 29.0f || std::abs(point.z) > 29.0f) {
                return std::nullopt;
            }
            Vec3 sum(0.0f);
            for (const PlacedLight& placed : lights) {
                sum += lightOn(point, kWorldUp, placed);
            }
            return displayed(sum);
        };

        // Compares a picture of the floor with the model. A floor seen at a low angle runs
        // through one pixel for many centimetres, and where exactly in the pixel the GPU
        // takes its sample is not for a test to say: a pixel is right when it is within
        // what the model gives an eighth of a pixel around its centre. A light missing
        // from a list would be missed in full.
        const auto checkFloor = [&](const Image& image, const CameraView& view) {
            constexpr float kTolerance = 4.0f / 255.0f;
            constexpr std::array<Vec2, 5> kSamples = {Vec2(0.0f, 0.0f), Vec2(0.125f, 0.0f), Vec2(-0.125f, 0.0f),
                                                      Vec2(0.0f, 0.125f), Vec2(0.0f, -0.125f)};
            uint32_t compared = 0;
            uint32_t lit = 0;
            uint32_t wrong = 0;
            float worst = 0.0f;
            UVec2 worstPixel(0, 0);
            for (uint32_t y = 0; y < size.y; ++y) {
                // Every other pixel, in the pattern of a chess board.
                for (uint32_t x = y % 2; x < size.x; x += 2) {
                    const Vec2 centre(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                    Vec3 least(1.0f);
                    Vec3 most(0.0f);
                    bool onFloor = true;
                    for (const Vec2& offset : kSamples) {
                        const std::optional<Vec3> expected = modelAt(view, centre + offset);
                        if (!expected) {
                            onFloor = false;
                            break;
                        }
                        least = glm::min(least, *expected);
                        most = glm::max(most, *expected);
                    }
                    if (!onFloor) {
                        continue;
                    }
                    const Vec3 actual(image.pixel(x, y));
                    const Vec3 outside = glm::max(glm::max(least - actual, actual - most), Vec3(0.0f));
                    const float error = std::max({outside.r, outside.g, outside.b});
                    ++compared;
                    lit += std::max({most.r, most.g, most.b}) > 0.15f ? 1 : 0;
                    wrong += error > kTolerance ? 1 : 0;
                    if (error > worst) {
                        worst = error;
                        worstPixel = UVec2(x, y);
                    }
                }
            }
            CAPTURE(worst);
            CAPTURE(worstPixel.x);
            CAPTURE(worstPixel.y);
            // There was a floor to compare, and light on a good part of it.
            CHECK(compared > size.x * size.y / 6);
            CHECK(lit > compared / 4);
            CHECK(wrong == 0);
        };

        for (const auto& [name, view] : views) {
            CAPTURE(name);
            scene.world.tryGet<MeshRendererComponent>(floor)->materials = {scene.material("matte")};
            const Image deferred = scene.render(view);
            const SceneRenderStatistics statistics = scene.renderer->statistics();
            CHECK(statistics.forwardPass.drawCalls == 0);
            // Most of the lights are in view, and none was left out.
            CHECK(statistics.lights > 100);
            CHECK(statistics.droppedLights == 0);
            // The clusters do their work: a light is in the lists of the clusters around
            // it, not of all of them.
            CHECK(statistics.lightListEntries < statistics.lights * (kClusterCount / 10));
            checkFloor(deferred, view);

            // Drawn forward, the floor is lit by the same lights from the same lists.
            scene.world.tryGet<MeshRendererComponent>(floor)->materials = {scene.material("sheer")};
            const Image forward = scene.render(view);
            CHECK(scene.renderer->statistics().basePass.drawCalls == 0);
            checkFloor(forward, view);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a glossy, textured surface looks the same under point, spot and rect lights whether it is lit from "
              "the G-buffer or drawn forward") {
        SceneFixture scene(UVec2(96, 96));
        scene.writeImage("colors",
                         test::makeChecker(64, 4, Vec4(0.8f, 0.3f, 0.1f, 1.0f), Vec4(0.1f, 0.3f, 0.7f, 1.0f)));
        scene.writeImage("bumps", test::makeBumpNormalMap(64, 4));
        scene.open();
        const std::string surface = fmt::format(
            R"("baseColorTexture": "{}", "normalTexture": "{}", "roughness": 0.35, "metallic": 0.4, "specular": 0.8)",
            scene.asset("colors.png").toString(), scene.asset("bumps.png").toString());
        scene.writeMaterial("deferred", surface);
        scene.writeMaterial("forward", surface + R"(, "alphaMode": "blended")");
        REQUIRE(scene.assets->refresh().has_value());

        const Entity ball = scene.add(BuiltinMesh::Sphere, scene.material("deferred"), at(Vec3(0.0f), Vec3(1.6f)));
        scene.world.tryGet<MeshRendererComponent>(ball)->lightChannels = 5;
        scene.addLight({.type = LightType::Point,
                        .color = Vec3(1.0f, 0.9f, 0.7f),
                        .intensity = 2.0f,
                        .range = 6.0f,
                        .channels = 1},
                       Vec3(1.5f, 1.8f, 1.2f));
        scene.addLight({.type = LightType::Spot,
                        .color = Vec3(0.5f, 0.7f, 1.0f),
                        .intensity = 6.0f,
                        .range = 8.0f,
                        .innerConeDegrees = 10.0f,
                        .outerConeDegrees = 20.0f,
                        .channels = 4},
                       Vec3(-2.0f, 0.5f, 2.0f), Vec3(2.0f, -0.4f, -2.0f));
        // A panel below the ball, shining up at it: its reflection is a rectangle.
        scene.addLight({.type = LightType::Rect,
                        .color = Vec3(1.0f, 0.6f, 0.4f),
                        .intensity = 1.5f,
                        .range = 6.0f,
                        .width = 1.2f,
                        .height = 0.6f,
                        .channels = 4},
                       Vec3(0.6f, -1.6f, 1.4f), Vec3(-0.4f, 1.0f, -0.8f));
        // In a channel the ball is not in: it must add nothing in either pass.
        scene.addLight({.type = LightType::Point, .intensity = 50.0f, .range = 9.0f, .channels = 2},
                       Vec3(0.0f, 0.0f, 3.0f));
        const CameraView view = scene.perspectiveView(Vec3(1.5f, 1.0f, 2.5f), Vec3(0.0f));

        const Image deferred = scene.render(view);
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 0);
        scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material("forward")};
        const Image forward = scene.render(view);
        CHECK(scene.renderer->statistics().basePass.drawCalls == 0);

        // The picture is not flat or blown out: there is something to compare.
        CHECK(glm::distance(Vec3(deferred.pixel(40, 40)), Vec3(deferred.pixel(60, 60))) > 0.05f);
        CHECK(deferred.pixel(48, 48).r < 0.98f);
        const ImageComparison difference = unwrap(compareImages(deferred, forward, {.channelTolerance = 3}));
        CHECK(difference.maxChannelDifference <= 3);
        CHECK(difference.differingPixels == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the light count view shows how many lights each pixel has to ask") {
        WallScene wall;
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::LightCount;

        // Without point or spot lights, every pixel asks none: dark grey. A directional
        // light is asked by all and is not counted.
        wall.add({.light = {.type = LightType::Directional}});
        Image image = wall.render(settings);
        checkShown(image, 34, 32, Vec3(0.1f));
        checkShown(image, 56, 32, Vec3(0.1f));

        // Small lights just in front of the wall, a little right of the middle: they
        // reach the clusters around them and no others.
        const auto addLights = [&](uint32_t count) {
            for (uint32_t index = 0; index < count; ++index) {
                wall.add({.light = {.type = LightType::Point, .range = 0.3f}, .position = Vec3(0.125f, 0.0f, 0.2f)});
            }
        };
        addLights(1);
        image = wall.render(settings);
        // One light: a quarter of the way from blue to green.
        checkShown(image, 34, 32, Vec3(0.0f, 0.25f, 0.75f));
        checkShown(image, 56, 32, Vec3(0.1f));
        checkShown(image, 8, 8, Vec3(0.1f));

        addLights(3);
        checkShown(wall.render(settings), 34, 32, Vec3(0.0f, 1.0f, 0.0f));
        addLights(2);
        checkShown(wall.render(settings), 34, 32, Vec3(0.5f, 0.5f, 0.0f));
        // Eight and more are red.
        addLights(2);
        checkShown(wall.render(settings), 34, 32, Vec3(1.0f, 0.0f, 0.0f));
        addLights(20);
        image = wall.render(settings);
        checkShown(image, 34, 32, Vec3(1.0f, 0.0f, 0.0f));
        checkShown(image, 56, 32, Vec3(0.1f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("the renderer counts the lights of a view, and leaves out what cannot be seen") {
        WallScene wall;
        const SceneRenderer& renderer = *wall.scene.renderer;

        wall.render();
        CHECK(renderer.statistics().lights == 0);
        CHECK(renderer.statistics().lightListEntries == 0);
        CHECK(renderer.statistics().droppedLights == 0);

        // A directional light always counts; it is in no cluster's list.
        wall.add({.light = {.type = LightType::Directional}});
        wall.render();
        CHECK(renderer.statistics().lights == 1);
        CHECK(renderer.statistics().lightListEntries == 0);

        // A point light in view counts, with one entry for every cluster it reaches.
        const Entity lamp =
            wall.add({.light = {.type = LightType::Point, .range = 0.5f}, .position = Vec3(0.0f, 0.0f, 1.0f)});
        wall.render();
        CHECK(renderer.statistics().lights == 2);
        const uint32_t entries = renderer.statistics().lightListEntries;
        CHECK(entries > 0);
        CHECK(entries < kClusterCount / 8);

        // Behind the camera, or to the side of what it sees, it reaches no cluster and
        // is not sent to the GPU.
        wall.scene.world.transform(lamp).position = Vec3(0.0f, 0.0f, 9.0f);
        wall.render();
        CHECK(renderer.statistics().lights == 1);
        CHECK(renderer.statistics().lightListEntries == 0);
        wall.scene.world.transform(lamp).position = Vec3(7.0f, 0.0f, 1.0f);
        wall.render();
        CHECK(renderer.statistics().lights == 1);

        // Reaching into view from outside, it counts again.
        wall.scene.world.tryGet<LightComponent>(lamp)->range = 6.0f;
        wall.render();
        CHECK(renderer.statistics().lights == 2);
        CHECK(renderer.statistics().lightListEntries > 0);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("more lights than a view can hold are left out, counted and reported once") {
        WallScene wall;
        const SceneRenderer& renderer = *wall.scene.renderer;
        const LogCapture log;

        // One more directional light than fits: each would add a fifth of white.
        for (uint32_t index = 0; index < kMaxDirectionalLights + 1; ++index) {
            wall.add({.light = {.type = LightType::Directional, .intensity = 0.2f}});
        }
        Image image = wall.render();
        CHECK(renderer.statistics().lights == kMaxDirectionalLights);
        CHECK(renderer.statistics().droppedLights == 1);
        checkPixel(image, 32, 32, Vec3(0.2f * static_cast<float>(kMaxDirectionalLights)));
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("too many lights for one view: 1 left out"));

        // Not again every frame.
        wall.render();
        wall.render();
        CHECK(log.count(LogLevel::Warn) == 1);

        // Point lights, six more than fit, all of them in view.
        wall.scene.world.clear();
        wall.wall = wall.scene.add(BuiltinMesh::Plane, wall.scene.material("matte"),
                                   at(Vec3(0.0f), Vec3(8.0f), glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f))));
        for (uint32_t index = 0; index < kMaxLocalLights + 6; ++index) {
            wall.add({.light = {.type = LightType::Point,
                                .intensity = 1.0f / static_cast<float>(kMaxLocalLights),
                                .range = 4.0f},
                      .position = Vec3(0.0f, 0.0f, 1.0f)});
        }
        image = wall.render();
        CHECK(renderer.statistics().lights == kMaxLocalLights);
        CHECK(renderer.statistics().droppedLights == 6);
        // The first 1024 of them are what lights the wall: together, one light of
        // intensity one a metre away, less what the fade towards the range takes.
        const PlacedLight together{.light = {.type = LightType::Point, .intensity = 1.0f, .range = 4.0f},
                                   .position = Vec3(0.0f, 0.0f, 1.0f)};
        checkPixel(image, 32, 32, lightOn(onWall(32, 32), kWallNormal, together));
        checkPixel(image, 44, 20, lightOn(onWall(44, 20), kWallNormal, together));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("the lights of a view may become many more from one frame to the next, and fewer again") {
        WallScene wall;
        const PlacedLight lamp{.light = {.type = LightType::Point, .intensity = 0.5f, .range = 100.0f},
                               .position = Vec3(0.0f, 0.0f, 1.0f)};
        const Entity first = wall.add(lamp);
        wall.checkLit(wall.render(), {lamp});

        // Three hundred more, each reaching every cluster: far more lights and list
        // entries than the buffers start out with room for.
        std::vector<Entity> more;
        PlacedLight dim = lamp;
        dim.light.intensity = 0.001f;
        for (uint32_t index = 0; index < 300; ++index) {
            more.push_back(wall.add(dim));
        }
        PlacedLight sum = lamp;
        sum.light.intensity = 0.5f + 300.0f * 0.001f;
        wall.checkLit(wall.render(), {sum});
        CHECK(wall.scene.renderer->statistics().lights == 301);
        CHECK(wall.scene.renderer->statistics().lightListEntries > 300 * 100);

        // And back to one: nothing of the others is left behind.
        for (const Entity entity : more) {
            wall.scene.world.destroyEntity(entity);
        }
        wall.checkLit(wall.render(), {lamp});
        CHECK(wall.scene.renderer->statistics().lights == 1);

        // To none at all.
        wall.scene.world.destroyEntity(first);
        checkPixel(wall.render(), 32, 32, Vec3(0.0f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a world without lights is black, unless the view asks for the default lighting") {
        WallScene wall;
        // A game shows what its scene says.
        checkPixel(wall.render(), 32, 32, Vec3(0.0f));

        // An editor lights a scene that has no lights yet.
        SceneRenderSettings lit = SceneFixture::flatSettings();
        lit.defaultLighting = true;
        const Vec4 pixel = wall.render(lit).pixel(32, 32);
        CHECK(pixel.r > 0.3f);
        CHECK(pixel.g > 0.3f);
        CHECK(pixel.b > 0.3f);

        // Once the scene has a light, it is lit by that alone - even while the light is
        // switched off: a scene that was made dark stays dark.
        const Entity lamp = wall.add({.light = {.type = LightType::Directional, .color = Vec3(0.0f, 0.5f, 0.0f)}});
        checkPixel(wall.render(lit), 32, 32, Vec3(0.0f, 0.5f, 0.0f));
        wall.scene.world.setEnabled(lamp, false);
        checkPixel(wall.render(lit), 32, 32, Vec3(0.0f));

        // The same goes for a scene with nothing but ambient light.
        wall.scene.world.destroyEntity(lamp);
        wall.scene.setAmbient(Vec3(0.2f), Vec3(0.2f));
        checkPixel(wall.render(lit), 32, 32, Vec3(0.2f));
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("two views of one world are each lit through their own clusters") {
        WallScene wall;
        const PlacedLight lamp{.light = {.type = LightType::Point, .intensity = 1.0f, .range = 3.0f},
                               .position = Vec3(0.6f, 0.4f, 1.0f)};
        wall.add(lamp);

        // A second view of the same wall, with perspective, from further away and into
        // targets of another size, recorded into the same command list.
        const UVec2 otherSize(96, 54);
        SceneTargets otherTargets = unwrap(SceneTargets::create(wall.scene.context.device(), otherSize, "Other"));
        // Narrow enough to see nothing but the wall.
        CameraComponent lens;
        lens.verticalFovDegrees = 30.0f;
        const CameraView other =
            makeCameraView(glm::inverse(lookAt(Vec3(1.0f, 0.5f, 6.0f), Vec3(0.0f))), lens, 96.0f / 54.0f);

        wall.scene.world.updateWorldTransforms();
        test::recordAndExecute(wall.scene.context, [&](nvrhi::ICommandList* commandList) {
            wall.scene.renderer->render(commandList, wall.scene.world, wall.view, wall.scene.targets,
                                        SceneFixture::flatSettings());
            wall.scene.renderer->render(commandList, wall.scene.world, other, otherTargets,
                                        SceneFixture::flatSettings());
        });
        wall.checkLit(test::readToImage(*wall.scene.context.graphics, wall.scene.targets.display()), {lamp});

        const Image second = test::readToImage(*wall.scene.context.graphics, otherTargets.display());
        for (uint32_t y = 3; y < otherSize.y; y += 8) {
            for (uint32_t x = 3; x < otherSize.x; x += 8) {
                const Ray ray =
                    screenRay(other, Vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), Vec2(otherSize));
                // Where the ray meets the wall, the plane z = 0.
                const Vec3 point = ray.at(-ray.origin.z / ray.direction.z);
                checkPixel(second, x, y, lightOn(point, kWallNormal, lamp));
            }
        }
        const SceneRenderStatistics frame = wall.scene.renderer->takeFrameStatistics();
        CHECK(frame.views == 2);
        CHECK(frame.lights == 2);

        otherTargets = SceneTargets();
        checkNoValidationIssues(*wall.scene.context.graphics);
    }
}
