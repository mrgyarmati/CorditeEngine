#include "cordite/assets/ImageCompare.h"
#include "cordite/assets/Primitives.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/LightShaftPass.h"
#include "cordite/renderer/LightShafts.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels wide and high: ninety degrees of the sky, so that a
/// pixel near the middle is about seven tenths of a degree.
constexpr uint32_t kSize = 128;

/// Settings that leave the light as it is - no curve, no bloom - and add no shafts.
PostProcessSettings plain() {
    PostProcessSettings look;
    look.toneMapper = ToneMapper::Linear;
    look.bloomIntensity = 0.0f;
    return look;
}

/// plain(), with light shafts of `intensity` that reach all the way to the sun.
PostProcessSettings withShafts(float intensity) {
    PostProcessSettings look = plain();
    look.lightShaftIntensity = intensity;
    look.lightShaftThreshold = 1.0f;
    look.lightShaftLength = 1.0f;
    return look;
}

/// A sky of `background` light, looked at from the origin along -Z, ninety degrees from
/// top to bottom, with a sun of white light - a color of one - towards `towardsSun`.
struct Sky {
    explicit Sky(const Vec3& towardsSun, const Vec3& background = Vec3(0.0f)) : background(background) {
        scene.writeFlatMaterial("black", Vec3(0.0f));
        scene.open();
        scene.setSun(towardsSun, Vec3(kPi));
        view = scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 90.0f);
    }

    Image render(const PostProcessSettings& look, RenderDebugView debugView = RenderDebugView::None) {
        SceneRenderSettings settings;
        settings.backgroundColor = background;
        settings.antiAliasing = AntiAliasing::Off;
        settings.postProcess = look;
        settings.debugView = debugView;
        return scene.render(view, settings);
    }

    /// The shafts alone, as their debug view shows them: their light, encoded for a display.
    Image shafts(const PostProcessSettings& look) { return render(look, RenderDebugView::LightShafts); }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene{UVec2(kSize)};
    CameraView view;
    Vec3 background;
};

/// The light of the shafts at a pixel of their debug view.
float shaftAt(const Image& shafts, uint32_t x, uint32_t y) {
    return srgbToLinear(shafts.pixel(x, y).r);
}

} // namespace

TEST_SUITE("LightShafts") {
    TEST_CASE("without intensity, with the sun behind, or without perspective, nothing is drawn or changed") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f));
        const Image without = sky.render(plain());
        const uint32_t passesWithout = sky.statistics().screenPasses;
        CHECK(unwrap(compareImages(sky.render(withShafts(0.0f)), without)).maxChannelDifference == 0);
        CHECK(sky.statistics().screenPasses == passesWithout);

        // With the sun ahead there are five passes, in two images half the size.
        sky.render(withShafts(1.0f));
        CHECK(sky.statistics().screenPasses == passesWithout + 5);
        CHECK(sky.statistics().lightShaftBytes == uint64_t{kSize / 2} * (kSize / 2) * 8 * 2);

        // Behind the camera, the sun has no point in the picture to send shafts from.
        sky.scene.setSun(Vec3(0.0f, 0.0f, 1.0f), Vec3(kPi));
        const Image behindWithout = sky.render(plain());
        CHECK(unwrap(compareImages(sky.render(withShafts(1.0f)), behindWithout)).maxChannelDifference == 0);
        CHECK(sky.statistics().screenPasses == passesWithout);
        CHECK(sky.statistics().lightShaftBytes == 0);

        // Without perspective every ray would be parallel to every other.
        sky.scene.setSun(Vec3(0.0f, 0.0f, -1.0f), Vec3(kPi));
        sky.view = sky.scene.orthographicView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 10.0f);
        const Image flatWithout = sky.render(plain());
        CHECK(unwrap(compareImages(sky.render(withShafts(1.0f)), flatWithout)).maxChannelDifference == 0);
        CHECK(sky.statistics().lightShaftBytes == 0);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("the shafts are brightest where the sun is, and fade along the way from it") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f));
        const Image shafts = sky.shafts(withShafts(0.5f));
        // Where the sun is, about as much as the sun gives: its color times the intensity.
        CHECK(std::abs(shaftAt(shafts, 64, 64) - 0.5f) < 0.03f);
        // Along a row, and down a column, less and less.
        float before = shaftAt(shafts, 64, 64);
        for (const uint32_t offset : {4u, 8u, 16u, 32u, 60u}) {
            CAPTURE(offset);
            const float right = shaftAt(shafts, 64 + offset, 64);
            const float down = shaftAt(shafts, 64, 64 + offset);
            CHECK(right < before);
            CHECK(std::abs(down - right) < 0.02f);
            before = right;
        }
        // The picture, finished, is that much brighter.
        const Image finished = sky.render(withShafts(0.5f));
        const Image dark = sky.render(plain());
        CHECK(finished.pixel(64, 64).r > dark.pixel(64, 64).r + 0.3f);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("the shafts reach as far as their length says") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f));
        PostProcessSettings shortShafts = withShafts(1.0f);
        shortShafts.lightShaftLength = 0.25f;
        const Image full = sky.shafts(withShafts(1.0f));
        const Image partway = sky.shafts(shortShafts);
        // Fifty pixels from the sun, a quarter of the way does not get to its bright
        // middle, which all of the way does.
        CHECK(shaftAt(partway, 114, 64) < shaftAt(full, 114, 64) * 0.5f);
        // At the sun itself the way has no length: the same.
        CHECK(std::abs(shaftAt(partway, 64, 64) - shaftAt(full, 64, 64)) < 0.02f);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("towards the side of the view the shafts fade, as lightShaftFade says") {
        // Eighty degrees to the right, in a picture 170 degrees wide: in it, and faded.
        const float angle = glm::radians(80.0f);
        const Vec3 towards(std::sin(angle), 0.0f, -std::cos(angle));
        Sky sky(towards);
        sky.view = sky.scene.perspectiveView(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), 170.0f);
        const float fade = lightShaftFade(sky.view, towards);
        REQUIRE(fade < 0.8f);
        const std::optional<Vec2> point = sunScreenPoint(sky.view, towards);
        REQUIRE(point.has_value());
        REQUIRE(point->x < 1.0f);
        const Image shafts = sky.shafts(withShafts(0.5f));
        const auto x = static_cast<uint32_t>(point->x * static_cast<float>(kSize));
        CHECK(std::abs(shaftAt(shafts, x, 64) - 0.5f * fade) < 0.03f);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("what stands in the way of the sun casts a shadow into its shafts") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f));
        // Black, five metres ahead, on the left of the sun - from six to eighteen pixels left
        // of it - and as high as it is.
        sky.scene.add(BuiltinMesh::Cube, sky.scene.material("black"),
                      at(Vec3(-0.9375f, 0.0f, -5.0f), Vec3(0.9375f, 1.25f, 0.01f)));
        const Image shafts = sky.shafts(withShafts(1.0f));
        // Beyond it the shafts are darker than as far on the other side, where nothing is
        // in the way.
        for (const uint32_t offset : {28u, 40u}) {
            CAPTURE(offset);
            const float shadowed = shaftAt(shafts, 64 - offset, 64);
            const float open = shaftAt(shafts, 64 + offset, 64);
            CHECK(open > 0.0f);
            CHECK(shadowed < open * 0.8f);
        }
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("a sun out of the picture sends its shafts in from the edge") {
        // Fifty degrees to the right: five degrees beyond the edge.
        const float angle = glm::radians(50.0f);
        Sky sky(Vec3(std::sin(angle), 0.0f, -std::cos(angle)));
        const Image shafts = sky.shafts(withShafts(1.0f));
        const float edge = shaftAt(shafts, kSize - 1, 64);
        const float middle = shaftAt(shafts, 64, 64);
        CHECK(edge > 0.05f);
        CHECK(edge > middle);
        CHECK(middle > shaftAt(shafts, 0, 64));
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("a sky ten thousand times as bright as white gives the shafts no more than the most there is") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f), Vec3(10000.0f));
        const Image shafts = sky.shafts(withShafts(0.1f));
        CHECK(shaftAt(shafts, 64, 64) > 0.1f);
        CHECK(shaftAt(shafts, 64, 64) <= 0.1f * kMaxLightShaftSource + 0.01f);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }

    TEST_CASE("the debug view of the shafts shows nothing in a scene that has none") {
        Sky sky(Vec3(0.0f, 0.0f, -1.0f), Vec3(0.5f));
        const Image none = sky.shafts(plain());
        CHECK(std::ranges::all_of(none.pixels(),
                                  [](std::byte value) { return value == std::byte{0} || value == std::byte{255}; }));
        // Nor with the sun behind.
        sky.scene.setSun(Vec3(0.0f, 0.0f, 1.0f), Vec3(kPi));
        CHECK(shaftAt(sky.shafts(withShafts(1.0f)), 64, 64) == 0.0f);
        checkNoValidationIssues(*sky.scene.context.graphics);
    }
}
