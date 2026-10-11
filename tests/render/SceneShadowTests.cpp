#include "cordite/assets/ImageCompare.h"
#include "cordite/math/Color.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/ShadowPlan.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::checkShown;
using cordite::test::lightOn;
using cordite::test::LogCapture;
using cordite::test::PlacedLight;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// How many pixels of a picture were compared with the model, by what the model says
/// they are.
struct Compared {
    uint32_t lit = 0;
    uint32_t shadowed = 0;
};

/// Compares what a picture shows of a flat, white, matte receiver with the model: the
/// lights, less what the boxes of `casters` keep from it, and the fill lights, which
/// cast no shadows and shine through everything. Pixels that show a caster or miss the
/// receiver are left out, and so are pixels within `margin` of the edge of a shadow or
/// of a caster: there the picture is soft and the model is not. Pixels farther from the
/// camera than `maxDepth` metres are left out as well. A pixel counts as shadowed when a
/// caster hides at least one light from it.
Compared checkReceiver(const Image& image, const CameraView& view, const Plane& receiver, const Vec3& normal,
                       std::span<const PlacedLight> lights, std::span<const PlacedLight> fillLights,
                       std::span<const Aabb> casters, uint32_t step = 2,
                       float maxDepth = std::numeric_limits<float>::max(), float margin = 2.0f) {
    const Vec2 size(static_cast<float>(image.width()), static_cast<float>(image.height()));

    struct Model {
        Vec3 shown{0.0f};
        /// One bit for each light, set where a caster hides it.
        uint32_t hidden = 0;
    };

    const auto modelAt = [&](Vec2 pixel) -> std::optional<Model> {
        const Ray ray = screenRay(view, pixel, size);
        const std::optional<float> hit = intersect(ray, receiver);
        if (!hit) {
            return std::nullopt;
        }
        for (const Aabb& caster : casters) {
            const std::optional<float> inFront = intersect(ray, caster);
            if (inFront && *inFront < *hit) {
                return std::nullopt;
            }
        }
        const Vec3 point = ray.at(*hit);
        if (glm::dot(point - view.position, view.forward) > maxDepth) {
            return std::nullopt;
        }
        Model model;
        for (size_t index = 0; index < lights.size(); ++index) {
            const Vec3 reaching = lightOn(point, normal, lights[index], casters);
            const Vec3 unhindered = lightOn(point, normal, lights[index]);
            model.shown += reaching;
            model.hidden |= glm::distance(reaching, unhindered) > 1.0e-4f ? 1u << (index % 32u) : 0u;
        }
        for (const PlacedLight& placed : fillLights) {
            model.shown += lightOn(point, normal, placed);
        }
        return model;
    };

    const std::array<Vec2, 8> around = {Vec2(margin, 0.0f),    Vec2(-margin, 0.0f),   Vec2(0.0f, margin),
                                        Vec2(0.0f, -margin),   Vec2(margin, margin),  Vec2(-margin, margin),
                                        Vec2(margin, -margin), Vec2(-margin, -margin)};
    Compared compared;
    for (uint32_t y = 1; y < image.height(); y += step) {
        for (uint32_t x = 1; x < image.width(); x += step) {
            const Vec2 centre(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            const std::optional<Model> model = modelAt(centre);
            if (!model) {
                continue;
            }
            bool atAnEdge = false;
            for (const Vec2& offset : around) {
                const std::optional<Model> beside = modelAt(centre + offset);
                atAnEdge = atAnEdge || !beside || beside->hidden != model->hidden;
            }
            if (atAnEdge) {
                continue;
            }
            checkPixel(image, x, y, model->shown);
            compared.shadowed += model->hidden != 0 ? 1 : 0;
            compared.lit += model->hidden != 0 ? 0 : 1;
        }
    }
    return compared;
}

/// A white, matte floor seen from straight above by a camera without perspective, with
/// boxes standing on it or hovering over it, and lights. The picture is 64 pixels a
/// side and shows four metres of the floor: its right is +X, its bottom +Z.
struct FloorScene {
    FloorScene() {
        scene.writeMatteMaterial("matte");
        scene.writeMatteMaterial("sheer", Vec3(1.0f), R"("alphaMode": "blended")");
        scene.open();
        floor = scene.add(BuiltinMesh::Plane, scene.material("matte"), at(Vec3(0.0f), Vec3(40.0f)));
        view = scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
    }

    /// A box that stands in the light: `size` metres large, around `centre`.
    Entity addCaster(const Vec3& centre, const Vec3& size) {
        casters.push_back(Aabb::fromCenterExtents(centre, size * 0.5f));
        return scene.add(BuiltinMesh::Cube, scene.material("matte"), at(centre, size));
    }

    Entity addLight(const PlacedLight& placed) {
        lights.push_back(placed);
        return scene.addLight(placed.light, placed.position, placed.direction);
    }

    /// A light that casts no shadows: it shines through the boxes.
    Entity addFillLight(const PlacedLight& placed) {
        fillLights.push_back(placed);
        return scene.addLight(placed.light, placed.position, placed.direction);
    }

    Image render(const SceneRenderSettings& settings = SceneFixture::flatSettings()) {
        return scene.render(view, settings);
    }

    /// Compares the floor in a picture with the model.
    [[nodiscard]] Compared checkFloor(const Image& image) const {
        return checkReceiver(image, view, Plane::fromPointNormal(Vec3(0.0f), kWorldUp), kWorldUp, lights, fillLights,
                             casters);
    }

    /// The same, as if nothing threw a shadow: every light shines through the boxes,
    /// which still hide the floor under them from the camera.
    [[nodiscard]] Compared checkFloorWithoutShadows(const Image& image) const {
        std::vector<PlacedLight> all = lights;
        all.insert(all.end(), fillLights.begin(), fillLights.end());
        return checkReceiver(image, view, Plane::fromPointNormal(Vec3(0.0f), kWorldUp), kWorldUp, {}, all, casters);
    }

    SceneFixture scene{UVec2(64, 64)};
    Entity floor;
    CameraView view;
    std::vector<Aabb> casters;
    std::vector<PlacedLight> lights;
    std::vector<PlacedLight> fillLights;
};

/// A sun that casts shadows, shining along `direction`. One cascade, twenty metres
/// deep: enough for the scenes of these tests, and a texel of it is two centimetres.
PlacedLight shadowSun(const Vec3& direction, float intensity = 1.0f) {
    return {.light = {.type = LightType::Directional,
                      .intensity = intensity,
                      .castsShadows = true,
                      .shadowDistance = 20.0f,
                      .shadowCascades = 1},
            .direction = glm::normalize(direction)};
}

/// What `checkBumpySurface` draws.
struct BumpySurface {
    /// Blended surfaces are drawn by the forward pass, all others through the G-buffer.
    bool blended = false;
    /// What the picture shows from top to bottom, in metres.
    float viewHeight = 4.0f;
};

/// Draws a flat square with a normal map of round bumps from above, under a sun that
/// stands at several heights down to four degrees over the horizon, with shadows and
/// without, and checks that the two pictures are the same: nothing stands in the light.
///
/// The square leaves a border of background around it, and beside it lies a strip one
/// pixel wide: the pixels at an edge, and those with nothing beside them at all, are
/// where finding the surface's own normal is hardest.
void checkBumpySurface(const BumpySurface& surface) {
    constexpr uint32_t kSize = 96;
    SceneFixture scene(UVec2(kSize, kSize));
    scene.writeImage("bumps", test::makeBumpNormalMap(64, 4));
    scene.writeMatteMaterial("plain");
    // Red, so that no sun makes it look like the grey background.
    scene.writeMatteMaterial("strip", Vec3(1.0f, 0.2f, 0.2f));
    scene.open();
    // Half of the map across the square: two bumps, each some thirty pixels wide.
    scene.writeMatteMaterial("bumpy", Vec3(1.0f),
                             fmt::format(R"("normalTexture": "{}", "uvScale": [0.5, 0.5], "normalScale": 1.5{})",
                                         scene.asset("bumps.png").toString(),
                                         surface.blended ? R"(, "alphaMode": "blended")" : ""));
    REQUIRE(scene.assets->refresh().has_value());

    const float side = surface.viewHeight * 0.75f;
    if (surface.blended) {
        // What is blended casts no shadow, and so cannot be in its own. It lies on a
        // solid floor here, as a decal would, and must not be in the floor's.
        scene.add(BuiltinMesh::Plane, scene.material("plain"), at(Vec3(0.0f), Vec3(side, 1.0f, side)));
        scene.add(BuiltinMesh::Plane, scene.material("bumpy"), at(Vec3(0.0f, 0.001f, 0.0f), Vec3(side, 1.0f, side)));
    } else {
        scene.add(BuiltinMesh::Plane, scene.material("bumpy"), at(Vec3(0.0f), Vec3(side, 1.0f, side)));
    }
    // The strip covers the middle of the pixels of column 91 and of no other.
    const float pixel = surface.viewHeight / static_cast<float>(kSize);
    scene.add(BuiltinMesh::Plane, scene.material("strip"),
              at(Vec3((91.5f - 48.0f) * pixel, 0.0f, 0.0f), Vec3(0.3f * pixel, 1.0f, side)));
    const CameraView view =
        scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), surface.viewHeight, Vec3(0.0f, 0.0f, -1.0f));

    const Entity sun = scene.addLight(shadowSun(Vec3(0.0f, -1.0f, 0.0f), 1.5f).light, Vec3(0.0f));
    SceneRenderSettings withoutShadows = SceneFixture::flatSettings();
    withoutShadows.shadowQuality = ShadowQuality::Off;
    for (const float degrees : {20.0f, 50.0f, 70.0f, 80.0f, 86.0f}) {
        for (const float turn : {0.0f, 45.0f, 170.0f}) {
            CAPTURE(degrees);
            CAPTURE(turn);
            const float tilt = toRadians(degrees);
            scene.world.transform(sun).rotation =
                lookRotation(Vec3(std::sin(tilt) * std::cos(toRadians(turn)), -std::cos(tilt),
                                  std::sin(tilt) * std::sin(toRadians(turn))));
            const Image shadowed = scene.render(view);
            CHECK(scene.renderer->statistics().shadowMaps == 1);
            const Image plain = scene.render(view, withoutShadows);

            // The bumps are in the picture - the square is not of one color - and so is
            // the strip, between two columns of background.
            float darkest = 1.0f;
            float brightest = 0.0f;
            for (uint32_t y = 16; y < 80; y += 2) {
                for (uint32_t x = 16; x < 80; x += 2) {
                    darkest = std::min(darkest, plain.pixel(x, y).g);
                    brightest = std::max(brightest, plain.pixel(x, y).g);
                }
            }
            CHECK(brightest - darkest > 0.1f);
            const Vec3 strip = Vec3(plain.pixel(91, 48));
            CHECK(glm::distance(strip, Vec3(plain.pixel(90, 48))) > 0.05f);
            CHECK(glm::distance(strip, Vec3(plain.pixel(92, 48))) > 0.05f);

            const ImageComparison difference = unwrap(compareImages(plain, shadowed, {.channelTolerance = 3}));
            CHECK(difference.differingPixels == 0);
        }
    }
    checkNoValidationIssues(*scene.context.graphics);
}

PlacedLight shadowSpot(const Vec3& position, const Vec3& target, float intensity) {
    return {.light = {.type = LightType::Spot,
                      .intensity = intensity,
                      .range = 12.0f,
                      .innerConeDegrees = 30.0f,
                      .outerConeDegrees = 40.0f,
                      .castsShadows = true},
            .position = position,
            .direction = glm::normalize(target - position)};
}

PlacedLight shadowPoint(const Vec3& position, float intensity) {
    return {.light = {.type = LightType::Point, .intensity = intensity, .range = 12.0f, .castsShadows = true},
            .position = position};
}

/// The linear brightness of a pixel of a picture that is grey there.
float brightnessAt(const Image& image, uint32_t x, uint32_t y) {
    return srgbToLinear(image.pixel(x, y).g);
}

} // namespace

TEST_SUITE("SceneShadows") {
    TEST_CASE("what stands in the light of the sun throws a shadow, and nothing else is darker for it") {
        FloorScene floor;
        // The sun 45 degrees up in the +X sky; a cube of a metre hovering half a metre
        // over the floor. Its shadow lies to its left, one and a half metres long.
        floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        floor.addCaster(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f));
        const Image image = floor.render();

        // In the shadow there is no light at all; beside it, all there is: the cosine of
        // 45 degrees.
        checkPixel(image, 12, 32, Vec3(0.0f));
        checkPixel(image, 4, 30, Vec3(0.0f));
        checkPixel(image, 12, 12, Vec3(std::sqrt(0.5f)));
        checkPixel(image, 52, 32, Vec3(std::sqrt(0.5f)));
        // The top of the cube is lit like the floor, and not in a shadow of its own.
        checkPixel(image, 32, 32, Vec3(std::sqrt(0.5f)));
        checkPixel(image, 26, 38, Vec3(std::sqrt(0.5f)));

        const Compared compared = floor.checkFloor(image);
        CHECK(compared.shadowed > 40);
        CHECK(compared.lit > 400);

        const SceneRenderStatistics& statistics = floor.scene.renderer->statistics();
        CHECK(statistics.shadowMaps == 1);
        // The floor and the cube, in one map.
        CHECK(statistics.shadowPass.drawCalls == 2);
        CHECK(statistics.shadowPass.triangles == 14);
        CHECK(statistics.lightsWithoutShadows == 0);
        // What the GPU was asked to draw includes the shadow maps.
        CHECK(statistics.meshes.drawCalls ==
              statistics.shadowPass.drawCalls + statistics.depthPass.drawCalls + statistics.basePass.drawCalls);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a spot light throws shadows away from where it stands") {
        FloorScene floor;
        floor.addLight(shadowSpot(Vec3(1.6f, 3.0f, 0.4f), Vec3(-0.3f, 0.0f, 0.0f), 14.0f));
        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.8f, 1.0f, 0.8f));
        floor.addCaster(Vec3(-0.9f, 1.2f, -1.0f), Vec3(0.5f, 0.3f, 0.5f));
        const Image image = floor.render();

        const Compared compared = floor.checkFloor(image);
        CHECK(compared.shadowed > 25);
        CHECK(compared.lit > 300);
        CHECK(floor.scene.renderer->statistics().shadowMaps == 1);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a spot light wider than a shadow map can be throws no shadow in its outermost ring") {
        // A light half a metre over the floor that shines down and to all sides, nearly
        // level, and a wall a metre from it that is taller than the light is high. The
        // shadow map of a spot light sees 85 degrees to each side of its axis, which is
        // 5.7 metres from under this light: up to there the floor behind the wall is in
        // its shadow, and beyond, where the map has nothing to say, it is lit.
        FloorScene floor;
        PlacedLight spot = shadowSpot(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.0f), 50.0f);
        spot.light.range = 20.0f;
        spot.light.innerConeDegrees = 88.0f;
        spot.light.outerConeDegrees = 89.0f;
        floor.addLight(spot);
        floor.addCaster(Vec3(1.0f, 0.5f, 0.0f), Vec3(0.2f, 1.0f, 0.6f));

        // Eight metres of the floor, from two to ten metres out: a pixel is an eighth of
        // a metre, and row 32 runs straight away from the light.
        const CameraView view =
            floor.scene.orthographicView(Vec3(6.0f, 6.0f, 0.0f), Vec3(6.0f, 0.0f, 0.0f), 8.0f, Vec3(0.0f, 0.0f, -1.0f));
        const Image image = floor.scene.render(view);
        CHECK(floor.scene.renderer->statistics().shadowMaps == 1);
        const auto floorAt = [](uint32_t x, uint32_t y) {
            return Vec3(2.0f + (static_cast<float>(x) + 0.5f) / 8.0f, 0.0f,
                        (static_cast<float>(y) + 0.5f) / 8.0f - 4.0f);
        };

        // Four metres out, 83 degrees from the axis: in the shadow of the wall, and
        // beside that shadow in the light.
        checkPixel(image, 16, 32, Vec3(0.0f));
        checkPixel(image, 16, 48, lightOn(floorAt(16, 48), kWorldUp, spot));
        // Eight metres out, 86 degrees from the axis: behind the wall still, and lit as
        // if nothing stood in the light.
        const Vec3 beyond = lightOn(floorAt(48, 32), kWorldUp, spot);
        CHECK(beyond.g > 0.03f);
        checkPixel(image, 48, 32, beyond);
        checkPixel(image, 60, 30, lightOn(floorAt(60, 30), kWorldUp, spot));
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a point light throws shadows to every side") {
        FloorScene floor;
        floor.addLight(shadowPoint(Vec3(0.2f, 1.6f, -0.1f), 3.0f));
        // Around the light, at different heights.
        floor.addCaster(Vec3(1.0f, 0.4f, 0.0f), Vec3(0.4f, 0.8f, 0.4f));
        floor.addCaster(Vec3(-0.9f, 0.3f, 0.6f), Vec3(0.5f, 0.6f, 0.5f));
        floor.addCaster(Vec3(0.1f, 0.5f, -1.1f), Vec3(0.6f, 1.0f, 0.3f));
        floor.addCaster(Vec3(-0.6f, 0.9f, -0.8f), Vec3(0.3f, 0.2f, 0.3f));
        const Image image = floor.render();

        const Compared compared = floor.checkFloor(image);
        CHECK(compared.shadowed > 40);
        CHECK(compared.lit > 300);
        CHECK(floor.scene.renderer->statistics().shadowMaps == 6);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("each of the six shadow maps of a point light shows what is on its side") {
        // A wall on one side of a light, a block between the two, and a camera that looks
        // at the wall past the light: the shadow of the block is a frame around it.
        const std::array<Vec3, 6> sides = {Vec3(1.0f, 0.0f, 0.0f),  Vec3(-1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                           Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f),  Vec3(0.0f, 0.0f, -1.0f)};
        const Quat upright = glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f));
        for (const Vec3& side : sides) {
            CAPTURE(side);
            SceneFixture scene;
            scene.writeMatteMaterial("matte");
            scene.open();
            // The plane stands up to face +Z; turned to look along `side`, it faces the light.
            scene.add(BuiltinMesh::Plane, scene.material("matte"),
                      at(side * 3.0f, Vec3(12.0f), lookRotation(side) * upright));
            const Vec3 blockCentre = side * 1.5f;
            scene.add(BuiltinMesh::Cube, scene.material("matte"), at(blockCentre, Vec3(0.5f)));
            const std::vector<Aabb> casters = {Aabb::fromCenterExtents(blockCentre, Vec3(0.25f))};
            const std::vector<PlacedLight> lights = {shadowPoint(Vec3(0.0f), 6.0f)};
            scene.addLight(lights[0].light, lights[0].position);

            const Vec3 up = std::abs(side.y) > 0.5f ? Vec3(0.0f, 0.0f, 1.0f) : kWorldUp;
            const CameraView view = scene.orthographicView(side * -2.0f, side * 3.0f, 4.0f, up);
            const Image image = scene.render(view);

            const Compared compared =
                checkReceiver(image, view, Plane::fromPointNormal(side * 3.0f, -side), -side, lights, {}, casters, 1);
            // The frame of shadow around the block, and the lit wall around that.
            CHECK(compared.shadowed > 20);
            CHECK(compared.lit > 1000);
            CHECK(scene.renderer->statistics().shadowMaps == 6);
            checkNoValidationIssues(*scene.context.graphics);
        }
    }

    TEST_CASE("a lit surface shows no stripes of its own shadow, however low the light stands") {
        FloorScene floor;
        const Entity sun = floor.addLight(shadowSun(Vec3(0.0f, -1.0f, 0.0f), 2.0f));
        // From straight above down to three degrees over the horizon, and from several sides.
        for (const float degrees : {0.0f, 30.0f, 60.0f, 80.0f, 87.0f}) {
            for (const float turn : {0.0f, 37.0f, 90.0f, 205.0f}) {
                CAPTURE(degrees);
                CAPTURE(turn);
                const float tilt = toRadians(degrees);
                const Vec3 direction(std::sin(tilt) * std::cos(toRadians(turn)), -std::cos(tilt),
                                     std::sin(tilt) * std::sin(toRadians(turn)));
                floor.lights[0].direction = glm::normalize(direction);
                floor.scene.world.transform(sun).rotation = lookRotation(direction);
                const Compared compared = floor.checkFloorWithoutShadows(floor.render());
                CHECK(compared.lit > 900);
            }
        }
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a bumpy surface under a low light looks the same with shadows as without, when nothing stands in it") {
        // The lookup in the shadow map is moved along the surface's normal, and the filter
        // reads the map along the surface's plane. A normal map bends the normal the light
        // is computed with; the surface itself stays flat, and must not end up in a
        // shadow of its own for it.
        SUBCASE("drawn through the G-buffer") {
            checkBumpySurface({.blended = false, .viewHeight = 4.0f});
        }
        SUBCASE("drawn by the forward pass") {
            checkBumpySurface({.blended = true, .viewHeight = 4.0f});
        }
        // Where a pixel covers half a millimetre, the surface's own normal still has to
        // be found from positions that hardly differ.
        SUBCASE("drawn through the G-buffer, from very near") {
            checkBumpySurface({.blended = false, .viewHeight = 0.05f});
        }
        SUBCASE("drawn by the forward pass, from very near") {
            checkBumpySurface({.blended = true, .viewHeight = 0.05f});
        }
    }

    TEST_CASE("point and spot lights low over a surface light it without stripes too") {
        FloorScene floor;
        // A point light a hand's breadth over the floor: at its edges the light arrives
        // nearly level.
        const Entity point = floor.addLight(shadowPoint(Vec3(0.3f, 0.25f, -0.2f), 0.4f));
        Compared compared = floor.checkFloorWithoutShadows(floor.render());
        CHECK(compared.lit > 500);

        // A spot light shining along the floor from one side of it.
        floor.scene.world.destroyEntity(point);
        floor.lights.clear();
        PlacedLight spot = shadowSpot(Vec3(-2.5f, 0.4f, 0.0f), Vec3(1.0f, 0.0f, 0.2f), 6.0f);
        spot.light.outerConeDegrees = 50.0f;
        floor.addLight(spot);
        compared = floor.checkFloorWithoutShadows(floor.render());
        CHECK(compared.lit > 500);
        CHECK(floor.scene.renderer->statistics().shadowMaps == 1);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a shadow begins where the thing that casts it stands") {
        FloorScene floor;
        const Entity sun = floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        // Standing on the floor: its left side is at x = -0.5, which is pixel 24.
        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f));
        const Image image = floor.render();
        // Ten centimetres from the box the floor is in its shadow, not in a gap of light
        // between the two; the shadow is a metre long and ends before pixel 8.
        CHECK(brightnessAt(image, 22, 32) < 0.05f);
        CHECK(brightnessAt(image, 21, 28) < 0.05f);
        CHECK(brightnessAt(image, 12, 32) < 0.01f);
        CHECK(brightnessAt(image, 10, 32) < 0.1f);
        CHECK(brightnessAt(image, 4, 32) == doctest::Approx(std::sqrt(0.5f)).epsilon(0.03));
        CHECK(floor.checkFloor(image).shadowed > 20);

        // With the offsets turned far up, the shadow is eaten away from its far end:
        // that is what too much of them does.
        LightComponent& light = *floor.scene.world.tryGet<LightComponent>(sun);
        light.shadowBias = 16.0f;
        light.shadowNormalOffset = 16.0f;
        CHECK(brightnessAt(floor.render(), 10, 32) > 0.3f);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("what the camera does not see still throws its shadow into the view") {
        FloorScene floor;
        // The view ends two metres from the middle; the box stands three metres out, and
        // a sun that is not high throws its shadow in, to within a quarter of a metre
        // of the middle.
        floor.addLight(shadowSun(Vec3(-1.5f, -1.0f, 0.0f), 1.5f));
        floor.addCaster(Vec3(3.0f, 0.75f, 0.0f), Vec3(1.0f, 1.5f, 1.0f));
        const Image image = floor.render();
        const Compared compared = floor.checkFloor(image);
        CHECK(compared.shadowed > 30);
        CHECK(compared.lit > 300);
        // The box itself is outside the view and was not drawn into it.
        CHECK(floor.scene.renderer->statistics().culled == 1);
        CHECK(floor.scene.renderer->statistics().basePass.drawCalls == 1);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("several lights throw their shadows at once, each its own") {
        FloorScene floor;
        floor.addLight(shadowSun(Vec3(-0.6f, -1.0f, 0.3f), 0.25f));
        floor.addLight(shadowSpot(Vec3(-1.8f, 2.5f, 1.2f), Vec3(0.2f, 0.0f, 0.0f), 4.0f));
        floor.addLight(shadowPoint(Vec3(1.2f, 1.4f, -1.0f), 0.8f));
        // One that casts no shadows, among them: it shines through everything.
        PlacedLight fill = shadowPoint(Vec3(0.0f, 2.0f, 1.5f), 0.4f);
        fill.light.castsShadows = false;
        floor.addFillLight(fill);

        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.6f, 1.0f, 0.6f));
        floor.addCaster(Vec3(-1.0f, 0.3f, -0.9f), Vec3(0.5f, 0.6f, 0.5f));
        floor.addCaster(Vec3(1.1f, 0.25f, 0.9f), Vec3(0.4f, 0.5f, 0.4f));
        const Image image = floor.render();

        const Compared compared = floor.checkFloor(image);
        CHECK(compared.shadowed > 100);
        CHECK(compared.lit > 100);

        const SceneRenderStatistics& statistics = floor.scene.renderer->statistics();
        // One cascade for the sun, one map for the spot light, six for the point light.
        CHECK(statistics.shadowMaps == 8);
        CHECK(statistics.lights == 4);
        CHECK(statistics.lightsWithoutShadows == 0);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("shadows fall on surfaces drawn forward as they do on surfaces lit from the G-buffer") {
        FloorScene floor;
        floor.addLight(shadowSun(Vec3(-0.6f, -1.0f, 0.3f), 0.3f));
        floor.addLight(shadowSpot(Vec3(-1.8f, 2.5f, 1.2f), Vec3(0.2f, 0.0f, 0.0f), 4.0f));
        floor.addLight(shadowPoint(Vec3(1.2f, 1.4f, -1.0f), 0.8f));
        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.6f, 1.0f, 0.6f));
        floor.addCaster(Vec3(-1.0f, 0.3f, -0.9f), Vec3(0.5f, 0.6f, 0.5f));

        const Image deferred = floor.render();
        CHECK(floor.scene.renderer->statistics().forwardPass.drawCalls == 0);
        // The floor again, of a material that is blended, with nothing showing through.
        floor.scene.world.tryGet<MeshRendererComponent>(floor.floor)->materials = {floor.scene.material("sheer")};
        const Image forward = floor.render();
        CHECK(floor.scene.renderer->statistics().forwardPass.drawCalls == 1);
        // The floor no longer throws shadows - nothing blended does - which changes
        // nothing: there is nothing under it.
        const ImageComparison difference = unwrap(compareImages(deferred, forward, {.channelTolerance = 3}));
        CHECK(difference.differingPixels == 0);
        CHECK(floor.checkFloor(forward).shadowed > 40);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("a masked surface throws the shadow of what is left of it") {
        FloorScene floor;
        // A square of a metre with two of its four quarters cut out, a metre over the
        // floor; the sun at 45 degrees puts its shadow a metre to the left.
        floor.scene.writeImage("quarters", test::makeChecker(16, 2, Vec4(1.0f), Vec4(1.0f, 1.0f, 1.0f, 0.0f)));
        REQUIRE(floor.scene.assets->refresh().has_value());
        floor.scene.writeMatteMaterial(
            "cutout", Vec3(1.0f),
            fmt::format(R"("baseColorTexture": "{}", "alphaMode": "masked", "alphaCutoff": 0.5, "doubleSided": true,
                           "filter": "nearest")",
                        floor.scene.asset("quarters.png").toString()));
        REQUIRE(floor.scene.assets->refresh().has_value());
        floor.scene.add(BuiltinMesh::Plane, floor.scene.material("cutout"), at(Vec3(0.0f, 1.0f, 0.0f)));
        floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        const Image image = floor.render();

        // The shadow covers x from -1.5 to -0.5 and z from -0.5 to 0.5: pixels 8 to 24
        // across, 24 to 40 down. Its quarters: the top left and the bottom right are
        // there, the other two are holes.
        const float lit = std::sqrt(0.5f);
        checkPixel(image, 12, 28, Vec3(0.0f));
        checkPixel(image, 20, 36, Vec3(0.0f));
        checkPixel(image, 20, 28, Vec3(lit));
        checkPixel(image, 12, 36, Vec3(lit));
        // Around it, light.
        checkPixel(image, 4, 32, Vec3(lit));
        checkPixel(image, 16, 18, Vec3(lit));
        checkPixel(image, 16, 46, Vec3(lit));
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("what is told not to throws no shadow: a mesh, a light, a surface that lets light through") {
        FloorScene floor;
        const Entity light = floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        const Entity box = floor.addCaster(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f));
        World& world = floor.scene.world;
        const SceneRenderer& renderer = *floor.scene.renderer;

        // As it is, the box throws a shadow.
        CHECK(floor.checkFloor(floor.render()).shadowed > 40);

        // A mesh that casts no shadows.
        world.tryGet<MeshRendererComponent>(box)->castsShadows = false;
        CHECK(floor.checkFloorWithoutShadows(floor.render()).lit > 700);
        CHECK(renderer.statistics().shadowPass.drawCalls == 1);
        world.tryGet<MeshRendererComponent>(box)->castsShadows = true;

        // A light that casts none: no shadow map is drawn at all.
        world.tryGet<LightComponent>(light)->castsShadows = false;
        CHECK(floor.checkFloorWithoutShadows(floor.render()).lit > 700);
        CHECK(renderer.statistics().shadowMaps == 0);
        CHECK(renderer.statistics().shadowPass.drawCalls == 0);
        // The atlas stays as it is, for when something casts a shadow again.
        CHECK(renderer.statistics().shadowAtlasBytes == uint64_t{4096} * 4096 * 4);
        world.tryGet<LightComponent>(light)->castsShadows = true;

        // A box of a blended material: it is drawn, and lets the light past.
        world.tryGet<MeshRendererComponent>(box)->materials = {floor.scene.material("sheer")};
        floor.render();
        CHECK(renderer.statistics().forwardPass.drawCalls == 1);
        CHECK(renderer.statistics().shadowPass.drawCalls == 1);
        // (The box covers part of the floor in the picture; beside it the floor is lit.)
        checkPixel(floor.render(), 12, 32, Vec3(std::sqrt(0.5f)));
        world.tryGet<MeshRendererComponent>(box)->materials = {floor.scene.material("matte")};

        // A box in a lighting channel the light does not shine on: not lit by it, and no
        // obstacle to it.
        world.tryGet<MeshRendererComponent>(box)->lightChannels = 2;
        const Image image = floor.render();
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkPixel(image, 12, 32, Vec3(std::sqrt(0.5f)));
        checkPixel(image, 4, 30, Vec3(std::sqrt(0.5f)));
        checkPixel(image, 52, 12, Vec3(std::sqrt(0.5f)));
        // A light that shines on both channels is blocked by it again.
        world.tryGet<LightComponent>(light)->channels = 3;
        CHECK(floor.checkFloor(floor.render()).shadowed > 40);
        world.tryGet<MeshRendererComponent>(box)->lightChannels = 1;
        world.tryGet<LightComponent>(light)->channels = 1;

        // A view with its shadows switched off.
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.shadowQuality = ShadowQuality::Off;
        CHECK(floor.checkFloorWithoutShadows(floor.render(settings)).lit > 700);
        CHECK(renderer.statistics().shadowMaps == 0);
        CHECK(renderer.statistics().lightsWithoutShadows == 0);

        // And back to where it began.
        CHECK(floor.checkFloor(floor.render()).shadowed > 40);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("shadows are as right at every quality, in an atlas of another size") {
        FloorScene floor;
        floor.addLight(shadowSun(Vec3(-0.6f, -1.0f, 0.3f), 0.3f));
        floor.addLight(shadowSpot(Vec3(-1.8f, 2.5f, 1.2f), Vec3(0.2f, 0.0f, 0.0f), 4.0f));
        floor.addLight(shadowPoint(Vec3(1.2f, 1.4f, -1.0f), 0.8f));
        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.6f, 1.0f, 0.6f));
        floor.addCaster(Vec3(-1.0f, 0.3f, -0.9f), Vec3(0.5f, 0.6f, 0.5f));

        // From one frame to the next, as a player changing the settings would.
        for (const ShadowQuality quality : {ShadowQuality::Medium, ShadowQuality::Low, ShadowQuality::High,
                                            ShadowQuality::Low, ShadowQuality::Medium}) {
            CAPTURE(nameOf(quality));
            SceneRenderSettings settings = SceneFixture::flatSettings();
            settings.shadowQuality = quality;
            const Compared compared = floor.checkFloor(floor.render(settings));
            CHECK(compared.shadowed > 40);
            CHECK(compared.lit > 300);
            CHECK(floor.scene.renderer->statistics().shadowMaps == 8);
            // The atlas is as large as the quality says, four bytes a texel.
            const uint64_t side = shadowSettingsFor(quality).atlasSize;
            CHECK(floor.scene.renderer->statistics().shadowAtlasBytes == side * side * 4);
        }

        // Switched off, the atlas gives its memory back: what is left is the smallest
        // there can be, so that there is something to bind.
        SceneRenderSettings off = SceneFixture::flatSettings();
        off.shadowQuality = ShadowQuality::Off;
        CHECK(floor.checkFloorWithoutShadows(floor.render(off)).lit > 300);
        CHECK(floor.scene.renderer->statistics().shadowMaps == 0);
        CHECK(floor.scene.renderer->statistics().shadowAtlasBytes ==
              uint64_t{kMinShadowTileSize} * kMinShadowTileSize * 4);
        // And on again.
        CHECK(floor.checkFloor(floor.render()).shadowed > 40);
        CHECK(floor.scene.renderer->statistics().shadowAtlasBytes == uint64_t{4096} * 4096 * 4);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("only the brightest sun casts shadows, and the renderer says so once") {
        FloorScene floor;
        const LogCapture log;
        floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f), 0.6f));
        // A second one from the other side, less bright. It lights the floor; its shadow
        // is missing: a scene has one sun.
        PlacedLight second = shadowSun(Vec3(1.0f, -1.0f, 0.0f), 0.4f);
        floor.scene.addLight(second.light, second.position, second.direction);
        floor.addCaster(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f));

        Image image = floor.render();
        const SceneRenderStatistics& statistics = floor.scene.renderer->statistics();
        CHECK(statistics.shadowMaps == 1);
        CHECK(statistics.lightsWithoutShadows == 1);
        // In the shadow of the brighter one there is the light of the other, and where
        // the other would have a shadow there is the light of both.
        checkPixel(image, 12, 32, Vec3(0.4f * std::sqrt(0.5f)));
        checkPixel(image, 52, 32, Vec3(std::sqrt(0.5f)));
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("1 lights that are to cast shadows cast none"));

        floor.render();
        floor.render();
        CHECK(log.count(LogLevel::Warn) == 1);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("views that show something else than the lit picture draw no shadow maps") {
        FloorScene floor;
        floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        floor.addLight(shadowPoint(Vec3(1.2f, 1.4f, -1.0f), 0.8f));
        floor.addCaster(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f));
        const SceneRenderer& renderer = *floor.scene.renderer;

        floor.render();
        CHECK(renderer.statistics().shadowMaps == 7);

        for (const RenderDebugView debugView :
             {RenderDebugView::BaseColor, RenderDebugView::Depth, RenderDebugView::LightCount,
              RenderDebugView::ShadowCascades, RenderDebugView::Overdraw}) {
            CAPTURE(nameOf(debugView));
            SceneRenderSettings settings = SceneFixture::flatSettings();
            settings.debugView = debugView;
            floor.render(settings);
            CHECK(renderer.statistics().shadowMaps == 0);
            CHECK(renderer.statistics().shadowPass.drawCalls == 0);
        }

        // The view of the light alone is lit, and has its shadows.
        SceneRenderSettings lightingOnly = SceneFixture::flatSettings();
        lightingOnly.debugView = RenderDebugView::LightingOnly;
        const Image image = floor.render(lightingOnly);
        CHECK(renderer.statistics().shadowMaps == 7);
        CHECK(brightnessAt(image, 12, 32) < brightnessAt(image, 12, 12) * 0.5f);
        checkNoValidationIssues(*floor.scene.context.graphics);
    }

    TEST_CASE("the shadow of the sun reaches as far as its shadow distance, through all its cascades") {
        const UVec2 size(256, 144);
        SceneFixture scene(size);
        scene.writeMatteMaterial("matte");
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("matte"), at(Vec3(0.0f), Vec3(400.0f)));

        // A long wall along the way the camera looks, on its right, and a sun from the
        // right: the shadow of the wall is a band on the floor, six metres wide, that runs
        // from in front of the camera to eighty metres away.
        const Vec3 wallCentre(2.5f, 1.5f, -30.0f);
        const Vec3 wallSize(1.0f, 3.0f, 80.0f);
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(wallCentre, wallSize));
        const std::vector<Aabb> casters = {Aabb::fromCenterExtents(wallCentre, wallSize * 0.5f)};
        PlacedLight sun = shadowSun(Vec3(-1.0f, -0.5f, 0.0f));
        sun.light.shadowDistance = 60.0f;
        sun.light.shadowCascades = 4;
        const std::vector<PlacedLight> lights = {sun};
        scene.addLight(sun.light, sun.position, sun.direction);

        CameraComponent lens;
        lens.verticalFovDegrees = 50.0f;
        lens.nearPlane = 0.1f;
        lens.farPlane = 300.0f;
        const CameraView view = makeCameraView(glm::inverse(lookAt(Vec3(0.0f, 4.0f, 10.0f), Vec3(0.0f, 0.0f, -20.0f))),
                                               lens, static_cast<float>(size.x) / static_cast<float>(size.y));
        const Image image = scene.render(view);
        CHECK(scene.renderer->statistics().shadowMaps == 4);

        // Up to where the shadows start to fade - nine tenths of the shadow distance -
        // the floor is as the model says, near the camera and far from it. The far
        // cascades are coarse and the sun is low, which lays the soft edge of the
        // shadow out over a few pixels: those are left out.
        const Plane ground = Plane::fromPointNormal(Vec3(0.0f), kWorldUp);
        const Compared near = checkReceiver(image, view, ground, kWorldUp, lights, {}, casters, 1, 53.0f, 4.0f);
        CHECK(near.shadowed > 2000);
        CHECK(near.lit > 2000);

        const auto brightnessOf = [&](const Vec3& point) {
            const std::optional<Vec2> pixel = worldToScreen(view, point, Vec2(size));
            REQUIRE(pixel.has_value());
            return srgbToLinear(image.pixel(static_cast<uint32_t>(pixel->x), static_cast<uint32_t>(pixel->y)).g);
        };
        // What the sun gives the floor: half of one, for the angle it arrives at.
        const float lit = 0.5f / std::sqrt(1.25f);
        // Fifty metres from the camera, in the last cascade: still in the shadow.
        CHECK(brightnessOf(Vec3(-1.0f, 0.0f, -40.0f)) < 0.03f);
        CHECK(brightnessOf(Vec3(-7.0f, 0.0f, -40.0f)) == doctest::Approx(lit).epsilon(0.03));
        // Beyond the shadow distance there are no shadows: where the band would go on,
        // the floor is as bright as beside it.
        CHECK(brightnessOf(Vec3(-1.0f, 0.0f, -58.0f)) == doctest::Approx(lit).epsilon(0.03));
        // And between nine tenths of it and all of it, the shadow is on its way out.
        const float fading = brightnessOf(Vec3(-1.0f, 0.0f, -47.0f));
        CHECK(fading > 0.1f * lit);
        CHECK(fading < 0.9f * lit);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the shadow cascades view colors the picture by the cascade each pixel is looked up in") {
        const UVec2 size(256, 144);
        SceneFixture scene(size);
        scene.writeMatteMaterial("matte");
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("matte"), at(Vec3(0.0f), Vec3(400.0f)));
        PlacedLight sun = shadowSun(Vec3(-1.0f, -0.7f, 0.0f));
        sun.light.shadowDistance = 60.0f;
        sun.light.shadowCascades = 4;
        sun.light.shadowCascadeDistribution = 0.5f;
        const Entity sunEntity = scene.addLight(sun.light, sun.position, sun.direction);

        CameraComponent lens;
        lens.verticalFovDegrees = 50.0f;
        lens.nearPlane = 0.1f;
        lens.farPlane = 300.0f;
        const CameraView view = makeCameraView(glm::inverse(lookAt(Vec3(0.0f, 4.0f, 10.0f), Vec3(0.0f, 0.0f, -20.0f))),
                                               lens, static_cast<float>(size.x) / static_cast<float>(size.y));
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::ShadowCascades;
        Image image = scene.render(view, settings);

        // Where the cascades end, as the plan has them.
        const auto ends = cascadeEnds(0.1f, 60.0f, 4, 0.5f);
        const std::array<Vec3, 4> tints = {Vec3(1.0f, 0.25f, 0.25f), Vec3(0.25f, 1.0f, 0.25f), Vec3(0.25f, 0.25f, 1.0f),
                                           Vec3(1.0f, 1.0f, 0.25f)};
        const Plane ground = Plane::fromPointNormal(Vec3(0.0f), kWorldUp);
        std::array<uint32_t, 5> seen{};
        for (uint32_t y = 2; y < size.y; y += 3) {
            for (uint32_t x = 2; x < size.x; x += 5) {
                const Ray ray =
                    screenRay(view, Vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), Vec2(size));
                const std::optional<float> hit = intersect(ray, ground);
                if (!hit) {
                    continue;
                }
                const float depth = glm::dot(ray.at(*hit) - view.position, view.forward);
                // The floor ends two hundred metres away.
                if (depth > 150.0f) {
                    continue;
                }
                // Not within a few percent of where two cascades meet: a pixel there may
                // be on either side.
                bool atABorder = std::abs(depth - 60.0f) < 3.0f;
                size_t cascade = 0;
                for (size_t index = 0; index < 4; ++index) {
                    atABorder = atABorder || std::abs(depth - ends[index]) < 0.04f * ends[index];
                    cascade += depth > ends[index] ? 1 : 0;
                }
                if (atABorder) {
                    continue;
                }
                // The floor is white, so the tints come out as they are; beyond the
                // shadow distance the picture is grey.
                checkShown(image, x, y, cascade < 4 ? tints[cascade] : Vec3(0.5f));
                ++seen[cascade];
            }
        }
        // Every cascade is in the picture, and what is beyond them.
        for (const uint32_t count : seen) {
            CHECK(count > 5);
        }

        // Without a sun that casts shadows, all of it is grey.
        scene.world.tryGet<LightComponent>(sunEntity)->castsShadows = false;
        image = scene.render(view, settings);
        checkShown(image, 128, 130, Vec3(0.5f));
        checkShown(image, 128, 90, Vec3(0.5f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("two views of one world each get the shadows of their own cameras") {
        FloorScene floor;
        floor.addLight(shadowSun(Vec3(-1.0f, -1.0f, 0.0f)));
        floor.addLight(shadowPoint(Vec3(1.2f, 1.4f, -1.0f), 0.8f));
        floor.addCaster(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.6f, 1.0f, 0.6f));

        // The second view: with perspective, from the side, at another quality.
        const UVec2 otherSize(128, 72);
        SceneTargets otherTargets = unwrap(SceneTargets::create(floor.scene.context.device(), otherSize, "Other"));
        CameraComponent lens;
        lens.verticalFovDegrees = 45.0f;
        const CameraView other =
            makeCameraView(glm::inverse(lookAt(Vec3(0.5f, 5.0f, 4.0f), Vec3(0.0f))), lens, 128.0f / 72.0f);
        SceneRenderSettings otherSettings = SceneFixture::flatSettings();
        otherSettings.shadowQuality = ShadowQuality::Low;

        floor.scene.world.updateWorldTransforms();
        test::recordAndExecute(floor.scene.context, [&](nvrhi::ICommandList* commandList) {
            floor.scene.renderer->render(commandList, floor.scene.world, floor.view, floor.scene.targets,
                                         SceneFixture::flatSettings());
            floor.scene.renderer->render(commandList, floor.scene.world, other, otherTargets, otherSettings);
        });
        const Compared first =
            floor.checkFloor(test::readToImage(*floor.scene.context.graphics, floor.scene.targets.display()));
        CHECK(first.shadowed > 20);
        const Compared second =
            checkReceiver(test::readToImage(*floor.scene.context.graphics, otherTargets.display()), other,
                          Plane::fromPointNormal(Vec3(0.0f), kWorldUp), kWorldUp, floor.lights, {}, floor.casters, 1);
        CHECK(second.shadowed > 40);
        CHECK(second.lit > 1000);

        const SceneRenderStatistics frame = floor.scene.renderer->takeFrameStatistics();
        CHECK(frame.views == 2);
        CHECK(frame.shadowMaps == 14);

        otherTargets = SceneTargets();
        checkNoValidationIssues(*floor.scene.context.graphics);
    }
}
