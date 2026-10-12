#include "cordite/renderer/Fog.h"

#include "cordite/assets/Image.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::checkShown;
using cordite::test::kBackground;
using cordite::test::kBlue;
using cordite::test::kGreen;
using cordite::test::kRed;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// A box that shows one color, whatever the light.
struct FlatBox {
    Vec3 center{0.0f};
    Vec3 size{1.0f};
    Vec3 color{1.0f};

    [[nodiscard]] Aabb bounds() const { return Aabb::fromCenterExtents(center, size * 0.5f); }
};

/// A world of such boxes, and what a test knows of it: where they are. With `sky`, an
/// environment of that one color all around, which is seen where no box is.
struct FlatScene {
    explicit FlatScene(std::vector<FlatBox> flatBoxes, std::optional<Vec3> sky = std::nullopt,
                       UVec2 pictureSize = UVec2(64, 64))
        : scene(pictureSize), boxes(std::move(flatBoxes)), size(pictureSize) {
        for (size_t index = 0; index < boxes.size(); ++index) {
            scene.writeFlatMaterial(fmt::format("flat{}", index), boxes[index].color);
        }
        if (sky) {
            Image even = unwrap(Image::create(8, 4, ImageFormat::Rgba32F));
            for (uint32_t y = 0; y < even.height(); ++y) {
                for (uint32_t x = 0; x < even.width(); ++x) {
                    even.setPixel(x, y, Vec4(*sky, 1.0f));
                }
            }
            scene.writeHdrImage("sky", even);
        }
        scene.open();
        for (size_t index = 0; index < boxes.size(); ++index) {
            // The built-in cube is a metre a side.
            scene.add(BuiltinMesh::Cube, scene.material(fmt::format("flat{}", index)),
                      at(boxes[index].center, boxes[index].size));
        }
        if (sky) {
            scene.setEnvironment(scene.asset("sky.hdr"));
        }
    }

    SceneFixture scene;
    std::vector<FlatBox> boxes;
    UVec2 size;
};

/// Puts a fog into a world, at its densest up to `height`.
Entity addFog(SceneFixture& scene, const FogComponent& fog, float height = 0.0f) {
    const Entity entity = scene.world.createEntity("Fog");
    scene.world.transform(entity).position = Vec3(7.0f, height, -3.0f);
    scene.world.add<FogComponent>(entity) = fog;
    return entity;
}

/// The fog such a component is at such a height, put together by hand: what the
/// pictures are held against. It has a sun only when the test says where one is.
FogSettings fogOf(const FogComponent& component, float height, const std::optional<Vec3>& towardsSun = std::nullopt) {
    FogSettings fog;
    fog.color = component.color;
    fog.density = component.density;
    fog.height = height;
    fog.heightFalloff = component.heightFalloff;
    fog.startDistance = component.startDistance;
    fog.maxOpacity = component.maxOpacity;
    fog.sunExponent = component.sunExponent;
    if (towardsSun) {
        fog.sunColor = component.sunColor;
        fog.towardsSun = *towardsSun;
    }
    return fog;
}

/// What the line of sight through a point of a picture meets.
struct Sight {
    /// Where the line sets out, and the unit vector along it.
    Vec3 viewer{0.0f};
    Vec3 direction = kWorldForward;
    /// Which box it meets first - none: -1 - and where.
    int box = -1;
    Vec3 point{0.0f};
};

Sight sightThrough(const CameraView& view, const Vec2& pixel, const Vec2& size, std::span<const FlatBox> boxes) {
    const Ray ray = screenRay(view, pixel, size);
    Sight sight;
    sight.direction = ray.direction;
    // With perspective every line of sight sets out at the camera. Without, each has a
    // start of its own, in the plane the camera is in.
    sight.viewer = view.orthographic ? ray.origin - view.forward * glm::dot(ray.origin - view.position, view.forward)
                                     : view.position;
    float nearest = std::numeric_limits<float>::max();
    for (size_t index = 0; index < boxes.size(); ++index) {
        const std::optional<float> hit = intersect(ray, boxes[index].bounds());
        // What is beyond the far plane is not drawn.
        if (hit && *hit < nearest && glm::dot(ray.at(*hit) - view.position, view.forward) < view.farPlane) {
            nearest = *hit;
            sight.box = static_cast<int>(index);
            sight.point = ray.at(*hit);
        }
    }
    return sight;
}

/// Holds a picture of `flat` against what the fog on every line of sight says: the
/// color of the box the line meets - or `behind`, where it meets none - seen through
/// `fog`. Every `step`-th pixel is looked at.
void checkAgainstFog(const Image& image, const FlatScene& flat, const CameraView& view, const FogSettings& fog,
                     const Vec3& behind, uint32_t step = 3) {
    const Vec2 size(flat.size);
    uint32_t checked = 0;
    for (uint32_t y = 1; y + 1 < flat.size.y; y += step) {
        for (uint32_t x = 1; x + 1 < flat.size.x; x += step) {
            const Vec2 corner(static_cast<float>(x), static_cast<float>(y));
            const Sight middle = sightThrough(view, corner + Vec2(0.5f), size, flat.boxes);
            // A pixel on an edge shows a little of both sides. Only those that show one
            // thing from corner to corner are held to it.
            bool isWhole = true;
            for (const Vec2& offset : {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f), Vec2(1.0f, 1.0f)}) {
                isWhole = isWhole && sightThrough(view, corner + offset, size, flat.boxes).box == middle.box;
            }
            if (!isWhole) {
                continue;
            }
            const Vec3 expected = middle.box >= 0 ? applyFog(fog, flat.boxes[static_cast<size_t>(middle.box)].color,
                                                             middle.viewer, middle.point)
                                                  : applyFogTowards(fog, behind, middle.viewer, middle.direction);
            checkPixel(image, x, y, expected);
            ++checked;
        }
    }
    // Most of them are whole.
    CHECK(checked > (flat.size.x / step) * (flat.size.y / step) * 3 / 4);
}

/// Three walls side by side that fill the view of a camera without perspective which
/// looks along -Z from five metres in front of the origin and sees three metres from
/// top to bottom: their fronts are 7, 15 and 35 metres from it.
constexpr std::array<float, 3> kWallDistances = {7.0f, 15.0f, 35.0f};
constexpr std::array<Vec3, 3> kWallColors = {kRed, kGreen, kBlue};
/// A column of pixels well inside each of them, in a picture 64 pixels wide.
constexpr std::array<uint32_t, 3> kWallColumns = {10, 32, 53};

std::vector<FlatBox> threeWalls() {
    return {
        {.center = Vec3(-1.0f, 0.0f, -2.5f), .size = Vec3(1.0f, 3.0f, 1.0f), .color = kRed},
        {.center = Vec3(0.0f, 0.0f, -10.5f), .size = Vec3(1.0f, 3.0f, 1.0f), .color = kGreen},
        {.center = Vec3(1.0f, 0.0f, -30.5f), .size = Vec3(1.0f, 3.0f, 1.0f), .color = kBlue},
    };
}

CameraView wallView(const SceneFixture& scene, float back = 0.0f) {
    return scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f + back), Vec3(0.0f), 3.0f);
}

/// A floor of 300 metres whose top is at height zero, a wall and a pillar on it.
std::vector<FlatBox> yard() {
    return {
        {.center = Vec3(0.0f, -0.5f, 0.0f), .size = Vec3(300.0f, 1.0f, 300.0f), .color = Vec3(0.8f, 0.2f, 0.1f)},
        {.center = Vec3(3.0f, 2.0f, -12.0f), .size = Vec3(2.0f, 4.0f, 2.0f), .color = Vec3(0.1f, 0.9f, 0.3f)},
        {.center = Vec3(-4.0f, 5.0f, -25.0f), .size = Vec3(1.5f, 10.0f, 1.5f), .color = Vec3(0.2f, 0.3f, 1.0f)},
    };
}

/// Views of the yard: with perspective from above the fog, from down in it and from
/// high above; without, down at it from the side, and up past it at what is behind.
std::vector<CameraView> yardViews(const SceneFixture& scene) {
    return {
        scene.perspectiveView(Vec3(0.0f, 3.0f, 6.0f), Vec3(0.0f, 0.5f, -10.0f)),
        scene.perspectiveView(Vec3(0.0f, 0.4f, 6.0f), Vec3(0.0f, 2.0f, -10.0f)),
        scene.perspectiveView(Vec3(0.0f, 30.0f, 0.1f), Vec3(0.0f, 0.0f, -4.0f)),
        scene.orthographicView(Vec3(0.0f, 6.0f, 10.0f), Vec3(0.0f, 1.0f, -4.0f), 12.0f),
        scene.orthographicView(Vec3(0.0f, 3.5f, 8.0f), Vec3(0.0f, 8.5f, -2.0f), 5.0f),
    };
}

/// A fog and the height it is put at.
struct PlacedFog {
    FogComponent fog;
    float height = 0.0f;
};

/// True when every number of the scene color of `scene` is one.
bool sceneColorIsFinite(SceneFixture& scene) {
    const TexturePixels pixels = unwrap(readTexture(*scene.context.graphics, scene.targets.sceneColor()));
    REQUIRE(pixels.format == nvrhi::Format::RGBA16_FLOAT);
    for (size_t offset = 0; offset + 8 <= pixels.data.size(); offset += 8) {
        std::array<uint16_t, 3> halves{};
        std::memcpy(halves.data(), pixels.data.data() + offset, sizeof(halves));
        for (const uint16_t half : halves) {
            if (!std::isfinite(glm::unpackHalf1x16(half))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_SUITE("Fog") {
    TEST_CASE("fog takes the place of what is far away: by one minus e to the minus its density times the distance") {
        FlatScene flat(threeWalls());
        SceneFixture& scene = flat.scene;
        const Vec3 fogColor(0.8f, 0.6f, 0.2f);
        const Entity fog = addFog(scene, {.color = fogColor, .density = 0.05f, .heightFalloff = 0.0f});

        Image image = scene.render(wallView(scene));
        for (size_t wall = 0; wall < kWallColumns.size(); ++wall) {
            CAPTURE(wall);
            const float amount = 1.0f - std::exp(-0.05f * kWallDistances[wall]);
            checkPixel(image, kWallColumns[wall], 32, glm::mix(kWallColors[wall], fogColor, amount));
            // The same all over a wall that faces the camera.
            checkPixel(image, kWallColumns[wall], 6, glm::mix(kWallColors[wall], fogColor, amount));
        }
        // About three tenths, a half and five sixths: the first wall is still red.
        checkPixel(image, kWallColumns[0], 32, Vec3(0.940938f, 0.177187f, 0.059062f));
        checkPixel(image, kWallColumns[2], 32, Vec3(0.660981f, 0.495736f, 0.339019f));

        // From ten metres further back there are ten metres more of it in front of each:
        // without perspective, too, the fog is between the camera and what it sees.
        image = scene.render(wallView(scene, 10.0f));
        for (size_t wall = 0; wall < kWallColumns.size(); ++wall) {
            CAPTURE(wall);
            const float amount = 1.0f - std::exp(-0.05f * (kWallDistances[wall] + 10.0f));
            checkPixel(image, kWallColumns[wall], 32, glm::mix(kWallColors[wall], fogColor, amount));
        }

        // Twice as dense, as much is lost in half the distance.
        scene.world.tryGet<FogComponent>(fog)->density = 0.1f;
        image = scene.render(wallView(scene));
        checkPixel(image, kWallColumns[0], 32, glm::mix(kRed, fogColor, 1.0f - std::exp(-0.7f)));
        checkPixel(image, kWallColumns[1], 32, glm::mix(kGreen, fogColor, 1.0f - std::exp(-1.5f)));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the fog begins at its start distance, and never takes more of a pixel than its greatest opacity") {
        FlatScene flat(threeWalls());
        SceneFixture& scene = flat.scene;
        const Vec3 fogColor(0.8f, 0.6f, 0.2f);
        const Entity fog =
            addFog(scene, {.color = fogColor, .density = 0.05f, .heightFalloff = 0.0f, .startDistance = 10.0f});

        // Seven metres away is clear; of the fifteen to the next wall, five are fog.
        Image image = scene.render(wallView(scene));
        checkPixel(image, kWallColumns[0], 32, kRed);
        checkPixel(image, kWallColumns[1], 32, glm::mix(kGreen, fogColor, 1.0f - std::exp(-0.25f)));
        checkPixel(image, kWallColumns[2], 32, glm::mix(kBlue, fogColor, 1.0f - std::exp(-1.25f)));

        // The far wall would be seven tenths fog; it is let be half.
        scene.world.tryGet<FogComponent>(fog)->maxOpacity = 0.5f;
        image = scene.render(wallView(scene));
        checkPixel(image, kWallColumns[1], 32, glm::mix(kGreen, fogColor, 1.0f - std::exp(-0.25f)));
        checkPixel(image, kWallColumns[2], 32, glm::mix(kBlue, fogColor, 0.5f));

        // With no opacity at all it is no fog.
        scene.world.tryGet<FogComponent>(fog)->maxOpacity = 0.0f;
        image = scene.render(wallView(scene));
        checkPixel(image, kWallColumns[1], 32, kGreen);
        checkPixel(image, kWallColumns[2], 32, kBlue);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what a view shows through fog is what the fog on each line of sight says") {
        // Fogs that thin out fast, slowly and not at all; that begin at a distance and
        // are held back; whose height is below everything, in the middle of it and above
        // it all - seen with and without perspective, from above, from inside and from
        // below. The background is behind the fog like everything else.
        const std::vector<PlacedFog> fogs = {
            {.fog = {.color = Vec3(0.6f, 0.7f, 0.8f), .density = 0.08f, .heightFalloff = 0.5f}, .height = 1.0f},
            {.fog = {.color = Vec3(0.2f, 0.5f, 0.3f),
                     .density = 0.03f,
                     .heightFalloff = 0.0f,
                     .startDistance = 6.0f,
                     .maxOpacity = 0.7f},
             .height = 0.0f},
            {.fog = {.color = Vec3(0.7f, 0.7f, 0.6f), .density = 0.02f, .heightFalloff = 0.002f}, .height = 0.0f},
            {.fog = {.color = Vec3(0.5f, 0.6f, 0.9f), .density = 0.01f, .heightFalloff = 0.3f}, .height = 40.0f},
            {.fog = {.color = Vec3(0.9f, 0.8f, 0.7f), .density = 0.6f, .heightFalloff = 0.8f, .startDistance = 2.0f},
             .height = -3.0f},
        };
        FlatScene flat(yard());
        SceneFixture& scene = flat.scene;
        const Entity entity = addFog(scene, {});
        const std::vector<CameraView> views = yardViews(scene);
        for (size_t fogIndex = 0; fogIndex < fogs.size(); ++fogIndex) {
            *scene.world.tryGet<FogComponent>(entity) = fogs[fogIndex].fog;
            scene.world.transform(entity).position.y = fogs[fogIndex].height;
            for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex) {
                CAPTURE(fogIndex);
                CAPTURE(viewIndex);
                const Image image = scene.render(views[viewIndex]);
                checkAgainstFog(image, flat, views[viewIndex], fogOf(fogs[fogIndex].fog, fogs[fogIndex].height),
                                kBackground);
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the sky is behind all the fog there is, and shows through where the fog thins out above") {
        // A color the format for images of light holds exactly.
        const Vec3 sky(0.125f, 0.25f, 0.75f);
        FlatScene flat(yard(), sky);
        SceneFixture& scene = flat.scene;
        const FogComponent fog{.color = Vec3(0.8f, 0.8f, 0.7f), .density = 0.05f, .heightFalloff = 0.4f};
        const Entity entity = addFog(scene, fog, 1.0f);

        for (const CameraView& view : yardViews(scene)) {
            checkAgainstFog(scene.render(view), flat, view, fogOf(fog, 1.0f), sky);
        }

        // Straight up from the fog's height the sky is seen through density / falloff
        // of it, an eighth: a little less than an eighth of it is fog. Level, all is.
        const CameraView up =
            scene.orthographicView(Vec3(60.0f, 1.0f, 0.0f), Vec3(60.0f, 2.0f, 0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f));
        checkPixel(scene.render(up), 32, 32, glm::mix(sky, fog.color, 1.0f - std::exp(-0.125f)));
        const CameraView level = scene.orthographicView(Vec3(0.0f, 50.0f, 0.0f), Vec3(0.0f, 50.0f, -1.0f), 2.0f);
        checkPixel(scene.render(level), 32, 32, fog.color);

        // A fog that is the same at every height hides the sky - unless it is held back.
        *scene.world.tryGet<FogComponent>(entity) = {.color = fog.color, .density = 0.01f, .heightFalloff = 0.0f};
        checkPixel(scene.render(up), 32, 32, fog.color);
        scene.world.tryGet<FogComponent>(entity)->maxOpacity = 0.25f;
        checkPixel(scene.render(up), 32, 32, glm::mix(sky, fog.color, 0.25f));

        // An environment that is not shown lights the scene all the same, and is not what
        // is behind the fog: the background is.
        scene.world.tryGet<EnvironmentComponent>(scene.environmentEntity())->visible = false;
        checkPixel(scene.render(up), 32, 32, glm::mix(kBackground, fog.color, 0.25f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("towards the sun the fog is brighter by what it is told to make of the sun") {
        FlatScene flat(yard());
        SceneFixture& scene = flat.scene;
        // The sun stands low, ahead and to the right.
        const Vec3 towardsSun = glm::normalize(Vec3(0.5f, 0.25f, -1.0f));
        const FogComponent fog{.color = Vec3(0.2f, 0.25f, 0.3f),
                               .density = 0.04f,
                               .heightFalloff = 0.1f,
                               .sunColor = Vec3(0.6f, 0.4f, 0.1f),
                               .sunExponent = 4.0f};
        addFog(scene, fog, 0.5f);
        const CameraView ahead = scene.perspectiveView(Vec3(0.0f, 2.0f, 6.0f), Vec3(2.0f, 3.0f, -10.0f), 70.0f);
        const CameraView behind = scene.perspectiveView(Vec3(0.0f, 2.0f, 6.0f), Vec3(-2.0f, 1.0f, 20.0f), 70.0f);

        // Without a light from one direction the fog has no sun, whatever it would make of one.
        checkAgainstFog(scene.render(ahead), flat, ahead, fogOf(fog, 0.5f), kBackground);

        scene.setSun(towardsSun, Vec3(2.0f));
        const Image image = scene.render(ahead);
        checkAgainstFog(image, flat, ahead, fogOf(fog, 0.5f, towardsSun), kBackground);
        checkAgainstFog(scene.render(behind), flat, behind, fogOf(fog, 0.5f, towardsSun), kBackground);
        // Where the sun is, all of what the fog makes of it is added to the fog's color.
        const std::optional<Vec2> sun = worldToScreen(ahead, ahead.position + towardsSun * 100.0f, Vec2(64.0f));
        REQUIRE(sun.has_value());
        checkPixel(image, static_cast<uint32_t>(sun->x), static_cast<uint32_t>(sun->y),
                   glm::mix(kBackground, fog.color + fog.sunColor,
                            fogAmountTowards(fogOf(fog, 0.5f), ahead.position, towardsSun)));

        // Of two suns the brighter is the fog's, whichever came first.
        const Vec3 towardsOther = glm::normalize(Vec3(-0.6f, 0.3f, -1.0f));
        scene.addLight({.type = LightType::Directional, .intensity = 4.0f}, Vec3(0.0f), -towardsOther);
        checkAgainstFog(scene.render(ahead), flat, ahead, fogOf(fog, 0.5f, towardsOther), kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what lets the view through is in the fog like everything else") {
        // Two walls fifteen metres away: a solid one, which the lighting pass fogs, and
        // one that is drawn forward, though nothing shows through it.
        SceneFixture scene;
        const Vec3 wallColor(0.9f, 0.1f, 0.4f);
        const Vec3 paneColor(0.1f, 0.8f, 0.2f);
        scene.writeFlatMaterial("solid", wallColor);
        scene.writeFlatMaterial("drawnForward", wallColor, R"("alphaMode": "blended")");
        scene.writeMaterial("pane", R"("baseColor": [0, 0, 0, 0.5], "specular": 0, "emissiveColor": [0.1, 0.8, 0.2],
                                      "alphaMode": "blended")");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("solid"), at(Vec3(-0.75f, 0.0f, -10.5f), Vec3(1.5f, 3.0f, 1.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("drawnForward"),
                  at(Vec3(0.75f, 0.0f, -10.5f), Vec3(1.5f, 3.0f, 1.0f)));
        const Vec3 fogColor(0.3f, 0.5f, 0.9f);
        addFog(scene, {.color = fogColor, .density = 0.06f, .heightFalloff = 0.0f});
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 3.0f);

        Image image = scene.render(view);
        const Vec3 foggedWall = glm::mix(wallColor, fogColor, 1.0f - std::exp(-0.06f * 15.0f));
        checkPixel(image, 16, 32, foggedWall);
        checkPixel(image, 48, 32, foggedWall);
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 1);

        // A pane of glass five metres away, in front of the upper half of both, lets
        // half of what is behind it through. What reaches the camera is that half of
        // the wall's light, as the fog of all fifteen metres leaves it, and the pane's
        // own, through the fog of its five.
        scene.add(BuiltinMesh::Cube, scene.material("pane"), at(Vec3(0.0f, 0.75f, -0.5f), Vec3(3.0f, 1.5f, 1.0f)));
        image = scene.render(view);
        const Vec3 foggedPane = glm::mix(paneColor, fogColor, 1.0f - std::exp(-0.06f * 5.0f));
        checkPixel(image, 16, 8, glm::mix(foggedWall, foggedPane, 0.5f));
        checkPixel(image, 48, 8, glm::mix(foggedWall, foggedPane, 0.5f));
        checkPixel(image, 16, 56, foggedWall);
        // Which is what the light does on its way: the wall's goes through ten metres of
        // fog, half of it through the pane with the pane's own added, and all of that
        // through five metres more.
        const float lastFive = std::exp(-0.06f * 5.0f);
        const float firstTen = std::exp(-0.06f * 10.0f);
        const Vec3 atPane = glm::mix(fogColor, wallColor, firstTen);
        const Vec3 behindPane = glm::mix(atPane, paneColor, 0.5f);
        checkPixel(image, 16, 8, glm::mix(fogColor, behindPane, lastFive));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the fog view shows how much of each pixel is fog, and the other debug views show none") {
        FlatScene flat(threeWalls());
        SceneFixture& scene = flat.scene;
        const Vec3 fogColor(0.8f, 0.6f, 0.2f);
        const Entity fog = addFog(scene, {.color = fogColor, .density = 0.05f, .heightFalloff = 0.0f});
        // From a little to the side, so that there is background beside the walls.
        const CameraView view = scene.orthographicView(Vec3(1.0f, 0.0f, 5.0f), Vec3(1.0f, 0.0f, 0.0f), 3.0f);
        // The middle wall is where the left one was, the right one where the middle was.
        const uint32_t middleWall = kWallColumns[0];
        const uint32_t rightWall = kWallColumns[1];
        const uint32_t beside = 60;

        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Fog;
        Image image = scene.render(view, settings);
        checkShown(image, middleWall, 32, Vec3(1.0f - std::exp(-0.75f)));
        checkShown(image, rightWall, 32, Vec3(1.0f - std::exp(-1.75f)));
        // Where nothing is, there is all the fog there can be.
        checkShown(image, beside, 32, Vec3(1.0f));
        scene.world.tryGet<FogComponent>(fog)->maxOpacity = 0.4f;
        image = scene.render(view, settings);
        checkShown(image, rightWall, 32, Vec3(0.4f));
        checkShown(image, beside, 32, Vec3(0.4f));
        scene.world.tryGet<FogComponent>(fog)->maxOpacity = 1.0f;

        // What a surface is has nothing to do with the air in front of it.
        settings.debugView = RenderDebugView::Emissive;
        image = scene.render(view, settings);
        checkPixel(image, middleWall, 32, kGreen);
        checkPixel(image, rightWall, 32, kBlue);
        checkPixel(image, beside, 32, kBackground);
        settings.debugView = RenderDebugView::BaseColor;
        image = scene.render(view, settings);
        checkPixel(image, rightWall, 32, Vec3(0.0f));
        checkPixel(image, beside, 32, kBackground);
        // Nor has what there is to reflect: nothing here, and no fog in front of it.
        settings.debugView = RenderDebugView::Reflections;
        image = scene.render(view, settings);
        checkPixel(image, rightWall, 32, Vec3(0.0f));
        checkPixel(image, beside, 32, kBackground);

        // The light alone is seen through the fog as the picture is: none here, so
        // what is seen is the fog.
        settings.debugView = RenderDebugView::LightingOnly;
        image = scene.render(view, settings);
        checkPixel(image, middleWall, 32, fogColor * (1.0f - std::exp(-0.75f)));
        checkPixel(image, rightWall, 32, fogColor * (1.0f - std::exp(-1.75f)));
        checkPixel(image, beside, 32, fogColor);

        // Without fog the view of it is black, the background too.
        scene.world.setEnabled(fog, false);
        settings.debugView = RenderDebugView::Fog;
        image = scene.render(view, settings);
        checkShown(image, rightWall, 32, Vec3(0.0f));
        checkShown(image, beside, 32, Vec3(0.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a world without fog, or with one that is switched off or has no density, is drawn as it always was") {
        FlatScene flat(yard());
        SceneFixture& scene = flat.scene;
        const CameraView view = yardViews(scene)[0];
        const Image clear = scene.render(view);
        checkAgainstFog(clear, flat, view, FogSettings{}, kBackground);
        const auto isSame = [&](const Image& image) {
            const ImageComparison difference = unwrap(compareImages(clear, image, {.channelTolerance = 0}));
            return difference.differingPixels == 0;
        };

        const Entity fog = addFog(scene, {.density = 0.0f});
        CHECK(isSame(scene.render(view)));
        scene.world.tryGet<FogComponent>(fog)->density = 0.1f;
        CHECK_FALSE(isSame(scene.render(view)));
        scene.world.setEnabled(fog, false);
        CHECK(isSame(scene.render(view)));
        scene.world.setEnabled(fog, true);
        CHECK_FALSE(isSame(scene.render(view)));
        // Numbers a fog cannot have make it no fog, or the nearest that it can be.
        scene.world.tryGet<FogComponent>(fog)->density = std::numeric_limits<float>::quiet_NaN();
        CHECK(isSame(scene.render(view)));
        scene.world.tryGet<FogComponent>(fog)->density = -4.0f;
        CHECK(isSame(scene.render(view)));
        *scene.world.tryGet<FogComponent>(fog) = {.color = Vec3(0.5f, 0.6f, 0.7f),
                                                  .density = 0.05f,
                                                  .heightFalloff = -3.0f,
                                                  .startDistance = -8.0f,
                                                  .maxOpacity = 9.0f,
                                                  .sunExponent = -2.0f};
        checkAgainstFog(scene.render(view), flat, view,
                        fogOf({.color = Vec3(0.5f, 0.6f, 0.7f), .density = 0.05f, .heightFalloff = 0.0f}, 0.0f),
                        kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("however dense, deep or far away: a fog leaves numbers in every pixel") {
        // The densest fog a scene can have, thinning out as fast as one can, with a sun
        // far brighter than white in it; seen from far above its height, from far below
        // and from inside, with the fog beginning at once and a long way off.
        FlatScene flat(yard());
        SceneFixture& scene = flat.scene;
        const Vec3 towardsSun = glm::normalize(Vec3(0.2f, 0.4f, -1.0f));
        scene.setSun(towardsSun, Vec3(3.0f));
        const Entity entity = addFog(scene, {});
        const std::vector<CameraView> views = yardViews(scene);
        for (const float density : {10.0f, 1.0e-4f}) {
            for (const float falloff : {10.0f, 0.0f}) {
                for (const float height : {-4000.0f, 0.5f, 4000.0f}) {
                    for (const float start : {0.0f, 20.0f}) {
                        const FogComponent fog{.color = Vec3(0.5f, 0.6f, 0.7f),
                                               .density = density,
                                               .heightFalloff = falloff,
                                               .startDistance = start,
                                               .sunColor = Vec3(4000.0f, 3000.0f, 1000.0f),
                                               .sunExponent = 64.0f};
                        *scene.world.tryGet<FogComponent>(entity) = fog;
                        scene.world.transform(entity).position.y = height;
                        for (size_t viewIndex = 0; viewIndex < views.size(); ++viewIndex) {
                            CAPTURE(density);
                            CAPTURE(falloff);
                            CAPTURE(height);
                            CAPTURE(start);
                            CAPTURE(viewIndex);
                            const Image image = scene.render(views[viewIndex]);
                            CHECK(sceneColorIsFinite(scene));
                            checkAgainstFog(image, flat, views[viewIndex], fogOf(fog, height, towardsSun), kBackground,
                                            7);
                        }
                    }
                }
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }
}
