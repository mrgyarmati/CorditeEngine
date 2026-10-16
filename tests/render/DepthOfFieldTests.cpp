#include "cordite/assets/ImageCompare.h"
#include "cordite/assets/Primitives.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/DepthOfFieldPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkShown;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels wide and high, and show this many metres of the
/// ground, seen from straight above: 32 pixels a metre.
constexpr uint32_t kSize = 128;
constexpr float kSeen = 4.0f;
/// How high above the ground the camera is.
constexpr float kEyeHeight = 8.0f;

/// Settings that leave the light as it is - no curve, no bloom - and blur nothing.
PostProcessSettings plain() {
    PostProcessSettings look;
    look.toneMapper = ToneMapper::Linear;
    look.bloomIntensity = 0.0f;
    return look;
}

/// plain(), sharp from half a metre in front of `focus` metres to half a metre behind,
/// blurred at the most a metre beyond that - in front by `nearBlur`, behind by `farBlur` -
/// and as widely as a blur can be: a radius of almost four pixels in these pictures.
PostProcessSettings focusedAt(float focus, float nearBlur, float farBlur) {
    PostProcessSettings look = plain();
    look.focusDistance = focus;
    look.focusRange = 0.5f;
    look.focusTransition = 1.0f;
    look.nearBlur = nearBlur;
    look.farBlur = farBlur;
    look.blurSize = kMaxBlurSize;
    return look;
}

/// A black background, and the picture finished with `look` whatever the world says -
/// with its edges left as they are, which anti-aliasing would change.
SceneRenderSettings finishedWith(const PostProcessSettings& look, RenderDebugView debugView,
                                 PostProcessQuality quality) {
    SceneRenderSettings settings;
    settings.backgroundColor = Vec3(0.0f);
    settings.antiAliasing = AntiAliasing::Off;
    settings.postProcess = look;
    settings.debugView = debugView;
    settings.postProcessQuality = quality;
    return settings;
}

/// Flat planes that give off light, seen from straight above: what is `depth` metres
/// below the camera is that far from it, along the way it looks.
struct Stage {
    Stage() {
        scene.writeFlatMaterial("white", Vec3(1.0f));
        scene.writeFlatMaterial("grey", Vec3(0.5f));
        scene.writeFlatMaterial("black", Vec3(0.0f));
        // Half see-through: drawn forward, and not in the depth buffer.
        scene.writeFlatMaterial("glass", Vec3(1.0f), R"("baseColor": [0, 0, 0, 0.5], "alphaMode": "blended")");
        scene.open();
        view = scene.orthographicView(Vec3(0.0f, kEyeHeight, 0.0f), Vec3(0.0f), kSeen, Vec3(0.0f, 0.0f, -1.0f));
    }

    /// A plane of `material` under all of the picture, `depth` metres from the camera.
    void ground(std::string_view material, float depth) {
        scene.add(BuiltinMesh::Plane, scene.material(material), at(Vec3(0.0f, kEyeHeight - depth, 0.0f), Vec3(40.0f)));
    }

    /// White stripes four pixels wide and four apart, across all of the picture, on black
    /// a centimetre behind them.
    void stripes(float depth) {
        ground("black", depth + 0.01f);
        for (int stripe = 0; stripe < 16; ++stripe) {
            const float left = -kSeen * 0.5f + 0.25f * static_cast<float>(stripe);
            scene.add(BuiltinMesh::Plane, scene.material("white"),
                      at(Vec3(left + 0.0625f, kEyeHeight - depth, 0.0f), Vec3(0.125f, 1.0f, 40.0f)));
        }
    }

    /// A square of `material` a metre wide - the pixels from 48 to 79 both ways - in the
    /// middle of the picture.
    void square(std::string_view material, float depth) {
        scene.add(BuiltinMesh::Plane, scene.material(material), at(Vec3(0.0f, kEyeHeight - depth, 0.0f)));
    }

    Image render(const PostProcessSettings& look, RenderDebugView debugView = RenderDebugView::None,
                 PostProcessQuality quality = PostProcessQuality::Medium) {
        return scene.render(view, finishedWith(look, debugView, quality));
    }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene{UVec2(kSize)};
    CameraView view;
};

/// How far apart the brightest and the darkest pixel of the middle half of the middle
/// row are: 1 from black to white.
float contrastOf(const Image& image) {
    float low = 1.0f;
    float high = 0.0f;
    for (uint32_t x = kSize / 4; x < kSize * 3 / 4; ++x) {
        const float value = image.pixel(x, kSize / 2).r;
        low = std::min(low, value);
        high = std::max(high, value);
    }
    return high - low;
}

/// By how many of 255 steps two pictures differ at the most.
uint32_t mostDifference(const Image& first, const Image& second) {
    return unwrap(compareImages(first, second)).maxChannelDifference;
}

/// How far a pixel is outside the square of Stage::square(), in pixels along the nearer
/// axis; 0 inside it.
uint32_t outsideSquare(uint32_t x, uint32_t y) {
    const auto outside = [](uint32_t value) -> uint32_t {
        return value < 48 ? 48 - value : (value > 79 ? value - 79 : 0);
    };
    return std::max(outside(x), outside(y));
}

} // namespace

TEST_SUITE("DepthOfField") {
    TEST_CASE("what is in focus is left exactly as it was, and blurring costs three passes and two images") {
        Stage stage;
        stage.stripes(6.0f);
        const Image sharp = stage.render(plain());
        CHECK(contrastOf(sharp) == 1.0f);
        const uint32_t passesWithout = stage.statistics().screenPasses;
        CHECK(stage.statistics().depthOfFieldBytes == 0);

        // Everything in the picture is within the range of the focus.
        CHECK(mostDifference(stage.render(focusedAt(6.0f, 1.0f, 1.0f)), sharp) == 0);
        CHECK(stage.statistics().screenPasses == passesWithout + 3);
        CHECK(stage.statistics().depthOfFieldBytes == uint64_t{kSize / 2} * (kSize / 2) * 8 * 2);

        // A view that has none any more gives the images back.
        stage.render(plain());
        CHECK(stage.statistics().depthOfFieldBytes == 0);
        CHECK(stage.statistics().screenPasses == passesWithout);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what is out of focus is blurred: stripes lose their contrast, the more the wider the blur") {
        Stage stage;
        stage.stripes(6.0f);
        for (const PostProcessQuality quality :
             {PostProcessQuality::Low, PostProcessQuality::Medium, PostProcessQuality::High}) {
            CAPTURE(nameOf(quality));
            float before = contrastOf(stage.render(plain(), RenderDebugView::None, quality));
            for (const float farBlur : {0.25f, 0.5f, 1.0f}) {
                CAPTURE(farBlur);
                // Sharp two metres from the camera; the stripes are four further.
                const float contrast =
                    contrastOf(stage.render(focusedAt(2.0f, 0.0f, farBlur), RenderDebugView::None, quality));
                CHECK(contrast < before);
                before = contrast;
            }
            // Blurred over a circle about as wide as a stripe and the gap beside it, little
            // is left of them: a circle does not weigh both alike, and a little stays.
            CHECK(before < 1.0f / 3.0f);
            // In front of the focus just the same, by the near blur.
            CHECK(contrastOf(stage.render(focusedAt(7.5f, 1.0f, 0.0f), RenderDebugView::None, quality)) < 1.0f / 3.0f);
        }
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a flat field stays as bright as it was, however much it is blurred") {
        Stage stage;
        stage.ground("grey", 6.0f);
        const Image sharp = stage.render(plain());
        for (const float farBlur : {0.25f, 0.5f, 1.0f}) {
            CAPTURE(farBlur);
            CHECK(mostDifference(stage.render(focusedAt(2.0f, 0.0f, farBlur)), sharp) <= 1);
        }
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("something sharp in front of a blurred background keeps its edge, and leaves no glow around it") {
        Stage stage;
        stage.ground("black", 7.0f);
        stage.square("white", 2.0f);
        const Image sharp = stage.render(plain());
        const Image focused = stage.render(focusedAt(2.0f, 0.0f, 1.0f));
        // The square is as it was, every pixel of it, and the black around it stays black
        // up to its edge: the half-sized texels along the edge are made of both, and are
        // neither blurred over the background nor laid over it.
        uint32_t changed = 0;
        uint32_t glowing = 0;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const uint32_t outside = outsideSquare(x, y);
                if (outside == 0) {
                    changed += focused.pixel(x, y) == sharp.pixel(x, y) ? 0 : 1;
                } else {
                    glowing += focused.pixel(x, y).r <= 2.0f / 255.0f ? 0 : 1;
                }
            }
        }
        CHECK(changed == 0);
        CHECK(glowing == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what is behind something blurs it no more than twice as much as it is blurred itself") {
        // A square a quarter of the way to the most blur, sharp from 1.5 to 2.5 metres.
        const PostProcessSettings look = focusedAt(2.0f, 0.0f, 1.0f);
        CHECK(focusBlur(look, 2.75f) == 0.25f);
        // The ground behind it twice as blurred, and four times.
        CHECK(focusBlur(look, 3.0f) == 0.5f);
        CHECK(focusBlur(look, 7.0f) == 1.0f);
        Image squares[2];
        for (const float depth : {3.0f, 7.0f}) {
            Stage stage;
            stage.ground("black", depth);
            stage.square("white", 2.75f);
            squares[depth > 5.0f ? 1 : 0] = stage.render(look);
            checkNoValidationIssues(*stage.scene.context.graphics);
        }
        // Inside its edge - where the picture is made of the square alone - it is the same
        // in both.
        uint32_t changed = 0;
        for (uint32_t y = 50; y <= 77; ++y) {
            for (uint32_t x = 50; x <= 77; ++x) {
                changed += squares[0].pixel(x, y) == squares[1].pixel(x, y) ? 0 : 1;
            }
        }
        CHECK(changed == 0);
    }

    TEST_CASE("something blurred in front spreads over the sharp background behind it") {
        Stage stage;
        stage.ground("black", 6.0f);
        stage.square("white", 1.0f);
        const Image sharp = stage.render(plain());
        const Image focused = stage.render(focusedAt(6.0f, 1.0f, 0.0f));
        // Just outside the square its light reaches over the black, and the square gives
        // some of it up at its edge.
        CHECK(focused.pixel(46, 64).r > sharp.pixel(46, 64).r + 0.1f);
        CHECK(focused.pixel(48, 64).r < sharp.pixel(48, 64).r - 0.05f);
        // Further away than the blur reaches - almost four pixels, and a texel of the
        // half-sized images on top - the background is exactly as it was.
        uint32_t changed = 0;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                if (outsideSquare(x, y) > 6) {
                    changed += focused.pixel(x, y) == sharp.pixel(x, y) ? 0 : 1;
                }
            }
        }
        CHECK(changed == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("translucent surfaces are not in the depth buffer, and are blurred as much as what is behind them") {
        Stage stage;
        stage.ground("black", 7.0f);
        stage.square("glass", 2.0f);
        const Image sharp = stage.render(plain());
        // The glass is at the focus; the ground it is drawn over is not.
        const Image focused = stage.render(focusedAt(2.0f, 0.0f, 1.0f));
        CHECK(focused.pixel(47, 64).r > sharp.pixel(47, 64).r + 0.05f);
        CHECK(focused.pixel(48, 64).r < sharp.pixel(48, 64).r - 0.05f);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a picture that is not finished has no depth of field") {
        Stage stage;
        stage.stripes(6.0f);
        SceneRenderSettings unfinished = SceneFixture::flatSettings();
        unfinished.backgroundColor = Vec3(0.0f);
        unfinished.postProcess = focusedAt(2.0f, 0.0f, 1.0f);
        CHECK(contrastOf(stage.scene.render(stage.view, unfinished)) == 1.0f);
        CHECK(stage.statistics().depthOfFieldBytes == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("the debug view shows how blurred each pixel is: red in front of the focus, blue behind, black in it") {
        Stage stage;
        stage.ground("grey", 6.0f);
        // On the left a square at a metre, in the middle one at three.
        stage.scene.add(BuiltinMesh::Plane, stage.scene.material("white"),
                        at(Vec3(-1.25f, kEyeHeight - 1.0f, 0.0f), Vec3(0.5f, 1.0f, 0.5f)));
        stage.square("white", 3.0f);
        // Sharp from 2.5 to 3.5 metres: the near square is 1.5 in front of that, a metre
        // into the transition and beyond, and the ground the same behind it.
        const Image shown = stage.render(focusedAt(3.0f, 0.5f, 1.0f), RenderDebugView::Focus);
        checkShown(shown, 24, 64, linearToSrgb(Vec3(0.5f, 0.0f, 0.0f)));
        checkShown(shown, 64, 64, Vec3(0.0f));
        checkShown(shown, 110, 64, Vec3(0.0f, 0.0f, 1.0f));
        // Half way into the transition: half the blur.
        const Image half = stage.render(focusedAt(5.0f, 0.5f, 1.0f), RenderDebugView::Focus);
        checkShown(half, 110, 64, linearToSrgb(Vec3(0.0f, 0.0f, 0.5f)));

        // With no depth of field at all, everything is sharp - and the view says so.
        const Image none = stage.render(plain(), RenderDebugView::Focus);
        CHECK(std::ranges::all_of(none.pixels(),
                                  [](std::byte value) { return value == std::byte{0} || value == std::byte{255}; }));
        checkShown(none, 24, 64, Vec3(0.0f));
        checkShown(none, 110, 64, Vec3(0.0f));
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE(
        "blur goes by the depth along the way the camera looks, and where nothing was drawn is behind at the most") {
        Stage stage;
        stage.ground("grey", 6.0f);
        // With perspective the corners of the ground are further from the camera than its
        // middle, and exactly as deep.
        const CameraView perspective =
            stage.scene.perspectiveView(Vec3(0.0f, kEyeHeight, 0.0f), Vec3(0.0f), 60.0f, Vec3(0.0f, 0.0f, -1.0f));
        const Image shown = stage.scene.render(
            perspective, finishedWith(focusedAt(5.0f, 0.0f, 1.0f), RenderDebugView::Focus, PostProcessQuality::Medium));
        for (const UVec2 pixel : {UVec2(64, 64), UVec2(2, 2), UVec2(125, 2), UVec2(2, 125), UVec2(125, 125)}) {
            checkShown(shown, pixel.x, pixel.y, linearToSrgb(Vec3(0.0f, 0.0f, 0.5f)));
        }

        // Nothing drawn at all: as far as can be, with or without perspective.
        stage.scene.world.clear();
        for (const CameraView& view : {stage.view, perspective}) {
            const Image empty = stage.scene.render(
                view, finishedWith(focusedAt(5.0f, 0.0f, 0.75f), RenderDebugView::Focus, PostProcessQuality::Medium));
            checkShown(empty, 64, 64, linearToSrgb(Vec3(0.0f, 0.0f, 0.75f)));
            checkShown(empty, 1, 1, linearToSrgb(Vec3(0.0f, 0.0f, 0.75f)));
        }
        checkNoValidationIssues(*stage.scene.context.graphics);
    }
}
