#include "cordite/assets/Image.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Color.h"
#include "cordite/math/Equirectangular.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Projection.h"
#include "cordite/math/SphericalHarmonics.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentLight.h"
#include "cordite/renderer/EnvironmentMaps.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <glm/gtc/packing.hpp>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::displayed;
using cordite::test::kBackground;
using cordite::test::LogCapture;
using cordite::test::recordAndExecute;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// An image of everything around, `width` pixels wide and half as high, in which each
/// direction holds the light `light` says.
Image makeEnvironment(uint32_t width, const std::function<Vec3(const Vec3&)>& light,
                      ImageFormat format = ImageFormat::Rgba32F) {
    Image image = unwrap(Image::create(width, width / 2, format));
    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            const Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(image.width()),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height()));
            image.setPixel(x, y, Vec4(light(equirectDirection(uv)), 1.0f));
        }
    }
    return image;
}

// Six colors, one for each way along an axis, that the format for images of light holds
// exactly: three quarters of a primary color for the positive ways, and of what is left
// of white without it for the negative ones.
constexpr Vec3 kPlusX(0.75f, 0.0f, 0.0f);
constexpr Vec3 kMinusX(0.0f, 0.75f, 0.75f);
constexpr Vec3 kPlusY(0.0f, 0.75f, 0.0f);
constexpr Vec3 kMinusY(0.75f, 0.0f, 0.75f);
constexpr Vec3 kPlusZ(0.0f, 0.0f, 0.75f);
constexpr Vec3 kMinusZ(0.75f, 0.75f, 0.0f);

/// The color of the axis a direction runs along most: a world of six patches, which
/// shows which way round an environment is.
Vec3 patchOf(const Vec3& direction) {
    const Vec3 size = glm::abs(direction);
    if (size.x >= size.y && size.x >= size.z) {
        return direction.x > 0.0f ? kPlusX : kMinusX;
    }
    if (size.y >= size.z) {
        return direction.y > 0.0f ? kPlusY : kMinusY;
    }
    return direction.z > 0.0f ? kPlusZ : kMinusZ;
}

/// A color for every direction: its x, y and z as red, green and blue around a grey.
/// Nothing about which way round an environment is can hide from it.
Vec3 colorOfDirection(const Vec3& direction) {
    return Vec3(0.375f) + 0.375f * direction;
}

/// A sky of `sky` above the horizon and nothing below.
std::function<Vec3(const Vec3&)> skyAbove(const Vec3& sky) {
    return [sky](const Vec3& direction) { return direction.y > 0.0f ? sky : Vec3(0.0f); };
}

std::function<Vec3(const Vec3&)> allAround(const Vec3& light) {
    return [light](const Vec3& /*direction*/) { return light; };
}

/// Writes `<name>.material.json`: a metal that reflects all the light there is, as
/// rough as `roughness` says.
void writeMetal(const SceneFixture& scene, std::string_view name, float roughness) {
    scene.writeMaterial(name, fmt::format(R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": {})", roughness));
}

/// The views of the tests that look at a cube of two metres: at its front, from above
/// and from below.
struct CubeViews {
    explicit CubeViews(const SceneFixture& scene)
        : front(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f)),
          above(scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f))),
          below(scene.orthographicView(Vec3(0.0f, -5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, 1.0f))) {}

    CameraView front;
    CameraView above;
    CameraView below;
};

/// Settings that show the reflections alone, as numbers.
SceneRenderSettings reflectionSettings() {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.debugView = RenderDebugView::Reflections;
    return settings;
}

} // namespace

TEST_SUITE("Environment") {
    TEST_CASE("an even environment lights a white, matte surface to its own brightness, and is the sky") {
        const Vec3 light(0.25f, 0.5f, 0.75f);
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeHdrImage("even", makeEnvironment(64, allAround(light)));
        scene.open();
        // A cube that leaves the corners of the picture to the sky.
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CubeViews views(scene);
        scene.setEnvironment(scene.asset("even.hdr"));

        for (const CameraView* view : {&views.front, &views.above, &views.below}) {
            const Image image = scene.render(*view);
            checkPixel(image, 32, 32, light);
            checkPixel(image, 4, 4, light);
            checkPixel(image, 60, 58, light);
        }

        // Its intensity scales what it lights and what is seen of it.
        scene.setEnvironment(scene.asset("even.hdr")).intensity = 0.5f;
        Image image = scene.render(views.front);
        checkPixel(image, 32, 32, light * 0.5f);
        checkPixel(image, 4, 4, light * 0.5f);

        // Not seen, it lights the scene all the same; the background shows where nothing is.
        scene.setEnvironment(scene.asset("even.hdr")).visible = false;
        image = scene.render(views.front);
        checkPixel(image, 32, 32, light);
        checkPixel(image, 4, 4, kBackground);

        // Without light it is no sky either, seen or not.
        scene.setEnvironment(scene.asset("even.hdr")).intensity = 0.0f;
        image = scene.render(views.front);
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkPixel(image, 4, 4, kBackground);

        // Switched off, it is as if there were none; so it is without an image.
        scene.setEnvironment(scene.asset("even.hdr"));
        scene.world.setEnabled(scene.environmentEntity(), false);
        image = scene.render(views.front);
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkPixel(image, 4, 4, kBackground);
        scene.world.setEnabled(scene.environmentEntity(), true);
        scene.setEnvironment(Uuid());
        image = scene.render(views.front);
        checkPixel(image, 32, 32, Vec3(0.0f));
        checkPixel(image, 4, 4, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("under a sky above and nothing below, a matte surface is lit by how much of the sky it faces") {
        const Vec3 sky(0.8f, 0.6f, 0.4f);
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeMatteMaterial("half", Vec3(0.5f, 1.0f, 0.25f));
        scene.writeHdrImage("sky", makeEnvironment(64, skyAbove(sky)));
        scene.open();
        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(2.0f)));
        const CubeViews views(scene);
        scene.setEnvironment(scene.asset("sky.hdr"));

        // All of it facing up, half of it facing the horizon, none of it facing down.
        checkPixel(scene.render(views.above), 32, 32, sky);
        checkPixel(scene.render(views.front), 32, 32, sky * 0.5f);
        checkPixel(scene.render(views.below), 32, 32, Vec3(0.0f));

        // Tilted by thirty degrees, the top sees a little less: half of one plus the
        // cosine of the tilt.
        scene.world.transform(cube).rotation = glm::angleAxis(toRadians(30.0f), Vec3(1.0f, 0.0f, 0.0f));
        checkPixel(scene.render(views.above), 32, 32, sky * (0.5f * (1.0f + std::cos(toRadians(30.0f)))));
        scene.world.transform(cube).rotation = kIdentityQuat;

        // The surface throws back as much of it as its color says.
        scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material("half")};
        checkPixel(scene.render(views.above), 32, 32, sky * Vec3(0.5f, 1.0f, 0.25f));

        // Ambient light adds to it.
        scene.setAmbient(Vec3(0.1f, 0.2f, 0.3f), Vec3(0.0f));
        checkPixel(scene.render(views.above), 32, 32, (sky + Vec3(0.1f, 0.2f, 0.3f)) * Vec3(0.5f, 1.0f, 0.25f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the sky is the right way round: -Z ahead in the middle of the image, +X to its right, +Y at the top") {
        SceneFixture scene;
        scene.writeHdrImage("patches", makeEnvironment(64, patchOf));
        scene.open();
        scene.setEnvironment(scene.asset("patches.hdr"));

        // A wide view from the middle of the world along -Z: ahead in the middle, and at
        // the edges what is to the right, to the left, above and below.
        Image image = scene.render(scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 120.0f));
        checkPixel(image, 32, 32, kMinusZ);
        checkPixel(image, 60, 32, kPlusX);
        checkPixel(image, 3, 32, kMinusX);
        checkPixel(image, 32, 3, kPlusY);
        checkPixel(image, 32, 60, kMinusY);

        // Along +X: what was to the right is ahead, and what was ahead is to the left.
        image = scene.render(scene.perspectiveView(Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), 120.0f));
        checkPixel(image, 32, 32, kPlusX);
        checkPixel(image, 60, 32, kPlusZ);
        checkPixel(image, 3, 32, kMinusZ);
        checkPixel(image, 32, 3, kPlusY);

        // Along +Z, where the left and the right edge of the image meet: no seam shows.
        image = scene.render(scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f), 120.0f));
        checkPixel(image, 60, 32, kMinusX);
        checkPixel(image, 3, 32, kPlusX);
        for (uint32_t y = 18; y < 46; ++y) {
            for (const uint32_t x : {30u, 31u, 32u, 33u}) {
                checkPixel(image, x, y, kPlusZ);
            }
        }

        // Where the camera is does not matter - the sky is infinitely far away - and it
        // stays in place far from the middle of the world, where numbers get coarse.
        image = scene.render(
            scene.perspectiveView(Vec3(12000.0f, -4000.0f, 30000.0f), Vec3(12000.0f, -4000.0f, 29999.0f), 120.0f));
        checkPixel(image, 32, 32, kMinusZ);
        checkPixel(image, 60, 32, kPlusX);
        checkPixel(image, 32, 3, kPlusY);

        // A camera without perspective looks the same way everywhere: one color.
        const CubeViews views(scene);
        image = scene.render(views.above);
        checkPixel(image, 3, 3, kMinusY);
        checkPixel(image, 60, 60, kMinusY);
        image = scene.render(views.below);
        checkPixel(image, 32, 32, kPlusY);
        image = scene.render(views.front);
        checkPixel(image, 5, 50, kMinusZ);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a view that reaches infinitely far sees the same sky") {
        SceneFixture scene;
        scene.writeHdrImage("patches", makeEnvironment(64, patchOf));
        scene.open();
        scene.setEnvironment(scene.asset("patches.hdr"));

        // Looking along +X and up a little, with a projection that has no far plane.
        CameraView view;
        view.position = Vec3(3.0f, 2.0f, 1.0f);
        view.forward = Vec3(1.0f, 0.0f, 0.0f);
        view.view = lookAt(view.position, view.position + view.forward, kWorldUp);
        view.projection = perspectiveInfiniteReverseZ(toRadians(120.0f), 1.0f, 0.1f);
        view.viewProjection = view.projection * view.view;
        view.inverseViewProjection = glm::inverse(view.viewProjection);
        const Image image = scene.render(view);
        checkPixel(image, 32, 32, kPlusX);
        checkPixel(image, 60, 32, kPlusZ);
        checkPixel(image, 3, 32, kMinusZ);
        checkPixel(image, 32, 3, kPlusY);
        checkPixel(image, 32, 60, kMinusY);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a mirror shows the environment where its reflection points, drawn from the G-buffer or forward") {
        SceneFixture scene;
        writeMetal(scene, "chrome", 0.0f);
        scene.writeMaterial("glass chrome",
                            R"("baseColor": [1, 1, 1, 1], "metallic": 1, "roughness": 0, "alphaMode": "blended")");
        scene.writeMatteMaterial("matte");
        scene.writeHdrImage("patches", makeEnvironment(64, patchOf));
        scene.open();
        // A ball of 1.6 metres seen from +Z: 25.6 pixels from its middle to its rim.
        const Entity ball = scene.add(BuiltinMesh::Sphere, scene.material("chrome"), at(Vec3(0.0f), Vec3(1.6f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        scene.setEnvironment(scene.asset("patches.hdr"));

        const auto checkReflections = [](const Image& image) {
            // In the middle the view is thrown straight back: +Z. Where the ball's surface
            // is tilted by 45 degrees - 18 pixels from the middle - it is thrown out
            // sideways: to the right, to the left, up and down.
            checkPixel(image, 32, 32, kPlusZ);
            checkPixel(image, 50, 32, kPlusX);
            checkPixel(image, 13, 32, kMinusX);
            checkPixel(image, 32, 13, kPlusY);
            checkPixel(image, 32, 50, kMinusY);
            // Beside the ball: the sky ahead.
            checkPixel(image, 3, 3, kMinusZ);
        };
        checkReflections(scene.render(view));
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 0);

        // The same surface drawn forward, blended over the sky at full opacity.
        scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material("glass chrome")};
        checkReflections(scene.render(view));
        CHECK(scene.renderer->statistics().basePass.drawCalls == 0);

        // The view of the reflections makes a mirror of whatever is there.
        scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material("matte")};
        checkReflections(scene.render(view, reflectionSettings()));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the sky shows every direction of the environment where it belongs") {
        SceneFixture scene;
        scene.writeHdrImage("directions", makeEnvironment(256, colorOfDirection));
        scene.open();
        scene.setEnvironment(scene.asset("directions.hdr"));

        // Wide views all around, one of them straight up and one tilted.
        const Vec3 eye(2.0f, 1.0f, -3.0f);
        for (const Vec3& towards : {Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, 0.0f),
                                    Vec3(-1.0f, 0.2f, 0.3f), Vec3(0.3f, 1.0f, -0.2f), Vec3(0.4f, -1.0f, 0.5f)}) {
            CAPTURE(towards);
            const CameraView view = scene.perspectiveView(eye, eye + towards, 110.0f);
            const Image image = scene.render(view);
            for (uint32_t y = 2; y < 64; y += 6) {
                for (uint32_t x = 2; x < 64; x += 6) {
                    CAPTURE(x);
                    CAPTURE(y);
                    const Ray ray =
                        screenRay(view, Vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), Vec2(64.0f));
                    CHECK_NEAR(Vec3(image.pixel(x, y)), displayed(colorOfDirection(ray.direction)), 0.02f);
                }
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a ball that mirrors, sharply or roughly, shows every direction of the environment where it belongs") {
        SceneFixture scene;
        writeMetal(scene, "metal0", 0.0f);
        writeMetal(scene, "metal4", 0.4f);
        writeMetal(scene, "metal8", 0.8f);
        scene.writeHdrImage("directions", makeEnvironment(256, colorOfDirection));
        scene.open();
        // A ball of 1.6 metres: 25.6 pixels from its middle to its rim.
        const float radius = 0.8f;
        const Entity ball =
            scene.add(BuiltinMesh::Sphere, scene.material("metal0"), at(Vec3(0.0f), Vec3(2.0f * radius)));
        scene.setEnvironment(scene.asset("directions.hdr"));

        // Seen from six sides: the ball throws back what is on the viewer's side of it,
        // so between them every face of the cube map is seen, all over.
        struct Side {
            Vec3 towardsEye;
            Vec3 up;
        };

        for (const Side& side : {Side{Vec3(0.0f, 0.0f, 1.0f), kWorldUp}, Side{Vec3(0.0f, 0.0f, -1.0f), kWorldUp},
                                 Side{Vec3(1.0f, 0.0f, 0.0f), kWorldUp}, Side{Vec3(-1.0f, 0.0f, 0.0f), kWorldUp},
                                 Side{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f)},
                                 Side{Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)}}) {
            CAPTURE(side.towardsEye);
            const CameraView view = scene.orthographicView(side.towardsEye * 5.0f, Vec3(0.0f), 2.0f, side.up);
            const Vec3 right = glm::cross(-side.towardsEye, side.up);
            for (const int tenths : {0, 4, 8}) {
                CAPTURE(tenths);
                const float roughness = static_cast<float>(tenths) / 10.0f;
                scene.world.tryGet<MeshRendererComponent>(ball)->materials = {
                    scene.material(fmt::format("metal{}", tenths))};
                const Image image = scene.render(view);
                uint32_t checked = 0;
                for (uint32_t y = 2; y < 64; y += 5) {
                    for (uint32_t x = 2; x < 64; x += 5) {
                        // Where the pixel is on the ball, in metres from its middle. Near
                        // the rim a pixel covers too many directions to say which.
                        const float across = (static_cast<float>(x) + 0.5f - 32.0f) / 32.0f;
                        const float upwards = (32.0f - static_cast<float>(y) - 0.5f) / 32.0f;
                        if (across * across + upwards * upwards > 0.6f * 0.6f * radius * radius) {
                            continue;
                        }
                        CAPTURE(x);
                        CAPTURE(y);
                        const float outwards = std::sqrt(radius * radius - across * across - upwards * upwards);
                        const Vec3 normal = (right * across + side.up * upwards + side.towardsEye * outwards) / radius;
                        const Vec3 mirrored = glm::reflect(-side.towardsEye, normal);
                        // The direction most of the light of a rough surface comes from
                        // (reflectionDirection in Environment.slang).
                        const float alpha = roughness * roughness;
                        const Vec3 towards = glm::normalize(
                            glm::mix(normal, mirrored, (1.0f - alpha) * (std::sqrt(1.0f - alpha) + alpha)));
                        // What the metal shows, without what its roughness loses.
                        const Vec3 shown = srgbToLinear(Vec3(image.pixel(x, y))) / (1.0f - 0.55f * roughness);
                        if (tenths == 0) {
                            CHECK_NEAR(shown, colorOfDirection(towards), 0.02f);
                        } else {
                            // A blur of the colors around that direction: nearer to grey,
                            // and away from grey the way the direction points.
                            const Vec3 fromGrey = shown - colorOfDirection(Vec3(0.0f));
                            const float strength = glm::length(fromGrey) / 0.375f;
                            CHECK(strength < 1.0f);
                            CHECK(strength > 0.35f);
                            CHECK(glm::dot(glm::normalize(fromGrey), towards) > 0.985f);
                        }
                        ++checked;
                    }
                }
                CHECK(checked > 25);
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("turning the environment turns its sky, its reflections and its light together") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        const Image patches = makeEnvironment(64, patchOf);
        scene.writeHdrImage("patches", patches);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CubeViews views(scene);
        const SphericalHarmonics9 light = projectEnvironment(patches);

        // 90 degrees counter-clockwise seen from above: what was ahead (-Z) is on the
        // left (-X), what was on the right (+X) is ahead.
        for (const float degrees : {0.0f, 90.0f, -90.0f, 180.0f, 450.0f}) {
            CAPTURE(degrees);
            scene.setEnvironment(scene.asset("patches.hdr")).rotationDegrees = degrees;
            // A direction of the world is, in the image, the direction turned back.
            const auto inImage = [degrees](const Vec3& direction) {
                return turnedAboutVertical(direction, -toRadians(degrees));
            };

            Image image = scene.render(views.front);
            // The sky beside the cube, ...
            checkPixel(image, 4, 4, patchOf(inImage(Vec3(0.0f, 0.0f, -1.0f))));
            // ... and the light on the face of the cube that looks along +Z.
            checkPixel(image, 32, 32, light.irradiance(inImage(Vec3(0.0f, 0.0f, 1.0f))) / kPi);
            // The top is lit from straight up whichever way the environment is turned.
            checkPixel(scene.render(views.above), 32, 32, light.irradiance(kWorldUp) / kPi);
            // The front of the cube as a mirror throws the view straight back.
            image = scene.render(views.front, reflectionSettings());
            checkPixel(image, 32, 32, patchOf(inImage(Vec3(0.0f, 0.0f, 1.0f))));
        }

        // Said plainly, for a quarter turn.
        scene.setEnvironment(scene.asset("patches.hdr")).rotationDegrees = 90.0f;
        Image image = scene.render(scene.perspectiveView(Vec3(0.0f, 3.0f, 0.0f), Vec3(-1.0f, 3.0f, 0.0f), 120.0f));
        checkPixel(image, 32, 32, kMinusZ);
        image = scene.render(scene.perspectiveView(Vec3(0.0f, 3.0f, 0.0f), Vec3(0.0f, 3.0f, -1.0f), 120.0f));
        checkPixel(image, 32, 32, kPlusX);
        // The six colors light the six sides of a cube differently: there is something
        // that turns.
        CHECK(glm::distance(light.irradiance(Vec3(1.0f, 0.0f, 0.0f)), light.irradiance(Vec3(0.0f, 0.0f, 1.0f))) > 0.5f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the rougher a metal, the less of an even environment it throws back, and never a different color") {
        const Vec3 light(0.9f, 0.6f, 0.3f);
        SceneFixture scene;
        for (const int tenths : {0, 2, 4, 6, 8, 10}) {
            writeMetal(scene, fmt::format("metal{}", tenths), static_cast<float>(tenths) / 10.0f);
        }
        scene.writeHdrImage("even", makeEnvironment(64, allAround(light)));
        scene.open();
        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("metal0"), at(Vec3(0.0f), Vec3(2.0f)));
        const CubeViews views(scene);
        scene.setEnvironment(scene.asset("even.hdr"));

        // Each of these roughnesses reads one level of the cube map. A level that lost
        // or made light when it was blurred would show here. What a white metal throws
        // back of light from all around is one less 0.55 times its roughness
        // (environmentReflectance in Brdf.slang).
        for (const int tenths : {0, 2, 4, 6, 8, 10}) {
            CAPTURE(tenths);
            const float roughness = static_cast<float>(tenths) / 10.0f;
            scene.world.tryGet<MeshRendererComponent>(cube)->materials = {
                scene.material(fmt::format("metal{}", tenths))};
            const Vec3 expected = light * (1.0f - 0.55f * roughness);
            checkPixel(scene.render(views.front), 32, 32, expected);
            checkPixel(scene.render(views.above), 32, 32, expected);
            checkPixel(scene.render(views.below), 20, 44, expected);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a rough metal blurs what it reflects: at the horizon of a sky over nothing, half of the sky") {
        const Vec3 sky(0.8f);
        SceneFixture scene;
        writeMetal(scene, "mirror", 0.0f);
        writeMetal(scene, "satin", 0.4f);
        writeMetal(scene, "rough", 1.0f);
        scene.writeHdrImage("sky", makeEnvironment(256, skyAbove(sky)));
        scene.open();
        const Entity ball = scene.add(BuiltinMesh::Sphere, scene.material("mirror"), at(Vec3(0.0f), Vec3(1.6f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        scene.setEnvironment(scene.asset("sky.hdr"));
        const auto use = [&](std::string_view material) {
            scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material(material)};
        };

        // A mirror: the upper half of the ball reflects the sky, the lower half nothing,
        // with an edge between them.
        Image image = scene.render(view);
        checkPixel(image, 32, 26, sky);
        checkPixel(image, 32, 38, Vec3(0.0f));
        const float mirrorAbove = image.pixel(32, 29).r;
        const float mirrorBelow = image.pixel(32, 35).r;
        CHECK(mirrorAbove - mirrorBelow > 0.85f);

        // Satin: the edge has become a slope.
        use("satin");
        image = scene.render(view);
        const float satinAbove = image.pixel(32, 29).r;
        const float satinBelow = image.pixel(32, 35).r;
        CHECK(satinAbove > satinBelow + 0.05f);
        CHECK(satinAbove - satinBelow < mirrorAbove - mirrorBelow - 0.2f);
        // Further up the ball it is the sky still, less what a metal this rough loses.
        checkPixel(image, 32, 14, sky * (1.0f - 0.55f * 0.4f));

        // As rough as can be: the middle of the ball, which faces the horizon, reflects
        // a blur of everything around the horizon - half of it sky. And what faces up
        // on such a surface reflects what is above it, wherever it is seen from.
        use("rough");
        image = scene.render(view);
        const float lossless = 1.0f - 0.55f;
        // Just above and just below the middle row, which the horizon runs through.
        const Vec3 above = Vec3(image.pixel(32, 31));
        const Vec3 below = Vec3(image.pixel(32, 32));
        CHECK_NEAR((above + below) * 0.5f, displayed(sky * (0.5f * lossless)), 0.03f);
        const Image fromAbove =
            scene.render(scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f)));
        checkPixel(fromAbove, 32, 32, sky * lossless);
        const Image fromBelow =
            scene.render(scene.orthographicView(Vec3(0.0f, -5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, 1.0f)));
        // What faces down reflects what is below: nothing, but for the hundredth of the
        // sky's light that the blurring lets through from the other side of the horizon.
        CHECK(fromBelow.pixel(32, 32).r < displayed(sky * lossless * 0.02f).r);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an environment brighter than white keeps its light, and a sun in it lights like a sun") {
        // A dim sky with a small sun a hundred times brighter than white, high in the
        // south-east. Without the sun the sky lights a floor to 0.05.
        const Vec3 towardsSun = glm::normalize(Vec3(0.5f, 0.7f, 0.5f));
        const auto light = [&](const Vec3& direction) {
            return glm::dot(direction, towardsSun) > std::cos(toRadians(4.0f)) ? Vec3(100.0f) : Vec3(0.05f);
        };
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        const Image sky = makeEnvironment(512, light);
        scene.writeHdrImage("sunny", sky);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(2.0f)));
        const CubeViews views(scene);
        scene.setEnvironment(scene.asset("sunny.hdr"));

        // The sun covers about 0.0153 steradians (a cap of four degrees), so facing it a
        // surface would get 1.53 from it; the floor faces it at an angle whose cosine is
        // towardsSun.y. Nine numbers say so to within a few percent.
        const float cap = kTwoPi * (1.0f - std::cos(toRadians(4.0f)));
        const float expected = 0.05f + 100.0f * cap * towardsSun.y / kPi;
        const SphericalHarmonics9 projected = projectEnvironment(sky);
        CHECK(projected.irradiance(kWorldUp).r / kPi == doctest::Approx(expected).epsilon(0.08));
        // That is what the renderer lights the floor with, ...
        const Image above = scene.render(views.above);
        checkPixel(above, 32, 32, projected.irradiance(kWorldUp) / kPi);
        CHECK(above.pixel(32, 32).r > displayed(Vec3(0.25f)).r);
        // ... and what faces away from the sun gets little more than the sky gives.
        const Image below = scene.render(views.below);
        CHECK(below.pixel(32, 32).r < displayed(Vec3(0.12f)).r);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a sun brighter than the scene color can hold stays a number, in the sky and in every mirror") {
        // The sun as bright as a file of light can say, in a black sky - and the
        // environment made eight times as bright on top.
        const Vec3 towardsSun = glm::normalize(Vec3(0.0f, 0.35f, -1.0f));
        const auto light = [&](const Vec3& direction) {
            return glm::dot(direction, towardsSun) > std::cos(toRadians(6.0f)) ? Vec3(1.0e6f) : Vec3(0.0f);
        };
        SceneFixture scene;
        writeMetal(scene, "chrome", 0.0f);
        scene.writeHdrImage("glare", makeEnvironment(256, light));
        scene.open();
        // A mirror on the floor, tilted up at the back, in which the sun is seen a second time.
        scene.add(BuiltinMesh::Plane, scene.material("chrome"),
                  at(Vec3(0.0f, -1.0f, -3.0f), Vec3(6.0f), glm::angleAxis(toRadians(10.0f), Vec3(1.0f, 0.0f, 0.0f))));
        scene.setEnvironment(scene.asset("glare.hdr")).intensity = 8.0f;
        const CameraView view = scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.1f, -1.0f), 70.0f);

        // The picture, finished: with bloom, which blurs whatever the scene color holds.
        SceneRenderSettings finished = SceneFixture::flatSettings();
        finished.postProcessQuality = PostProcessQuality::High;
        finished.postProcess = PostProcessSettings{.toneMapper = ToneMapper::Linear, .bloomIntensity = 0.5f};
        const Image image = scene.render(view, finished);

        // The scene color holds numbers, none of them more than a half float can. (Some
        // GPUs see to that themselves, and would pass this without the shaders' help;
        // others keep what is too much as infinity.)
        const TexturePixels sceneColor = unwrap(readTexture(*scene.context.graphics, scene.targets.sceneColor()));
        REQUIRE(sceneColor.format == nvrhi::Format::RGBA16_FLOAT);
        float most = 0.0f;
        uint32_t glaring = 0;
        for (size_t offset = 0; offset + 8 <= sceneColor.data.size(); offset += 8) {
            std::array<uint16_t, 3> halves{};
            std::memcpy(halves.data(), sceneColor.data.data() + offset, sizeof(halves));
            for (const uint16_t half : halves) {
                const float value = glm::unpackHalf1x16(half);
                REQUIRE(std::isfinite(value));
                most = std::max(most, value);
            }
            glaring += glm::unpackHalf1x16(halves[0]) > 60000.0f ? 1 : 0;
        }
        CHECK(most == 65504.0f);
        // The sun, and the sun in the mirror.
        CHECK(glaring > 20);

        // And the picture shows a sun with a glow around it, not a hole: white where
        // the sun is, and bright, not black, beside it.
        const std::optional<Vec2> sun = worldToScreen(view, towardsSun * 100.0f, Vec2(64.0f));
        REQUIRE(sun.has_value());
        const auto sunX = static_cast<uint32_t>(sun->x);
        const auto sunY = static_cast<uint32_t>(sun->y);
        CHECK(image.pixel(sunX, sunY).r > 0.99f);
        for (const int offset : {-12, 12}) {
            const Vec4 beside = image.pixel(static_cast<uint32_t>(static_cast<int>(sunX) + offset), sunY);
            CHECK(beside.r > 0.2f);
            CHECK(beside.g > 0.2f);
            CHECK(beside.b > 0.2f);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a sky seen small is averaged, not picked from") {
        // Columns of white and black, one pixel wide each, all around.
        SceneFixture scene;
        Image stripes = unwrap(Image::create(1024, 512, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < stripes.height(); ++y) {
            for (uint32_t x = 0; x < stripes.width(); ++x) {
                stripes.setPixel(x, y, Vec4(Vec3(x % 2 == 0 ? 1.0f : 0.0f), 1.0f));
            }
        }
        scene.writeHdrImage("stripes", stripes);
        scene.open();
        scene.setEnvironment(scene.asset("stripes.hdr"));

        // A pixel of this view covers some five columns: it shows their average, grey,
        // everywhere - not white here and black there, flickering as the camera turns.
        const Image image = scene.render(scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 90.0f));
        for (const uint32_t x : {5u, 20u, 31u, 32u, 33u, 47u, 60u}) {
            for (const uint32_t y : {8u, 32u, 55u}) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK_NEAR(Vec3(image.pixel(x, y)), displayed(Vec3(0.5f)), 0.04f);
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an image of 8-bit colors is an environment too, taken as sRGB") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        // Stored as numbers: half-way to white is a fifth of white's light.
        Image image = unwrap(Image::create(32, 16, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < image.height(); ++y) {
            for (uint32_t x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, Vec4(128.0f / 255.0f, 64.0f / 255.0f, 1.0f, 1.0f));
            }
        }
        scene.writeImage("painted", image);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        scene.setEnvironment(scene.asset("painted.png"));

        const Vec3 light = srgbToLinear(Vec3(128.0f / 255.0f, 64.0f / 255.0f, 1.0f));
        const Image rendered = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));
        checkPixel(rendered, 32, 32, light);
        checkPixel(rendered, 4, 4, light);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("of several environments the one whose entity has the lowest id lights the world") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeHdrImage("first", makeEnvironment(32, allAround(Vec3(0.2f, 0.4f, 0.6f))));
        scene.writeHdrImage("second", makeEnvironment(32, allAround(Vec3(0.7f, 0.5f, 0.3f))));
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        // Made in the other order, to show that it is not the order of making.
        const Entity later =
            unwrap(scene.world.createEntityWithId(*Uuid::parse("00000000-0000-4000-8000-000000000002"), "Later"));
        scene.world.add<EnvironmentComponent>(later) = {.texture = scene.asset("second.hdr")};
        const Entity earlier =
            unwrap(scene.world.createEntityWithId(*Uuid::parse("00000000-0000-4000-8000-000000000001"), "Earlier"));
        scene.world.add<EnvironmentComponent>(earlier) = {.texture = scene.asset("first.hdr")};

        Image image = scene.render(view);
        checkPixel(image, 32, 32, Vec3(0.2f, 0.4f, 0.6f));
        checkPixel(image, 4, 4, Vec3(0.2f, 0.4f, 0.6f));
        // They do not add up. With the first switched off, the second takes over.
        scene.world.setEnabled(earlier, false);
        image = scene.render(view);
        checkPixel(image, 32, 32, Vec3(0.7f, 0.5f, 0.3f));
        checkPixel(image, 4, 4, Vec3(0.7f, 0.5f, 0.3f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the cube map of an environment is made once, and again when its image changes") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeHdrImage("sky", makeEnvironment(32, allAround(Vec3(0.2f))));
        scene.writeHdrImage("other", makeEnvironment(32, allAround(Vec3(0.6f))));
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const SceneRenderer& renderer = *scene.renderer;

        // Without an environment nothing is made or held.
        scene.render(view);
        CHECK(renderer.statistics().environmentsMade == 0);
        CHECK(renderer.statistics().environmentBytes == 0);

        scene.setEnvironment(scene.asset("sky.hdr"));
        checkPixel(scene.render(view), 32, 32, Vec3(0.2f));
        CHECK(renderer.statistics().environmentsMade == 1);
        CHECK(renderer.statistics().environmentBytes == kEnvironmentBytes);
        // Six faces of 256 texels a side, and five ever smaller levels of them.
        CHECK(kEnvironmentBytes == uint64_t{6} * (65536 + 16384 + 4096 + 1024 + 256 + 64) * 8);
        for (int frame = 0; frame < 3; ++frame) {
            checkPixel(scene.render(view), 32, 32, Vec3(0.2f));
            CHECK(renderer.statistics().environmentsMade == 0);
        }
        // How it is turned and how bright it is are not part of the cube map.
        scene.setEnvironment(scene.asset("sky.hdr")).rotationDegrees = 40.0f;
        scene.world.tryGet<EnvironmentComponent>(scene.environmentEntity())->intensity = 2.0f;
        checkPixel(scene.render(view), 32, 32, Vec3(0.4f));
        CHECK(renderer.statistics().environmentsMade == 0);

        // Another image: another cube map, next to the first for a while.
        scene.setEnvironment(scene.asset("other.hdr"));
        checkPixel(scene.render(view), 32, 32, Vec3(0.6f));
        CHECK(renderer.statistics().environmentsMade == 1);
        CHECK(renderer.statistics().environmentBytes == 2 * kEnvironmentBytes);
        // Over the views of a frame the cube maps made add up; their memory does not.
        scene.render(view);
        const SceneRenderStatistics frame = scene.renderer->takeFrameStatistics();
        CHECK(frame.environmentsMade == 2);
        CHECK(frame.environmentBytes == 2 * kEnvironmentBytes);

        // The file changes under the same id: once the assets have been looked at again,
        // the environment is made of the new image.
        scene.writeHdrImage("other", makeEnvironment(32, allAround(Vec3(0.9f, 0.1f, 0.1f))));
        REQUIRE(scene.assets->refresh().has_value());
        Image image = scene.render(view);
        checkPixel(image, 32, 32, Vec3(0.9f, 0.1f, 0.1f));
        checkPixel(image, 4, 4, Vec3(0.9f, 0.1f, 0.1f));
        CHECK(renderer.statistics().environmentsMade == 1);
        scene.render(view);
        CHECK(renderer.statistics().environmentsMade == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an environment nobody asks for any more gives its memory back") {
        SceneFixture scene;
        scene.writeHdrImage("sky", makeEnvironment(32, allAround(Vec3(0.2f))));
        scene.writeHdrImage("other", makeEnvironment(32, allAround(Vec3(0.6f))));
        scene.open();
        const std::unique_ptr<RenderResources> owned =
            unwrap(RenderResources::create(scene.context.device(), *scene.assets));
        RenderResources& resources = *owned;
        {
            EnvironmentMaps environments =
                unwrap(EnvironmentMaps::create(scene.context.device(), *scene.context.shaders, *scene.assets));
            CHECK(environments.count() == 0);
            CHECK(environments.byteSize() == 0);
            CHECK(environments.none().radiance != nullptr);
            CHECK(environments.none().sky != nullptr);
            CHECK(environments.none().levelCount == 1);

            const Uuid sky = scene.asset("sky.hdr");
            const Uuid other = scene.asset("other.hdr");
            recordAndExecute(scene.context, [&](nvrhi::ICommandList* commandList) {
                resources.beginFrame();
                // Nothing to make for no image.
                CHECK(environments.environment(commandList, Uuid(), resources) == nullptr);
                CHECK(environments.count() == 0);

                const GpuEnvironment* made = environments.environment(commandList, sky, resources);
                REQUIRE(made != nullptr);
                CHECK(made->radiance != nullptr);
                CHECK(made->sky != nullptr);
                CHECK(made->levelCount == kEnvironmentLevels);
                // An even grey: all of its light is in the first of the nine numbers,
                // which times the basis there (0.282095) is the grey.
                CHECK(made->diffuse[0].r * 0.282095f == doctest::Approx(0.2f).epsilon(0.01));
                CHECK(std::abs(made->diffuse[3].r) < 1.0e-3f);
                CHECK(environments.bakeCount() == 1);

                // Two take turns: both are kept, however long that goes on.
                for (int turn = 0; turn < 200; ++turn) {
                    REQUIRE(environments.environment(commandList, turn % 2 == 0 ? sky : other, resources) != nullptr);
                }
                CHECK(environments.count() == 2);
                CHECK(environments.bakeCount() == 2);
                CHECK(environments.byteSize() == 2 * kEnvironmentBytes);

                // The other one, asked for last just now, is not asked for any more: it is
                // kept for as many calls as it says, and let go by the one after.
                for (uint64_t call = 0; call < EnvironmentMaps::kKeepUnusedCalls; ++call) {
                    REQUIRE(environments.environment(commandList, sky, resources) != nullptr);
                }
                CHECK(environments.count() == 2);
                REQUIRE(environments.environment(commandList, sky, resources) != nullptr);
                CHECK(environments.count() == 1);
                CHECK(environments.byteSize() == kEnvironmentBytes);
                CHECK(environments.bakeCount() == 2);

                // Views without an environment count as well: in the end nothing is held.
                for (uint64_t call = 0; call <= EnvironmentMaps::kKeepUnusedCalls; ++call) {
                    CHECK(environments.environment(commandList, Uuid(), resources) == nullptr);
                }
                CHECK(environments.count() == 0);
                CHECK(environments.byteSize() == 0);

                // Asked for again, it is made again.
                REQUIRE(environments.environment(commandList, sky, resources) != nullptr);
                CHECK(environments.bakeCount() == 3);
                resources.flushUploads();
            });
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an environment whose image is missing or broken lights nothing, and says so once") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        REQUIRE(writeTextFile(scene.file("broken.hdr"), "#?RADIANCE\nthis is not an image").has_value());
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        scene.setAmbient(Vec3(0.3f), Vec3(0.3f));
        const LogCapture log;

        // An id no asset has.
        scene.setEnvironment(*Uuid::parse("7f1c2b9e-3a4d-4c5e-8f60-112233445566"));
        for (int frame = 0; frame < 3; ++frame) {
            const Image image = scene.render(view);
            // The rest of the scene is drawn as it is.
            checkPixel(image, 32, 32, Vec3(0.3f));
            checkPixel(image, 4, 4, kBackground);
        }
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("7f1c2b9e-3a4d-4c5e-8f60-112233445566"));
        CHECK(scene.renderer->statistics().environmentsMade == 0);
        CHECK(scene.renderer->statistics().environmentBytes == 0);

        // A file that is no image.
        scene.setEnvironment(scene.asset("broken.hdr"));
        for (int frame = 0; frame < 3; ++frame) {
            const Image image = scene.render(view);
            checkPixel(image, 32, 32, Vec3(0.3f));
            checkPixel(image, 4, 4, kBackground);
        }
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("broken.hdr"));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the debug views of the G-buffer show no sky, and the views of the light do") {
        SceneFixture scene;
        scene.writeMatteMaterial("matte");
        scene.writeHdrImage("even", makeEnvironment(32, allAround(Vec3(0.5f, 0.25f, 0.75f))));
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f), Vec3(1.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        scene.setEnvironment(scene.asset("even.hdr"));

        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::BaseColor;
        checkPixel(scene.render(view, settings), 4, 4, kBackground);
        settings.debugView = RenderDebugView::Normals;
        checkPixel(scene.render(view, settings), 4, 4, kBackground);

        // The light alone: a matte grey cube under the sky.
        settings.debugView = RenderDebugView::LightingOnly;
        Image image = scene.render(view, settings);
        checkPixel(image, 4, 4, Vec3(0.5f, 0.25f, 0.75f));
        checkPixel(image, 32, 32, Vec3(0.5f, 0.25f, 0.75f) * 0.5f);
        // The reflections alone: a mirror in an even environment is that environment.
        settings.debugView = RenderDebugView::Reflections;
        image = scene.render(view, settings);
        checkPixel(image, 4, 4, Vec3(0.5f, 0.25f, 0.75f));
        checkPixel(image, 32, 32, Vec3(0.5f, 0.25f, 0.75f));
        // Mirrors show the ambient light as well, and no light of the lights.
        scene.setEnvironment(Uuid());
        scene.setAmbient(Vec3(0.6f, 0.4f, 0.2f), Vec3(0.2f, 0.4f, 0.6f));
        scene.setSun(Vec3(0.0f, 0.0f, 1.0f), Vec3(3.0f));
        image = scene.render(view, settings);
        // The front of the cube throws the view back along the horizon: half sky, half ground.
        checkPixel(image, 32, 32, Vec3(0.4f));
        checkPixel(image, 4, 4, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
