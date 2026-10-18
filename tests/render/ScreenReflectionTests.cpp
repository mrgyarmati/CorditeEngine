#include "cordite/assets/Image.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/Fog.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/ScreenReflections.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::HalfPicture;
using cordite::test::SceneFixture;
using cordite::test::sceneLight;
using cordite::test::unwrap;

namespace {

constexpr UVec2 kSize(128, 72);
const Vec3 kEye(0.0f, 1.5f, 3.0f);
const Vec3 kLookAt(0.0f, 0.8f, -2.0f);
/// What the wall gives off: what a mirror shows of it.
const Vec3 kWallColor(0.2f, 0.8f, 0.4f);

/// What the strip gives off.
const Vec3 kStripColor(1.0f, 0.0f, 0.0f);

/// A floor of white metal ten metres a side, as rough as `roughness`; unless told otherwise,
/// a wall of light at z = -2 that stands on it, six metres wide and two high. When asked: a
/// sky of light from all around; a strip of light two metres in front of the wall, twenty
/// centimetres high and ten thick, from 0.9 to 1.1 metres up; a floor whose material says
/// that half of the light from all around reaches it. Nothing else lights anything.
struct MirrorScene {
    float roughness = 0.0f;
    bool wall = true;
    /// How wide the wall is, in metres.
    float wallWidth = 6.0f;
    bool sky = false;
    bool strip = false;
    bool halfOccluded = false;
};

void build(SceneFixture& scene, const MirrorScene& mirror) {
    scene.writeFlatMaterial("wall", kWallColor);
    scene.writeFlatMaterial("strip", kStripColor);
    if (mirror.halfOccluded) {
        Image half = unwrap(Image::create(4, 4, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                half.setPixel(x, y, Vec4(0.5f, 0.5f, 0.5f, 1.0f));
            }
        }
        scene.writeImage("half", half);
    }
    if (mirror.sky) {
        // Brighter the higher it is, so that a mirror shows something of it everywhere.
        Image sky = unwrap(Image::create(16, 8, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < sky.height(); ++y) {
            for (uint32_t x = 0; x < sky.width(); ++x) {
                const float up = 1.0f - static_cast<float>(y) / static_cast<float>(sky.height());
                sky.setPixel(x, y, Vec4(0.2f + 0.6f * up, 0.3f + 0.4f * up, 0.6f, 1.0f));
            }
        }
        scene.writeHdrImage("sky", sky);
    }
    scene.open();
    // The floor names the texture of its occlusion by its id, which there is once the
    // assets have been read.
    const std::string occlusion =
        mirror.halfOccluded ? fmt::format(R"(, "occlusionTexture": "{}")", scene.asset("half.png").toString()) : "";
    scene.writeMaterial("floor", fmt::format(R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": {}{})",
                                             mirror.roughness, occlusion));
    REQUIRE(scene.assets->refresh().has_value());
    scene.add(BuiltinMesh::Plane, scene.material("floor"), at(Vec3(0.0f), Vec3(10.0f, 1.0f, 10.0f)));
    if (mirror.wall) {
        scene.add(BuiltinMesh::Cube, scene.material("wall"),
                  at(Vec3(0.0f, 1.0f, -2.05f), Vec3(mirror.wallWidth, 2.0f, 0.1f)));
    }
    if (mirror.strip) {
        scene.add(BuiltinMesh::Cube, scene.material("strip"), at(Vec3(0.0f, 1.0f, -0.05f), Vec3(6.0f, 0.2f, 0.1f)));
    }
    if (mirror.sky) {
        scene.setEnvironment(scene.asset("sky.hdr"));
    }
}

/// The camera of the mirror scene: what it sees ends at `farPlane` metres.
CameraView mirrorView(float farPlane = 500.0f) {
    CameraComponent camera;
    camera.verticalFovDegrees = 60.0f;
    camera.farPlane = farPlane;
    return makeCameraView(glm::inverse(lookAt(kEye, kLookAt, kWorldUp)), camera,
                          static_cast<float>(kSize.x) / static_cast<float>(kSize.y));
}

/// Settings that draw the reflections from the screen at `quality`, in a look whose other
/// screen effects take nothing away: what a pixel must be follows from the reflections.
SceneRenderSettings reflecting(ScreenEffectsQuality quality = ScreenEffectsQuality::High, float intensity = 1.0f) {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.screenEffectsQuality = quality;
    PostProcessSettings look;
    look.ambientOcclusionIntensity = 0.0f;
    look.reflectionIntensity = intensity;
    settings.postProcess = look;
    return settings;
}

/// Where the picture puts a point of the world: from (0, 0) at the top left to (1, 1).
Vec2 placeInPicture(const CameraView& view, const Vec3& point) {
    const Vec4 clip = view.viewProjection * Vec4(point, 1.0f);
    return {clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f};
}

/// What the line of sight through the middle of the pixel at (x, y) does in the mirror
/// scene: where it meets the floor, and where the mirror sends it on to the plane of the
/// wall - and where the picture shows that.
struct MirrorSight {
    bool onFloor = false;
    /// Whether it meets the floor about where the wall stands on it: what the pixel shows
    /// there may be either.
    bool atFootOfWall = false;
    Vec3 floor{0.0f};
    Vec3 onWallPlane{0.0f, 100.0f, 0.0f};
    Vec2 wallUv{-1.0f};
};

MirrorSight sightThrough(const CameraView& view, uint32_t x, uint32_t y) {
    // From where the pixel's line of sight crosses the near plane, through a point of it
    // further on: the camera's own position with perspective, and along the way the camera
    // looks without.
    const Vec2 uv = (Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) / Vec2(kSize);
    const auto pointAt = [&](float depth) {
        const Vec4 point = view.inverseViewProjection * Vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
        return Vec3(point) / point.w;
    };
    const Vec3 near = pointAt(1.0f);
    const Vec3 sight = glm::normalize(pointAt(0.5f) - near);
    MirrorSight result;
    if (sight.y >= 0.0f) {
        return result;
    }
    const Vec3 floor = near + sight * (-near.y / sight.y);
    result.atFootOfWall = std::abs(floor.z + 2.0f) < 0.3f;
    // What the picture shows of the floor: in front of the wall.
    if (floor.z <= -2.0f || std::abs(floor.x) > 5.0f || floor.z > 5.0f) {
        return result;
    }
    result.onFloor = true;
    result.floor = floor;
    const Vec3 mirrored = glm::reflect(sight, kWorldUp);
    if (mirrored.z < 0.0f) {
        result.onWallPlane = floor + mirrored * ((-2.0f - floor.z) / mirrored.z);
        result.wallUv = placeInPicture(view, result.onWallPlane);
    }
    return result;
}

/// Whether the mirror sends the line of sight well inside the wall, to a place well inside
/// the picture - where what was met is trusted fully - from a floor that is not at the foot
/// of the wall.
bool seesWall(const MirrorSight& sight, float margin = 0.15f) {
    return sight.onFloor && sight.floor.z > -1.8f && sight.onWallPlane.y > margin &&
           sight.onWallPlane.y < 2.0f - margin && std::abs(sight.onWallPlane.x) < 3.0f - margin &&
           sight.wallUv.x > 0.12f && sight.wallUv.x < 0.88f && sight.wallUv.y > 0.12f && sight.wallUv.y < 0.88f;
}

/// Whether the mirror sends the line of sight over the top of the wall, into the sky.
bool seesOverWall(const MirrorSight& sight) {
    return sight.onFloor && sight.onWallPlane.y > 2.15f;
}

/// Counts what is held against what, and says where the first that differs is.
struct Tally {
    uint32_t checked = 0;
    uint32_t wrong = 0;
    std::string first;

    void hold(bool right, uint32_t x, uint32_t y, const Vec3& seen) {
        ++checked;
        if (!right && wrong++ == 0) {
            first = fmt::format("({}, {}): {} {} {}", x, y, seen.r, seen.g, seen.b);
        }
    }
};

} // namespace

TEST_SUITE("ScreenReflections") {
    TEST_CASE("a mirror floor reflects the wall that stands on it where the mirror image of the wall is, at every "
              "quality, and over the wall the sky as it did") {
        SceneFixture scene(kSize);
        build(scene, {.sky = true});
        const CameraView view = mirrorView();
        scene.render(view, reflecting(ScreenEffectsQuality::Off));
        const HalfPicture without = sceneLight(scene);
        for (const ScreenEffectsQuality quality :
             {ScreenEffectsQuality::Low, ScreenEffectsQuality::Medium, ScreenEffectsQuality::High}) {
            CAPTURE(nameOf(quality));
            scene.render(view, reflecting(quality));
            const HalfPicture light = sceneLight(scene);
            Tally wall;
            Tally sky;
            for (uint32_t y = 0; y < kSize.y; ++y) {
                for (uint32_t x = 0; x < kSize.x; ++x) {
                    const MirrorSight sight = sightThrough(view, x, y);
                    const Vec3 seen = light.at(x, y);
                    if (seesWall(sight)) {
                        // A white mirror throws back all the light it is sent: of the wall,
                        // and nothing of the sky it reflected before.
                        wall.hold(glm::distance(seen, kWallColor) < 0.03f, x, y, seen);
                    } else if (seesOverWall(sight)) {
                        sky.hold(seen == without.at(x, y), x, y, seen);
                    }
                }
            }
            CAPTURE(wall.first);
            CAPTURE(sky.first);
            CHECK(wall.checked > 1000);
            CHECK(wall.wrong == 0);
            CHECK(sky.checked > 100);
            CHECK(sky.wrong == 0);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("towards the borders of the picture what the screen gives fades into what the surroundings give") {
        // A wall wider than the picture: what the floor reflects of it reaches the borders.
        SceneFixture scene(kSize);
        build(scene, {.wallWidth = 14.0f});
        const CameraView view = mirrorView();
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        Tally inside;
        Tally fading;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                const MirrorSight sight = sightThrough(view, x, y);
                if (!sight.onFloor || sight.floor.z < -1.8f || sight.onWallPlane.y < 0.15f ||
                    sight.onWallPlane.y > 1.85f || sight.wallUv.y < 0.12f || sight.wallUv.y > 0.88f) {
                    continue;
                }
                const Vec3 seen = light.at(x, y);
                const float acrossToBorder = std::min(sight.wallUv.x, 1.0f - sight.wallUv.x);
                if (acrossToBorder > 0.12f) {
                    inside.hold(glm::distance(seen, kWallColor) < 0.03f, x, y, seen);
                } else if (acrossToBorder > 0.01f && acrossToBorder < 0.09f) {
                    // Where the reflection is found is within a pixel of its mirror image: the
                    // fade, a tenth of the picture wide, can differ by a twelfth of its way.
                    const Vec3 expected = kWallColor * screenEdgeFade(sight.wallUv);
                    fading.hold(glm::distance(seen, expected) < 0.07f, x, y, seen);
                }
            }
        }
        CAPTURE(inside.first);
        CAPTURE(fading.first);
        CHECK(inside.checked > 1000);
        CHECK(inside.wrong == 0);
        CHECK(fading.checked > 50);
        CHECK(fading.wrong == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a reflection that goes on past the far plane meets nothing there: the sky is not something drawn") {
        // What is further than six metres is not drawn: the far floor is sky.
        SceneFixture scene(kSize);
        build(scene, {.sky = true});
        const CameraView view = mirrorView(6.0f);
        scene.render(view, reflecting(ScreenEffectsQuality::Off));
        const HalfPicture without = sceneLight(scene);
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        Tally sky;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                if (seesOverWall(sightThrough(view, x, y))) {
                    const Vec3 seen = light.at(x, y);
                    sky.hold(seen == without.at(x, y), x, y, seen);
                }
            }
        }
        CAPTURE(sky.first);
        CHECK(sky.checked > 100);
        CHECK(sky.wrong == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("without perspective a mirror floor reflects the wall that stands on it as well") {
        SceneFixture scene(kSize);
        build(scene, {});
        // From in front and above, every line of sight along the way the camera looks.
        // Their reflections, parallel as well, all meet the wall.
        const CameraView view = scene.orthographicView(Vec3(0.0f, 3.0f, 4.0f), Vec3(0.0f, 0.8f, -1.0f), 4.0f);
        REQUIRE(view.orthographic);
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        Tally wall;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                const MirrorSight sight = sightThrough(view, x, y);
                const Vec3 seen = light.at(x, y);
                if (seesWall(sight)) {
                    wall.hold(glm::distance(seen, kWallColor) < 0.03f, x, y, seen);
                }
            }
        }
        CAPTURE(wall.first);
        CHECK(wall.checked > 1000);
        CHECK(wall.wrong == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a rough floor takes less from the screen, and one rougher than the most nothing") {
        for (const float roughness : {0.3f, 0.5f}) {
            CAPTURE(roughness);
            SceneFixture scene(kSize);
            build(scene, {.roughness = roughness});
            const CameraView view = mirrorView();
            scene.render(view, reflecting());
            const HalfPicture light = sceneLight(scene);
            // Up to 0.4 a surface takes from the screen, all of it up to 0.2: at 0.3 half.
            // Of the light it reflects, a white metal throws back one less 0.55 times its
            // roughness (environmentReflectance in Brdf.slang).
            const Vec3 expected = roughness < 0.4f ? kWallColor * 0.5f * (1.0f - 0.55f * roughness) : Vec3(0.0f);
            Tally wall;
            for (uint32_t y = 0; y < kSize.y; ++y) {
                for (uint32_t x = 0; x < kSize.x; ++x) {
                    // A rough surface reflects a little towards its normal: a wider margin.
                    if (seesWall(sightThrough(view, x, y), 0.5f)) {
                        const Vec3 seen = light.at(x, y);
                        wall.hold(glm::distance(seen, expected) < 0.03f, x, y, seen);
                    }
                }
            }
            CAPTURE(wall.first);
            CHECK(wall.checked > 500);
            CHECK(wall.wrong == 0);
            checkNoValidationIssues(*scene.context.graphics);
        }
    }

    TEST_CASE("what a surface says of its own occlusion dims what the screen gives, as it dims its surroundings") {
        SceneFixture scene(kSize);
        build(scene, {.sky = true, .halfOccluded = true});
        const CameraView view = mirrorView();
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        // Half, as eight bits hold it - of the wall, not of the sky.
        const Vec3 expected = kWallColor * (128.0f / 255.0f);
        Tally wall;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                if (seesWall(sightThrough(view, x, y))) {
                    const Vec3 seen = light.at(x, y);
                    wall.hold(glm::distance(seen, expected) < 0.02f, x, y, seen);
                }
            }
        }
        CAPTURE(wall.first);
        CHECK(wall.checked > 1000);
        CHECK(wall.wrong == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a reflection that passes behind something thinner than the thickness goes on to the wall behind it") {
        SceneFixture scene(kSize);
        build(scene, {.strip = true});
        const CameraView view = mirrorView();
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        // The top of the strip in the picture, at its front.
        const float stripTop = placeInPicture(view, Vec3(0.0f, 1.1f, 0.0f)).y;
        Tally wall;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                const MirrorSight sight = sightThrough(view, x, y);
                if (!seesWall(sight) || sight.floor.z < 0.1f) {
                    continue;
                }
                // Where the reflection passes the strip, a metre and more under it, and the
                // wall it goes on to seen over the top of the strip: on its way across the
                // picture it passes behind the strip, by more than the thickness.
                const Vec3 mirrored = glm::normalize(sight.onWallPlane - sight.floor);
                const float underStrip = sight.floor.y + mirrored.y * (-sight.floor.z / mirrored.z);
                if (underStrip > 0.8f || sight.wallUv.y > stripTop - 2.0f / static_cast<float>(kSize.y)) {
                    continue;
                }
                const Vec3 seen = light.at(x, y);
                wall.hold(glm::distance(seen, kWallColor) < 0.03f, x, y, seen);
            }
        }
        CAPTURE(wall.first);
        CHECK(wall.checked > 200);
        CHECK(wall.wrong == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what the screen gives is seen through the fog, as the light of the floor is") {
        SceneFixture scene(kSize);
        build(scene, {});
        FogComponent component;
        component.color = Vec3(0.3f, 0.3f, 0.35f);
        component.density = 0.1f;
        const Entity entity = scene.world.createEntity("Fog");
        // At its densest up to five metres: all of the scene is in it alike.
        scene.world.transform(entity).position = Vec3(0.0f, 5.0f, 0.0f);
        scene.world.add<FogComponent>(entity) = component;
        FogSettings fog;
        fog.color = component.color;
        fog.density = component.density;
        fog.height = 5.0f;
        fog.heightFalloff = component.heightFalloff;
        fog.startDistance = component.startDistance;
        fog.maxOpacity = component.maxOpacity;
        fog.sunExponent = component.sunExponent;

        const CameraView view = mirrorView();
        scene.render(view, reflecting());
        const HalfPicture light = sceneLight(scene);
        Tally wall;
        float largestFog = 0.0f;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                const MirrorSight sight = sightThrough(view, x, y);
                if (!seesWall(sight)) {
                    continue;
                }
                // The floor in its fog, and the wall in the fog between the camera and the
                // wall - the light the picture holds where the reflection met it - let
                // through by the fog between the camera and the floor.
                const Vec3 met = applyFog(fog, kWallColor, kEye, sight.onWallPlane);
                const float through = 1.0f - fogAmount(fog, kEye, sight.floor);
                const Vec3 expected = applyFog(fog, Vec3(0.0f), kEye, sight.floor) + met * through;
                largestFog = std::max(largestFog, 1.0f - through);
                const Vec3 seen = light.at(x, y);
                wall.hold(glm::distance(seen, expected) < 0.03f, x, y, seen);
            }
        }
        CAPTURE(wall.first);
        CHECK(wall.checked > 1000);
        CHECK(wall.wrong == 0);
        // Enough fog that leaving out what it takes would not pass.
        CHECK(largestFog > 0.2f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the view of the screen reflections shows what they put in place, over black") {
        SceneFixture scene(kSize);
        build(scene, {.sky = true});
        const CameraView view = mirrorView();
        SceneRenderSettings settings = reflecting();
        settings.debugView = RenderDebugView::ScreenReflections;
        scene.render(view, settings);
        const HalfPicture light = sceneLight(scene);
        Tally wall;
        Tally rest;
        for (uint32_t y = 0; y < kSize.y; ++y) {
            for (uint32_t x = 0; x < kSize.x; ++x) {
                const MirrorSight sight = sightThrough(view, x, y);
                const Vec3 seen = light.at(x, y);
                if (seesWall(sight)) {
                    wall.hold(glm::distance(seen, kWallColor) < 0.03f, x, y, seen);
                } else if ((!sight.onFloor && !sight.atFootOfWall) || seesOverWall(sight)) {
                    // The wall, the sky, and the floor that reflects the sky: nothing met.
                    rest.hold(seen == Vec3(0.0f), x, y, seen);
                }
            }
        }
        CAPTURE(wall.first);
        CAPTURE(rest.first);
        CHECK(wall.checked > 1000);
        CHECK(wall.wrong == 0);
        CHECK(rest.checked > 2000);
        CHECK(rest.wrong == 0);

        // A view that draws none shows nothing.
        settings.screenEffectsQuality = ScreenEffectsQuality::Off;
        scene.render(view, settings);
        const HalfPicture none = sceneLight(scene);
        CHECK(none.at(64, 60) == Vec3(0.0f));
        CHECK(none.at(64, 20) == Vec3(0.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("where reflections meet nothing, and where none are asked for, the light is what it was, to the bit") {
        // A floor under a sky that it reflects everywhere: every reflection goes up into it.
        {
            SceneFixture scene(kSize);
            build(scene, {.wall = false, .sky = true});
            const CameraView view = mirrorView();
            scene.render(view, reflecting(ScreenEffectsQuality::Off));
            const std::vector<std::byte> without = sceneLight(scene).pixels.data;
            const uint32_t passesWithout = scene.renderer->statistics().screenPasses;
            scene.render(view, reflecting(ScreenEffectsQuality::High));
            CHECK(sceneLight(scene).pixels.data == without);
            // It was drawn, and found nothing.
            CHECK(scene.renderer->statistics().screenPasses == passesWithout + 2);
            CHECK(sceneLight(scene).at(64, 60) != Vec3(0.0f));
            checkNoValidationIssues(*scene.context.graphics);
        }
        // With the wall, but asked for none: nothing is drawn.
        SceneFixture scene(kSize);
        build(scene, {.sky = true});
        const CameraView view = mirrorView();
        scene.render(view, reflecting(ScreenEffectsQuality::Off));
        const std::vector<std::byte> without = sceneLight(scene).pixels.data;
        const uint32_t passesWithout = scene.renderer->statistics().screenPasses;
        scene.render(view, reflecting(ScreenEffectsQuality::High, 0.0f));
        CHECK(sceneLight(scene).pixels.data == without);
        CHECK(scene.renderer->statistics().screenPasses == passesWithout);
        CHECK(scene.renderer->statistics().screenReflectionBytes == 0);
        SceneRenderSettings switchedOff = reflecting();
        switchedOff.screenReflections = false;
        scene.render(view, switchedOff);
        CHECK(sceneLight(scene).pixels.data == without);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a view of which nothing can be smooth enough draws no reflections, and a smooth decal or a texture "
              "that may be smooth is enough") {
        // A floor rougher than the roughest that takes reflections, and a wall of light whose
        // material is as rough as a material is when it says nothing.
        SceneFixture scene(kSize);
        build(scene, {.roughness = 0.6f});
        const CameraView view = mirrorView();
        scene.render(view, reflecting(ScreenEffectsQuality::Off));
        const uint32_t passesWithout = scene.renderer->statistics().screenPasses;
        const auto drawsReflections = [&] {
            scene.render(view, reflecting());
            const SceneRenderStatistics& statistics = scene.renderer->statistics();
            CHECK((statistics.screenReflectionBytes > 0) == (statistics.screenPasses == passesWithout + 2));
            return statistics.screenPasses == passesWithout + 2;
        };
        CHECK_FALSE(drawsReflections());

        // A decal of a smooth material makes the floor smooth where it lands.
        scene.writeMaterial("wet", R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": 0.1)");
        REQUIRE(scene.assets->refresh().has_value());
        const Entity decal = scene.world.createEntity("Wet");
        scene.world.transform(decal) = at(Vec3(0.0f, 0.0f, 0.5f), Vec3(2.0f, 2.0f, 1.0f),
                                          lookRotation(Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f)));
        scene.world.add<DecalComponent>(decal) = {.material = scene.material("wet")};
        CHECK(drawsReflections());
        scene.world.destroyEntity(decal);
        CHECK_FALSE(drawsReflections());

        // So may a texture of roughness, which says how rough each texel is - here as rough as
        // ever - times the material's roughness: nothing is asked of its texels.
        Image rough = unwrap(Image::create(4, 4, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                rough.setPixel(x, y, Vec4(1.0f));
            }
        }
        scene.writeImage("rough", rough);
        REQUIRE(scene.assets->refresh().has_value());
        scene.writeMaterial("floor", fmt::format(R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": 0.6, )"
                                                 R"("metallicRoughnessTexture": "{}")",
                                                 scene.asset("rough.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        CHECK(drawsReflections());
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the reflections of a view take a copy of its light and an image of what was found, as large as its "
              "quality says") {
        SceneFixture scene(kSize);
        build(scene, {});
        const CameraView view = mirrorView();
        const uint64_t copy = uint64_t{kSize.x} * kSize.y * 8;
        scene.render(view, reflecting(ScreenEffectsQuality::High));
        CHECK(scene.renderer->statistics().screenReflectionBytes == copy + copy);
        scene.render(view, reflecting(ScreenEffectsQuality::Medium));
        CHECK(scene.renderer->statistics().screenReflectionBytes == copy + uint64_t{kSize.x / 2} * (kSize.y / 2) * 8);
        // And gives both back when it draws none - also in a debug view that draws the meshes
        // themselves, which has no G-buffer to reflect in.
        scene.render(view, reflecting(ScreenEffectsQuality::Off));
        CHECK(scene.renderer->statistics().screenReflectionBytes == 0);
        CHECK(scene.targets.reflectionSource() == nullptr);
        scene.render(view, reflecting());
        REQUIRE(scene.targets.reflectionSource() != nullptr);
        SceneRenderSettings wireframe = reflecting();
        wireframe.debugView = RenderDebugView::Wireframe;
        scene.render(view, wireframe);
        CHECK(scene.renderer->statistics().screenReflectionBytes == 0);
        CHECK(scene.targets.reflectionSource() == nullptr);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
