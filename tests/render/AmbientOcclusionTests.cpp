#include "cordite/renderer/AmbientOcclusion.h"

#include "cordite/assets/Image.h"
#include "cordite/math/Color.h"
#include "cordite/math/Projection.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::displayed;
using cordite::test::recordAndExecute;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

constexpr uint32_t kSize = 128;

/// Settings that show the ambient occlusion itself, as numbers: 1 is all the light.
SceneRenderSettings occlusionSettings(ScreenEffectsQuality quality = ScreenEffectsQuality::High,
                                      const PostProcessSettings& look = {}) {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.screenEffectsQuality = quality;
    settings.debugView = RenderDebugView::AmbientOcclusion;
    settings.postProcess = look;
    return settings;
}

/// Settings that show the light as it was worked out, with ambient occlusion in it.
SceneRenderSettings litSettings(ScreenEffectsQuality quality = ScreenEffectsQuality::High,
                                const PostProcessSettings& look = {}) {
    SceneRenderSettings settings = SceneFixture::flatSettings();
    settings.screenEffectsQuality = quality;
    settings.postProcess = look;
    return settings;
}

/// A look with the given numbers for the ambient occlusion, in metres and degrees.
PostProcessSettings lookWith(float radius, float bias = 0.0f, float intensity = 1.0f) {
    PostProcessSettings look;
    look.ambientOcclusionRadius = radius;
    look.ambientOcclusionBias = bias;
    look.ambientOcclusionIntensity = intensity;
    return look;
}

/// How far up the wall of a Corner reaches from its foot, and how far along it either
/// way, in metres.
constexpr float kWallHeight = 6.0f;
constexpr float kWallHalfLength = 6.0f;

/// A floor, and a wall standing on it along the line x = 0: the floor is the plane
/// y = 0 where x is more than nothing.
struct Corner {
    /// `lean` is how far the wall leans back from upright, in degrees: 0 is a wall, 45 a
    /// ramp, and less than nothing a wall that hangs over the floor.
    explicit Corner(float lean = 0.0f) : scene(UVec2(kSize, kSize)) {
        scene.writeMatteMaterial("matte");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(2.0f, -0.5f, 0.0f), Vec3(12.0f, 1.0f, 12.0f)));
        // A slab whose face towards +X goes through the line x = 0, y = 0, turned about
        // that line.
        const Quat turn = glm::angleAxis(toRadians(lean), Vec3(0.0f, 0.0f, 1.0f));
        scene.add(
            BuiltinMesh::Cube, scene.material("matte"),
            at(turn * Vec3(-0.5f, 0.5f * kWallHeight, 0.0f), Vec3(1.0f, kWallHeight, 2.0f * kWallHalfLength), turn));
    }

    /// A camera without perspective that looks at the foot of the wall from `elevation`
    /// degrees above the floor, `around` degrees round from straight in front of it.
    [[nodiscard]] CameraView view(float elevation = 45.0f, float around = 0.0f, float height = 2.0f) const {
        const float up = toRadians(elevation);
        const float round = toRadians(around);
        const Vec3 towardsEye(std::cos(up) * std::cos(round), std::sin(up), std::cos(up) * std::sin(round));
        return scene.orthographicView(towardsEye * 6.0f, Vec3(0.0f), height);
    }

    SceneFixture scene;
};

/// Where a point of the scene is in a picture taken through `view`.
Vec2 pixelOf(const CameraView& view, const Vec3& point) {
    const std::optional<Vec2> pixel = worldToScreen(view, point, Vec2(static_cast<float>(kSize)));
    REQUIRE(pixel.has_value());
    return *pixel;
}

/// What `image`, a picture taken through `view`, shows at a point of the scene: for a
/// picture of the ambient occlusion, the share of the light from all around there.
float valueAt(const Image& image, const CameraView& view, const Vec3& point) {
    const Vec2 pixel = pixelOf(view, point);
    REQUIRE(pixel.x >= 0.0f);
    REQUIRE(pixel.y >= 0.0f);
    REQUIRE(pixel.x < static_cast<float>(kSize));
    REQUIRE(pixel.y < static_cast<float>(kSize));
    return image.pixel(static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y)).r;
}

/// A point on the floor of a Corner, `distance` metres from the foot of the wall.
Vec3 onFloor(float distance, float along = 0.0f) {
    return {distance, 0.0f, along};
}

/// What the ambient occlusion on the floor of a Corner comes to when the wall is looked
/// for in every direction and at every distance: what the pass draws with a few looks a
/// pixel, without their noise. The arithmetic is the engine's own, on the CPU
/// (AmbientOcclusion.h); the wall is a formula.
struct Reference {
    /// As Corner: how far the wall leans back, in degrees.
    float lean = 0.0f;
    /// As PostProcessSettings: metres, degrees, a power.
    float radius = 0.5f;
    float bias = 0.0f;
    float intensity = 1.0f;
    /// The pass looks no further across the screen than a quarter of the picture's
    /// height. Without this, the reference looks as far as the radius, whatever the
    /// picture.
    bool limitedByThePicture = true;

    [[nodiscard]] float at(const Vec3& point, const CameraView& view) const {
        const Vec3 normal = kWorldUp;
        const Vec3 toViewer = view.orthographic ? -view.forward : glm::normalize(view.position - point);
        // The directions of the screen, in the world.
        const Vec3 right(view.view[0][0], view.view[1][0], view.view[2][0]);
        const Vec3 up(view.view[0][1], view.view[1][1], view.view[2][1]);
        // The wall's face: a plane through the line x = 0, y = 0 that leaves the side
        // `open` free and runs `upTheWall` from that line.
        const float tilt = toRadians(lean);
        const Vec3 open(std::cos(tilt), std::sin(tilt), 0.0f);
        const Vec3 upTheWall(-std::sin(tilt), std::cos(tilt), 0.0f);

        // No further than the scene is large.
        float reach = std::min(radius, 20.0f);
        if (limitedByThePicture) {
            const float pixelsPerMetre =
                glm::distance(pixelOf(view, point), pixelOf(view, point + right * 0.01f)) / 0.01f;
            reach = std::min(radius, 0.25f * static_cast<float>(kSize) / pixelsPerMetre);
        }

        constexpr int kSlices = 180;
        constexpr int kLooks = 500;
        double visible = 0.0;
        double all = 0.0;
        for (int slice = 0; slice < kSlices; ++slice) {
            const float turn = (static_cast<float>(slice) + 0.5f) / static_cast<float>(kSlices) * kPi;
            // Pixels run down the screen.
            const Vec3 direction = right * std::cos(turn) - up * std::sin(turn);
            const SliceNormal seen = projectIntoSlice(normal, toViewer, direction);
            if (seen.length < 1.0e-4f) {
                continue;
            }
            std::array<float, 2> rise = {0.0f, 0.0f};
            for (size_t side = 0; side < 2; ++side) {
                for (int look = 1; look <= kLooks; ++look) {
                    // More looks near the point than far from it: a wall that hangs
                    // over the point is all within a little of it on the screen.
                    const float part = static_cast<float>(look) / static_cast<float>(kLooks);
                    const float across = (side == 0 ? -1.0f : 1.0f) * reach * part * part;
                    // What is seen that far across the screen from the point: where the
                    // line of sight through there meets the wall's plane, if that is on
                    // the wall. Anything else there is floor, which rises by nothing.
                    const Vec3 onScreen = point + direction * across;
                    const Vec3 origin = view.orthographic ? onScreen : view.position;
                    const Vec3 sight = view.orthographic ? -toViewer : glm::normalize(onScreen - view.position);
                    const float towardsWall = glm::dot(sight, open);
                    if (towardsWall > -1.0e-6f) {
                        continue;
                    }
                    const Vec3 met = origin + sight * (-glm::dot(origin, open) / towardsWall);
                    const float height = glm::dot(met, upTheWall);
                    if (height < 0.0f || height > kWallHeight || std::abs(met.z) > kWallHalfLength) {
                        continue;
                    }
                    const Vec3 offset = met - point;
                    const float distance = glm::length(offset);
                    if (distance < 1.0e-5f) {
                        continue;
                    }
                    // As the pass weighs it: all of it up to four tenths of the radius,
                    // nothing of it at the radius.
                    const float weight = std::clamp((radius - distance) / (radius * 0.6f), 0.0f, 1.0f);
                    rise[side] = std::max(rise[side], riseAboveSurface(offset / distance, normal, toViewer) * weight);
                }
            }
            const float before = std::min(seen.angle - kHalfPi + std::max(rise[0] - toRadians(bias), 0.0f), 0.0f);
            const float after = std::max(seen.angle + kHalfPi - std::max(rise[1] - toRadians(bias), 0.0f), 0.0f);
            visible += static_cast<double>(seen.length) * sliceVisibility(seen.angle, before, after);
            all += static_cast<double>(seen.length) * openSliceVisibility(seen.angle);
        }
        return std::pow(std::clamp(static_cast<float>(visible / all), 0.0f, 1.0f), intensity);
    }
};

/// How far what the pass draws at its best may be from the reference: the pass looks a
/// few times where the reference looks hundreds of times, and misses a little of what
/// rises highest.
constexpr float kTolerance = 0.07f;

} // namespace

TEST_SUITE("AmbientOcclusion") {
    TEST_CASE("an open floor gets all the light from around it, from wherever it is seen") {
        SceneFixture scene(UVec2(kSize, kSize));
        scene.writeMatteMaterial("matte");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f, -0.5f, 0.0f), Vec3(40.0f, 1.0f, 40.0f)));

        for (const ScreenEffectsQuality quality :
             {ScreenEffectsQuality::Low, ScreenEffectsQuality::Medium, ScreenEffectsQuality::High}) {
            CAPTURE(nameOf(quality));
            // From straight above, at a slant, nearly along it, and with perspective.
            for (const CameraView& view :
                 {scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f)),
                  scene.orthographicView(Vec3(3.0f, 3.0f, 2.0f), Vec3(0.0f), 2.0f),
                  scene.orthographicView(Vec3(6.0f, 0.6f, 1.0f), Vec3(0.0f), 2.0f),
                  scene.perspectiveView(Vec3(1.0f, 1.2f, 3.0f), Vec3(0.0f, 0.0f, -2.0f)),
                  scene.perspectiveView(Vec3(0.0f, 0.3f, 3.0f), Vec3(0.0f, 0.0f, -4.0f))}) {
                // Without a bias, which would let through what barely rises above the
                // floor, and on a background no floor could be mistaken for.
                SceneRenderSettings settings = occlusionSettings(quality, lookWith(0.5f));
                settings.backgroundColor = Vec3(1.0f, 0.0f, 0.0f);
                const Image image = scene.render(view, settings);
                float least = 1.0f;
                uint32_t floorPixels = 0;
                for (uint32_t y = 0; y < kSize; ++y) {
                    for (uint32_t x = 0; x < kSize; ++x) {
                        const Vec4 pixel = image.pixel(x, y);
                        // Where the floor is, the picture is a grey.
                        if (pixel.r == pixel.g) {
                            least = std::min(least, pixel.r);
                            ++floorPixels;
                        }
                    }
                }
                CHECK(floorPixels > kSize * kSize / 4);
                // Not nearly: the floor's own pixels rise above it by nothing at all,
                // even where a pixel covers a metre of it.
                CHECK(least == 1.0f);
                // The two passes of the ambient occlusion, the view of it, and its encoding.
                CHECK(scene.renderer->statistics().screenPasses == 4);
            }
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the reference the pictures are held against says what a wall without end must") {
        // At the foot of a wall half of the sky is hidden from a floor, whatever the
        // floor is seen from; of a ramp or an overhang, half of one plus the cosine of
        // the angle between "up" and the way their face looks (AmbientOcclusionTests,
        // without a GPU). A wall that is looked at as far as it goes, from close by.
        const Corner corner;
        for (const float lean : {-30.0f, 0.0f, 30.0f, 60.0f}) {
            CAPTURE(lean);
            const Reference endless{.lean = lean, .radius = 1000.0f, .limitedByThePicture = false};
            const float expected = 0.5f * (1.0f + std::cos(toRadians(90.0f - lean)));
            for (const auto& [elevation, around] :
                 std::initializer_list<std::pair<float, float>>{{45.0f, 0.0f}, {25.0f, 30.0f}}) {
                CAPTURE(elevation);
                CHECK(endless.at(onFloor(0.02f), corner.view(elevation, around)) ==
                      doctest::Approx(expected).epsilon(0.02).scale(1.0));
            }
        }
        // With a radius, only what is within it hides anything: more light at the foot
        // of the wall than half, and all of it once the wall is a radius away.
        const Reference half{.radius = 0.5f};
        const CameraView view = corner.view();
        CHECK(half.at(onFloor(0.04f), view) > 0.52f);
        CHECK(half.at(onFloor(0.04f), view) < 0.7f);
        CHECK(half.at(onFloor(0.2f), view) > half.at(onFloor(0.1f), view) + 0.05f);
        CHECK(half.at(onFloor(0.5f), view) == doctest::Approx(1.0f));
        CHECK(half.at(onFloor(0.8f), view) == 1.0f);
    }

    TEST_CASE("at the foot of a wall about half the light is kept away, and a radius from it none") {
        Corner corner;
        const CameraView view = corner.view();
        const Image image = corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f)));
        const Reference reference{.radius = 0.5f};

        float before = 0.0f;
        for (const float distance : {0.04f, 0.08f, 0.12f, 0.16f, 0.2f, 0.25f, 0.3f, 0.36f, 0.42f, 0.5f, 0.6f, 0.8f}) {
            CAPTURE(distance);
            const float here = valueAt(image, view, onFloor(distance));
            CHECK(here == doctest::Approx(reference.at(onFloor(distance), view)).epsilon(kTolerance).scale(1.0));
            // Away from the wall the light comes back, steadily.
            CHECK(here >= before - 0.02f);
            before = here;
        }
        CHECK(valueAt(image, view, onFloor(0.04f)) < 0.72f);
        CHECK(valueAt(image, view, onFloor(0.6f)) >= 0.99f);
        CHECK(valueAt(image, view, onFloor(0.8f)) == 1.0f);
        // The wall just above the floor is in the same corner: as dark.
        CHECK(valueAt(image, view, Vec3(0.0f, 0.04f, 0.0f)) ==
              doctest::Approx(valueAt(image, view, onFloor(0.04f))).epsilon(0.05).scale(1.0));
        // And it is the same all along the wall.
        for (const float along : {-0.7f, -0.3f, 0.4f, 0.8f}) {
            CAPTURE(along);
            CHECK(valueAt(image, view, onFloor(0.1f, along)) ==
                  doctest::Approx(valueAt(image, view, onFloor(0.1f))).epsilon(0.06).scale(1.0));
        }
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("how dark a corner is does not depend on where it is seen from, or how") {
        Corner corner;
        const SceneRenderSettings settings = occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f));
        const Reference reference{.radius = 0.5f};
        const Vec3 towardsEye = glm::normalize(Vec3(1.0f, 1.0f, 0.0f));

        // Higher, lower and from the side; from further away; and with perspective,
        // near and far: the radius is in metres.
        for (const CameraView& view :
             {corner.view(30.0f), corner.view(60.0f), corner.view(45.0f, 35.0f), corner.view(35.0f, -40.0f),
              corner.view(20.0f, 20.0f), corner.view(45.0f, 0.0f, 4.0f),
              corner.scene.perspectiveView(towardsEye * 2.5f, Vec3(0.0f), 40.0f),
              corner.scene.perspectiveView(towardsEye * 5.0f, Vec3(0.0f), 30.0f),
              corner.scene.perspectiveView(Vec3(2.0f, 1.2f, 1.5f), Vec3(0.0f, 0.2f, 0.0f), 45.0f)}) {
            CAPTURE(view.position);
            CAPTURE(view.orthographic);
            const Image image = corner.scene.render(view, settings);
            for (const float distance : {0.06f, 0.15f, 0.3f, 0.6f}) {
                CAPTURE(distance);
                CHECK(valueAt(image, view, onFloor(distance)) ==
                      doctest::Approx(reference.at(onFloor(distance), view)).epsilon(kTolerance).scale(1.0));
            }
        }
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("the radius says how far a wall reaches into the light") {
        Corner corner;
        const CameraView view = corner.view();
        for (const float radius : {0.15f, 0.3f, 0.5f}) {
            CAPTURE(radius);
            const Image image =
                corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(radius)));
            const Reference reference{.radius = radius};
            for (const float part : {0.2f, 0.3f, 0.5f, 0.8f, 1.2f}) {
                CAPTURE(part);
                CHECK(valueAt(image, view, onFloor(part * radius)) ==
                      doctest::Approx(reference.at(onFloor(part * radius), view)).epsilon(kTolerance).scale(1.0));
            }
            CHECK(valueAt(image, view, onFloor(0.2f * radius)) < 0.85f);
            CHECK(valueAt(image, view, onFloor(1.2f * radius)) == 1.0f);
        }
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("a ramp keeps less of the light away than a wall, and what hangs over the floor more") {
        struct Case {
            float lean;
            float elevation;
        };

        float last = 0.0f;
        for (const Case& slope : {Case{-30.0f, 18.0f}, Case{0.0f, 40.0f}, Case{30.0f, 40.0f}, Case{55.0f, 50.0f}}) {
            CAPTURE(slope.lean);
            Corner corner(slope.lean);
            const CameraView view = corner.view(slope.elevation);
            const Image image =
                corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f)));
            const Reference reference{.lean = slope.lean, .radius = 0.5f};
            for (const float distance : {0.05f, 0.12f, 0.25f}) {
                CAPTURE(distance);
                CHECK(valueAt(image, view, onFloor(distance)) ==
                      doctest::Approx(reference.at(onFloor(distance), view)).epsilon(kTolerance).scale(1.0));
            }
            const float atTheFoot = valueAt(image, view, onFloor(0.05f));
            CHECK(atTheFoot > last + 0.05f);
            last = atTheFoot;
            checkNoValidationIssues(*corner.scene.context.graphics);
        }
    }

    TEST_CASE("the intensity is a power: none of it is no occlusion, twice of it the square") {
        Corner corner;
        const CameraView view = corner.view();
        const Image plain = corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f)));
        CHECK(corner.scene.renderer->statistics().screenPasses == 4);
        const Image doubled =
            corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f, 0.0f, 2.0f)));
        const Image halved =
            corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f, 0.0f, 0.5f)));
        for (const float distance : {0.05f, 0.1f, 0.18f}) {
            CAPTURE(distance);
            const float once = valueAt(plain, view, onFloor(distance));
            CHECK(once < 0.95f);
            CHECK(valueAt(doubled, view, onFloor(distance)) == doctest::Approx(once * once).epsilon(0.02).scale(1.0));
            CHECK(valueAt(halved, view, onFloor(distance)) ==
                  doctest::Approx(std::sqrt(once)).epsilon(0.02).scale(1.0));
        }

        // None at all: nothing is drawn for it, and the view of it is white.
        Image none =
            corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f, 0.0f, 0.0f)));
        CHECK(corner.scene.renderer->statistics().screenPasses == 2);
        CHECK(valueAt(none, view, onFloor(0.05f)) == 1.0f);
        // And so it is when the view is to have no screen effects, whatever the scene says.
        none = corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::Off, lookWith(0.5f, 0.0f, 3.0f)));
        CHECK(corner.scene.renderer->statistics().screenPasses == 2);
        CHECK(valueAt(none, view, onFloor(0.05f)) == 1.0f);
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("the bias lets through what rises only a little above a surface") {
        // A ramp of 60 degrees. With a bias of 25, it stands in the light as one of 35 would.
        Corner corner(30.0f);
        const CameraView view = corner.view(45.0f);
        const Image without = corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f)));
        const Image with =
            corner.scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f, 25.0f)));
        const Reference plain{.lean = 30.0f, .radius = 0.5f};
        const Reference biased{.lean = 30.0f, .radius = 0.5f, .bias = 25.0f};
        for (const float distance : {0.04f, 0.07f, 0.12f}) {
            CAPTURE(distance);
            const float found = valueAt(with, view, onFloor(distance));
            CHECK(found == doctest::Approx(biased.at(onFloor(distance), view)).epsilon(kTolerance).scale(1.0));
            CHECK(valueAt(without, view, onFloor(distance)) ==
                  doctest::Approx(plain.at(onFloor(distance), view)).epsilon(kTolerance).scale(1.0));
        }
        // Close to the foot of the ramp, where it keeps most of the light away, the bias
        // gives a good part of it back.
        CHECK(biased.at(onFloor(0.04f), view) > plain.at(onFloor(0.04f), view) + 0.08f);
        CHECK(valueAt(with, view, onFloor(0.04f)) > valueAt(without, view, onFloor(0.04f)) + 0.06f);
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("light from all around is dimmed by it - ambient light and an environment - and the lights are not") {
        Corner corner;
        const CameraView view = corner.view();
        SceneFixture& scene = corner.scene;
        const Vec3 nearTheWall = onFloor(0.06f);
        const Vec3 inTheOpen = onFloor(0.8f);
        const float share = valueAt(scene.render(view, occlusionSettings()), view, nearTheWall);
        REQUIRE(share < 0.8f);
        const auto colorAt = [&](const Image& image, const Vec3& point) {
            const Vec2 pixel = pixelOf(view, point);
            return Vec3(image.pixel(static_cast<uint32_t>(pixel.x), static_cast<uint32_t>(pixel.y)));
        };

        // Ambient light, the same from above and below: the floor shows it, less what
        // the wall keeps away.
        const Vec3 ambient(0.8f, 0.6f, 0.4f);
        scene.setAmbient(ambient, ambient);
        Image image = scene.render(view, litSettings());
        // Two passes for the ambient occlusion, the lighting, the encoding.
        CHECK(scene.renderer->statistics().screenPasses == 4);
        CHECK_NEAR(colorAt(image, inTheOpen), displayed(ambient), 0.02f);
        CHECK_NEAR(colorAt(image, nearTheWall), displayed(ambient * share), 0.03f);
        // Without the screen effects it is as bright there as anywhere.
        image = scene.render(view, litSettings(ScreenEffectsQuality::Off));
        CHECK(scene.renderer->statistics().screenPasses == 2);
        CHECK_NEAR(colorAt(image, nearTheWall), displayed(ambient), 0.02f);

        // The light of an environment is light from all around too.
        scene.setAmbient(Vec3(0.0f), Vec3(0.0f));
        Image even = unwrap(Image::create(32, 16, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < even.height(); ++y) {
            for (uint32_t x = 0; x < even.width(); ++x) {
                even.setPixel(x, y, Vec4(0.5f, 0.75f, 0.25f, 1.0f));
            }
        }
        scene.writeHdrImage("even", even);
        REQUIRE(scene.assets->refresh().has_value());
        scene.setEnvironment(scene.asset("even.hdr"));
        image = scene.render(view, litSettings());
        CHECK(scene.renderer->statistics().screenPasses == 4);
        CHECK_NEAR(colorAt(image, inTheOpen), displayed(Vec3(0.5f, 0.75f, 0.25f)), 0.02f);
        CHECK_NEAR(colorAt(image, nearTheWall), displayed(Vec3(0.5f, 0.75f, 0.25f) * share), 0.03f);
        scene.world.setEnabled(scene.environmentEntity(), false);

        // The light of a light is not dimmed: shadows are for that. And where there is
        // no light from around, nothing is worked out.
        scene.setSun(Vec3(0.3f, 1.0f, 0.2f), Vec3(2.0f));
        image = scene.render(view, litSettings());
        CHECK(scene.renderer->statistics().screenPasses == 2);
        CHECK(colorAt(image, nearTheWall).r == doctest::Approx(colorAt(image, inTheOpen).r).epsilon(0.01));
        CHECK(colorAt(image, nearTheWall).r > 0.5f);

        // With both, only the part from all around goes down.
        const Vec3 sunOnTheFloor = srgbToLinear(colorAt(image, inTheOpen));
        scene.setAmbient(Vec3(0.2f), Vec3(0.2f));
        image = scene.render(view, litSettings());
        CHECK_NEAR(colorAt(image, inTheOpen), displayed(sunOnTheFloor + Vec3(0.2f)), 0.02f);
        CHECK_NEAR(colorAt(image, nearTheWall), displayed(sunOnTheFloor + Vec3(0.2f) * share), 0.03f);

        // The view of the light alone has it too; the view of the reflections, which
        // asks what there is to reflect, has not.
        SceneRenderSettings lightingOnly = litSettings();
        lightingOnly.debugView = RenderDebugView::LightingOnly;
        scene.render(view, lightingOnly);
        CHECK(scene.renderer->statistics().screenPasses == 4);
        SceneRenderSettings reflections = litSettings();
        reflections.debugView = RenderDebugView::Reflections;
        scene.render(view, reflections);
        CHECK(scene.renderer->statistics().screenPasses == 2);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what a material says of its own occlusion and what the screen adds multiply") {
        SceneFixture scene(UVec2(kSize, kSize));
        Image half = unwrap(Image::create(4, 4, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                half.setPixel(x, y, Vec4(0.5f, 0.5f, 0.5f, 1.0f));
            }
        }
        scene.writeImage("half", half);
        scene.open();
        scene.writeMatteMaterial("matte");
        scene.writeMatteMaterial("shaded", Vec3(1.0f),
                                 fmt::format(R"("occlusionTexture": "{}")", scene.asset("half.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        scene.add(BuiltinMesh::Cube, scene.material("shaded"), at(Vec3(2.0f, -0.5f, 0.0f), Vec3(12.0f, 1.0f, 12.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(-0.5f, 3.0f, 0.0f), Vec3(1.0f, 6.0f, 12.0f)));
        const CameraView view = scene.orthographicView(glm::normalize(Vec3(1.0f, 1.0f, 0.0f)) * 6.0f, Vec3(0.0f), 2.0f);
        scene.setAmbient(Vec3(0.8f), Vec3(0.8f));

        const float share = valueAt(scene.render(view, occlusionSettings()), view, onFloor(0.06f));
        REQUIRE(share < 0.8f);
        const Image image = scene.render(view, litSettings());
        // Half, as eight bits hold it.
        const float stored = 128.0f / 255.0f;
        CHECK(valueAt(image, view, onFloor(0.8f)) == doctest::Approx(displayed(Vec3(0.8f * stored)).r).epsilon(0.02));
        CHECK(valueAt(image, view, onFloor(0.06f)) ==
              doctest::Approx(displayed(Vec3(0.8f * stored * share)).r).epsilon(0.04));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a thin thing far in front of a wall casts no darkness on it, and none comes up from under a thing") {
        SceneFixture scene(UVec2(kSize, kSize));
        scene.writeMatteMaterial("matte");
        scene.open();
        // A wall across the view, and a pole two and a half metres in front of it.
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f, 0.0f, -3.0f), Vec3(12.0f, 12.0f, 1.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(-0.5f, 0.0f, 0.0f), Vec3(0.06f, 4.0f, 0.06f)));
        const CameraView front = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        Image image = scene.render(front, occlusionSettings());
        // The pole is four pixels wide around column 32; the wall beside it is as open
        // as the wall far from it.
        for (const uint32_t x : {22u, 26u, 28u, 36u, 38u, 42u, 100u}) {
            for (const uint32_t y : {20u, 64u, 110u}) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(image.pixel(x, y).r == 1.0f);
            }
        }

        // A box on a floor, seen from above at a slant.
        scene.world.clear();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f, -0.5f, 0.0f), Vec3(12.0f, 1.0f, 12.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f, 0.3f, 0.0f), Vec3(0.6f)));
        const CameraView above = scene.orthographicView(Vec3(1.5f, 5.0f, 2.5f), Vec3(0.0f, 0.3f, 0.0f), 2.0f);
        image = scene.render(above, occlusionSettings());
        // The floor at the foot of the box is dark. The top of the box, a pixel or two
        // from the edge beside which that dark floor is seen, is not.
        CHECK(valueAt(image, above, Vec3(0.34f, 0.0f, 0.0f)) < 0.8f);
        CHECK(valueAt(image, above, Vec3(0.0f, 0.0f, 0.34f)) < 0.8f);
        for (const Vec3& onTop : {Vec3(0.26f, 0.6f, 0.0f), Vec3(0.0f, 0.6f, 0.26f), Vec3(0.26f, 0.6f, 0.26f),
                                  Vec3(-0.26f, 0.6f, 0.2f), Vec3(0.0f, 0.6f, 0.0f)}) {
            CAPTURE(onTop);
            CHECK(valueAt(image, above, onTop) >= 0.98f);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("every quality finds the same darkness, the higher the more finely") {
        Corner corner;
        const CameraView view = corner.view();
        const Reference reference{.radius = 0.5f};
        for (const ScreenEffectsQuality quality :
             {ScreenEffectsQuality::Low, ScreenEffectsQuality::Medium, ScreenEffectsQuality::High}) {
            CAPTURE(nameOf(quality));
            const Image image = corner.scene.render(view, occlusionSettings(quality, lookWith(0.5f)));
            CHECK(corner.scene.renderer->statistics().screenPasses == 4);
            for (const float distance : {0.06f, 0.15f, 0.25f, 0.4f, 0.7f}) {
                CAPTURE(distance);
                // Along the wall the lower qualities waver; on average they agree with
                // what looking everywhere finds.
                float sum = 0.0f;
                float least = 1.0f;
                float most = 0.0f;
                constexpr int kPlaces = 9;
                for (int place = 0; place < kPlaces; ++place) {
                    const float found =
                        valueAt(image, view, onFloor(distance, -0.8f + 0.2f * static_cast<float>(place)));
                    sum += found;
                    least = std::min(least, found);
                    most = std::max(most, found);
                }
                CHECK(sum / kPlaces == doctest::Approx(reference.at(onFloor(distance), view)).epsilon(0.09).scale(1.0));
                CHECK(most - least < (quality == ScreenEffectsQuality::High ? 0.08f : 0.15f));
            }
        }
        checkNoValidationIssues(*corner.scene.context.graphics);
    }

    TEST_CASE("the image the occlusion is worked out in belongs to the view: as large as the quality asks, and "
              "given back when the view draws none") {
        SceneFixture scene(UVec2(kSize, kSize));
        scene.writeMatteMaterial("matte");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("matte"), at(Vec3(0.0f, -0.5f, 0.0f), Vec3(12.0f, 1.0f, 12.0f)));
        const CameraView view = scene.orthographicView(Vec3(3.0f, 3.0f, 2.0f), Vec3(0.0f), 2.0f);
        const SceneRenderer& renderer = *scene.renderer;

        // Nothing is worked out, nothing is held.
        scene.render(view, occlusionSettings(ScreenEffectsQuality::Off));
        CHECK(renderer.statistics().ambientOcclusionBytes == 0);
        CHECK(scene.targets.tracedOcclusion() == nullptr);
        CHECK(scene.targets.tracedOcclusionBytes() == 0);

        // For every pixel: two half floats each. For one in four: a quarter of that,
        // in place of the first.
        scene.render(view, occlusionSettings(ScreenEffectsQuality::High));
        CHECK(renderer.statistics().ambientOcclusionBytes == uint64_t{kSize} * kSize * 4);
        REQUIRE(scene.targets.tracedOcclusion() != nullptr);
        CHECK(scene.targets.tracedOcclusion()->getDesc().width == kSize);
        scene.render(view, occlusionSettings(ScreenEffectsQuality::Medium));
        CHECK(renderer.statistics().ambientOcclusionBytes == uint64_t{kSize / 2} * (kSize / 2) * 4);
        nvrhi::ITexture* const kept = scene.targets.tracedOcclusion();
        scene.render(view, occlusionSettings(ScreenEffectsQuality::Low));
        CHECK(scene.targets.tracedOcclusion() == kept);
        // A view of something else for a frame - the base color - keeps it; it is the
        // settings that say whether the view has ambient occlusion.
        SceneRenderSettings baseColor = occlusionSettings(ScreenEffectsQuality::Medium);
        baseColor.debugView = RenderDebugView::BaseColor;
        scene.render(view, baseColor);
        CHECK(scene.targets.tracedOcclusion() == kept);

        {
            // A second view, of a size that does not divide: a texel for every block of
            // pixels, the last of which hangs over the edge. Each view has its own.
            SceneTargets other = unwrap(SceneTargets::create(scene.context.device(), UVec2(63, 31), "Other"));
            const CameraView wide = makeCameraView(glm::inverse(lookAt(Vec3(3.0f, 3.0f, 2.0f), Vec3(0.0f), kWorldUp)),
                                                   CameraComponent(), 63.0f / 31.0f);
            recordAndExecute(scene.context, [&](nvrhi::ICommandList* commandList) {
                scene.renderer->render(commandList, scene.world, wide, other,
                                       occlusionSettings(ScreenEffectsQuality::Medium));
            });
            CHECK(renderer.statistics().ambientOcclusionBytes == uint64_t{32} * 16 * 4);
            CHECK(other.tracedOcclusionBytes() == uint64_t{32} * 16 * 4);
            CHECK(scene.targets.tracedOcclusion() == kept);
            // Its picture is one of ambient occlusion on an open floor: white, to the
            // last row and the last column.
            const Image image = test::readToImage(*scene.context.graphics, other.display());
            CHECK(image.pixel(31, 26).r == 1.0f);
            CHECK(image.pixel(62, 30).r == 1.0f);
            CHECK(image.pixel(0, 30).r == 1.0f);

            // Over the views of a frame it is the largest that is reported.
            scene.render(view, occlusionSettings(ScreenEffectsQuality::Medium));
            CHECK(scene.renderer->takeFrameStatistics().ambientOcclusionBytes == uint64_t{kSize} * kSize * 4);

            // Targets of another size have none until it is drawn for them again.
            REQUIRE(other.resize(scene.context.device(), UVec2(40, 20)).has_value());
            CHECK(other.tracedOcclusion() == nullptr);
            // Asked for by hand: a divisor of four, and one that there is not.
            REQUIRE(other.prepareTracedOcclusion(scene.context.device(), 4).has_value());
            CHECK(other.tracedOcclusionBytes() == uint64_t{10} * 5 * 4);
            CHECK(other.tracedOcclusionFramebuffer() != nullptr);
#if defined(CORDITE_ENABLE_ASSERTS)
            CHECK_THROWS_AS((void)other.prepareTracedOcclusion(scene.context.device(), 0), test::AssertionFailure);
            CHECK_THROWS_AS((void)other.prepareTracedOcclusion(nullptr, 2), test::AssertionFailure);
#else
            CHECK_FALSE(other.prepareTracedOcclusion(scene.context.device(), 0).has_value());
            CHECK_FALSE(other.prepareTracedOcclusion(nullptr, 2).has_value());
#endif
            other.releaseTracedOcclusion();
            CHECK(other.tracedOcclusion() == nullptr);
            CHECK(other.tracedOcclusionBytes() == 0);
            scene.context.graphics->waitIdle();
        }

        // The view is to have no ambient occlusion any more: its image is given back.
        scene.render(view, occlusionSettings(ScreenEffectsQuality::Off));
        CHECK(renderer.statistics().ambientOcclusionBytes == 0);
        CHECK(scene.targets.tracedOcclusion() == nullptr);
        // And so it is when the scene asks for none.
        scene.render(view, occlusionSettings(ScreenEffectsQuality::High));
        CHECK(scene.targets.tracedOcclusion() != nullptr);
        scene.render(view, occlusionSettings(ScreenEffectsQuality::High, lookWith(0.5f, 0.0f, 0.0f)));
        CHECK(scene.targets.tracedOcclusion() == nullptr);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
