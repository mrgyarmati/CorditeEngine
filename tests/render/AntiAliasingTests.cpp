#include "cordite/assets/Image.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/math/Color.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The picture is this many pixels a side, and shows this many metres.
constexpr uint32_t kPictureSize = 96;
constexpr float kPictureMetres = 3.0f;

/// Half the length and half the width of the bar the tests look at, in metres.
constexpr Vec2 kBarHalfSize(0.9f, 0.35f);

constexpr std::array<PostProcessQuality, 3> kFinishing = {PostProcessQuality::Low, PostProcessQuality::Medium,
                                                          PostProcessQuality::High};

/// A picture finished so that white is white and black is black - no curve, no bloom -
/// with its edges smoothed as `antiAliasing` says, as finely as `quality` does.
SceneRenderSettings finished(AntiAliasing antiAliasing, PostProcessQuality quality = PostProcessQuality::Medium) {
    PostProcessSettings look;
    look.toneMapper = ToneMapper::Linear;
    look.bloomIntensity = 0.0f;
    SceneRenderSettings settings;
    settings.backgroundColor = Vec3(0.0f);
    settings.screenEffectsQuality = ScreenEffectsQuality::Off;
    settings.postProcessQuality = quality;
    settings.antiAliasing = antiAliasing;
    settings.postProcess = look;
    return settings;
}

/// A bar that gives off light on a black background, turned about the line of sight
/// and seen without perspective.
struct Bar {
    explicit Bar(float degrees = 0.0f, const Vec3& light = Vec3(1.0f)) : scene(UVec2(kPictureSize)) {
        scene.writeFlatMaterial("bar", light);
        scene.open();
        entity = scene.add(BuiltinMesh::Cube, scene.material("bar"),
                           at(Vec3(0.0f), Vec3(kBarHalfSize.x * 2.0f, kBarHalfSize.y * 2.0f, 0.2f)));
        turn(degrees);
    }

    /// Turns the bar counter-clockwise, as the picture shows it, by `degrees` from level.
    void turn(float degrees) {
        angle = toRadians(degrees);
        scene.world.transform(entity).rotation = glm::angleAxis(angle, Vec3(0.0f, 0.0f, 1.0f));
    }

    /// The picture, seen with `up` at its top.
    Image render(const SceneRenderSettings& settings, const Vec3& up = kWorldUp) {
        return scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), kPictureMetres, up), settings);
    }

    /// How much of the pixel at (x, y) the bar covers, seen with +Y at the top: what a
    /// much finer picture of a white bar, scaled down, would show there.
    [[nodiscard]] float coverage(uint32_t x, uint32_t y) const {
        constexpr int kSamples = 16;
        int inside = 0;
        for (int row = 0; row < kSamples; ++row) {
            for (int column = 0; column < kSamples; ++column) {
                const Vec2 pixel(static_cast<float>(x) + (static_cast<float>(column) + 0.5f) / kSamples,
                                 static_cast<float>(y) + (static_cast<float>(row) + 0.5f) / kSamples);
                const Vec2 place((pixel.x / kPictureSize - 0.5f) * kPictureMetres,
                                 (0.5f - pixel.y / kPictureSize) * kPictureMetres);
                // Into the bar's own directions.
                const Vec2 own(place.x * std::cos(angle) + place.y * std::sin(angle),
                               -place.x * std::sin(angle) + place.y * std::cos(angle));
                inside += std::abs(own.x) <= kBarHalfSize.x && std::abs(own.y) <= kBarHalfSize.y ? 1 : 0;
            }
        }
        return static_cast<float>(inside) / (kSamples * kSamples);
    }

    /// How far a picture of the white bar is from what a much finer one shows: the
    /// brightness of each pixel against the part of it the bar covers, added up.
    [[nodiscard]] float errorOf(const Image& image) const {
        float total = 0.0f;
        for (uint32_t y = 0; y < kPictureSize; ++y) {
            for (uint32_t x = 0; x < kPictureSize; ++x) {
                total += std::abs(image.pixel(x, y).g - coverage(x, y));
            }
        }
        return total;
    }

    SceneFixture scene;
    Entity entity;
    float angle = 0.0f;
};

/// How many pixels of two pictures differ by more than `tolerance` levels of 255.
uint64_t differingPixels(const Image& first, const Image& second, uint8_t tolerance = 0) {
    return unwrap(compareImages(first, second, {.channelTolerance = tolerance})).differingPixels;
}

/// The picture `image` turned by half a turn, and by a quarter turn counter-clockwise.
Image turnedHalf(const Image& image) {
    Image turned = unwrap(Image::create(image.width(), image.height(), ImageFormat::Rgba8));
    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            turned.setPixel(image.width() - 1 - x, image.height() - 1 - y, image.pixel(x, y));
        }
    }
    return turned;
}

Image turnedQuarter(const Image& image) {
    Image turned = unwrap(Image::create(image.height(), image.width(), ImageFormat::Rgba8));
    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            turned.setPixel(y, image.width() - 1 - x, image.pixel(x, y));
        }
    }
    return turned;
}

} // namespace

TEST_SUITE("AntiAliasing") {
    TEST_CASE("a picture without edges is left exactly as it is") {
        // An even grey, which the last pass breaks up with a little noise so that it
        // does not band: none of that is an edge.
        Bar bar(0.0f, Vec3(0.2f));
        bar.scene.world.transform(bar.entity).scale = Vec3(40.0f, 40.0f, 0.2f);
        for (const PostProcessQuality quality : kFinishing) {
            CAPTURE(nameOf(quality));
            const Image plain = bar.render(finished(AntiAliasing::Off, quality));
            const Image smoothed = bar.render(finished(AntiAliasing::Fxaa, quality));
            CHECK(differingPixels(plain, smoothed) == 0);
            // It is the grey it was told to be.
            CHECK(smoothed.pixel(40, 50).g == doctest::Approx(linearToSrgb(0.2f)).epsilon(0.01));
        }

        // Nor is a step too small to see: two greys a hundredth of white apart.
        bar.scene.world.transform(bar.entity).scale = Vec3(kBarHalfSize.x * 2.0f, kBarHalfSize.y * 2.0f, 0.2f);
        bar.turn(20.0f);
        SceneRenderSettings settings = finished(AntiAliasing::Off);
        settings.backgroundColor = srgbToLinear(Vec3(linearToSrgb(0.2f) - 0.01f));
        const Image plain = bar.render(settings);
        settings.antiAliasing = AntiAliasing::Fxaa;
        CHECK(differingPixels(plain, bar.render(settings)) == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("slanted edges come closer to what a finer picture shows") {
        // A picture drawn much finer and scaled down has, in every pixel on the edge of
        // the bar, as much white as the bar covers of the pixel. Drawn as it is, such a
        // pixel is white or black; smoothed, it is to be nearer the truth.
        Bar bar;
        for (const float degrees : {3.0f, 8.0f, 20.0f, 33.0f, 58.0f, 75.0f, 86.0f}) {
            bar.turn(degrees);
            const float aliased = bar.errorOf(bar.render(finished(AntiAliasing::Off)));
            // The bar has some 250 pixels of edge, each wrong by a quarter on average.
            CHECK(aliased > 30.0f);
            for (const PostProcessQuality quality : kFinishing) {
                CAPTURE(degrees);
                CAPTURE(nameOf(quality));
                const float smoothed = bar.errorOf(bar.render(finished(AntiAliasing::Fxaa, quality)));
                // A third of what is wrong goes at the least, where the steps are long;
                // nearly half of it where they are a few pixels.
                CHECK(smoothed < aliased * 0.75f);
                if (degrees > 5.0f && degrees < 85.0f) {
                    CHECK(smoothed < aliased * 0.55f);
                }
            }
        }

        // At exactly 45 degrees every step is one pixel long, and a pixel is in the
        // middle of its step: there is little for the edge to say, and the picture is
        // no worse for it.
        bar.turn(45.0f);
        const float aliased = bar.errorOf(bar.render(finished(AntiAliasing::Off)));
        CHECK(bar.errorOf(bar.render(finished(AntiAliasing::Fxaa))) < aliased);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("the higher the quality, the further an edge is followed to where its step ends") {
        // Steps of some twenty and thirty pixels: the longer they are, the more of them
        // a search that gives up early leaves as they were.
        for (const float degrees : {3.0f, 2.0f, 87.0f}) {
            CAPTURE(degrees);
            Bar bar(degrees);
            const float low = bar.errorOf(bar.render(finished(AntiAliasing::Fxaa, PostProcessQuality::Low)));
            const float medium = bar.errorOf(bar.render(finished(AntiAliasing::Fxaa, PostProcessQuality::Medium)));
            const float high = bar.errorOf(bar.render(finished(AntiAliasing::Fxaa, PostProcessQuality::High)));
            CHECK(medium < low);
            CHECK(high <= medium);
            checkNoValidationIssues(*bar.scene.context.graphics);
        }
    }

    TEST_CASE("an edge along the pixels stays where it is, nearly as sharp as it was") {
        // A bar that lies level: its long edges run along rows of pixels, and there is
        // no step to smooth. Far from its ends, all that is done to them is the little
        // softening every pixel on an edge gets. Nearer to an end - within the reach of
        // the search - the end is taken for a step, and the edge is blended a little
        // more, the more the nearer the end is: that is the price of finding steps by
        // looking. It never moves the edge: the brighter side stays the brighter.
        Bar bar(0.0f);
        const Image plain = bar.render(finished(AntiAliasing::Off));
        const Image smoothed = bar.render(finished(AntiAliasing::Fxaa));
        // The upper edge is between two rows; find them in the middle column.
        uint32_t firstWhite = 0;
        while (firstWhite < kPictureSize && plain.pixel(48, firstWhite).g < 0.5f) {
            ++firstWhite;
        }
        REQUIRE(firstWhite > 2);
        REQUIRE(firstWhite < kPictureSize / 2);
        for (uint32_t x = 24; x < 72; x += 2) {
            CAPTURE(x);
            const bool isFarFromTheEnds = x >= 44 && x <= 52;
            CHECK(smoothed.pixel(x, firstWhite).g > (isFarFromTheEnds ? 0.93f : 0.6f));
            CHECK(smoothed.pixel(x, firstWhite - 1).g < (isFarFromTheEnds ? 0.07f : 0.4f));
            // A pixel further in or out is not touched at all.
            CHECK(smoothed.pixel(x, firstWhite + 2).g == doctest::Approx(1.0f).epsilon(0.01));
            CHECK(smoothed.pixel(x, firstWhite - 3).g == doctest::Approx(0.0f).epsilon(0.01));
        }
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("it does the same whichever way round a picture is") {
        // The same bar seen with the camera upside down, and on its side, is the same
        // picture turned - before the smoothing and after it. A smoothing that took up
        // for down, or left for right, would not pass.
        //
        // On its side a picture has its rows where its columns were, and a pixel that is
        // as much on an edge along the rows as on one along the columns - at a corner -
        // is taken to be on the one along the rows: a few such pixels may come out
        // differently. Upside down, rows are rows, and none may.
        constexpr uint64_t kCornerPixels = 4;
        SceneRenderSettings settings = finished(AntiAliasing::Fxaa, PostProcessQuality::High);
        {
            // Off the middle of the picture: the picture is not its own mirror image.
            Bar bar(17.0f);
            bar.scene.world.transform(bar.entity).position = Vec3(0.35f, -0.2f, 0.0f);
            const Image upright = bar.render(settings);
            const Image upsideDown = bar.render(settings, Vec3(0.0f, -1.0f, 0.0f));
            const Image onItsSide = bar.render(settings, Vec3(1.0f, 0.0f, 0.0f));
            // Two levels: the noise that keeps gradients from banding is not turned
            // with the picture.
            CHECK(differingPixels(turnedHalf(upright), upsideDown, 2) == 0);
            CHECK(differingPixels(turnedQuarter(upright), onItsSide, 2) <= kCornerPixels);
            // And there is something to turn: upside down it is another picture.
            CHECK(differingPixels(upright, upsideDown, 2) > 100);
            CHECK(differingPixels(upright, onItsSide, 2) > 100);
            checkNoValidationIssues(*bar.scene.context.graphics);
        }

        // A bar of another color on another ground: darker on lighter.
        Bar dark(-28.0f, Vec3(0.05f, 0.1f, 0.4f));
        dark.scene.world.transform(dark.entity).position = Vec3(-0.3f, 0.25f, 0.0f);
        settings.backgroundColor = Vec3(0.8f, 0.7f, 0.5f);
        const Image upright = dark.render(settings);
        CHECK(differingPixels(turnedHalf(upright), dark.render(settings, Vec3(0.0f, -1.0f, 0.0f)), 2) == 0);
        CHECK(differingPixels(turnedQuarter(upright), dark.render(settings, Vec3(1.0f, 0.0f, 0.0f)), 2) <=
              kCornerPixels);
        checkNoValidationIssues(*dark.scene.context.graphics);
    }

    TEST_CASE("a picture that is not finished, and a debug view, are not smoothed") {
        Bar bar(20.0f);
        SceneRenderSettings settings = finished(AntiAliasing::Off, PostProcessQuality::Off);
        const Image plain = bar.render(settings);
        settings.antiAliasing = AntiAliasing::Fxaa;
        CHECK(differingPixels(plain, bar.render(settings)) == 0);
        CHECK(bar.scene.targets.aliased() == nullptr);

        settings = finished(AntiAliasing::Off);
        settings.debugView = RenderDebugView::Emissive;
        const Image view = bar.render(settings);
        settings.antiAliasing = AntiAliasing::Fxaa;
        CHECK(differingPixels(view, bar.render(settings)) == 0);
        CHECK(bar.scene.targets.aliased() == nullptr);
        // A pixel on the edge of the bar is white or black there, and nothing between.
        uint32_t between = 0;
        for (uint32_t y = 0; y < kPictureSize; ++y) {
            for (uint32_t x = 0; x < kPictureSize; ++x) {
                const float value = view.pixel(x, y).g;
                between += value > 0.05f && value < 0.95f ? 1 : 0;
            }
        }
        CHECK(between == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("the image a picture waits in to be smoothed belongs to the view, for as long as it is needed") {
        Bar bar(20.0f);
        CHECK(bar.scene.targets.aliased() == nullptr);
        CHECK(bar.scene.targets.aliasedBytes() == 0);

        bar.render(finished(AntiAliasing::Fxaa));
        REQUIRE(bar.scene.targets.aliased() != nullptr);
        CHECK(bar.scene.targets.aliased()->getDesc().width == kPictureSize);
        CHECK(bar.scene.targets.aliased()->getDesc().height == kPictureSize);
        CHECK(bar.scene.targets.aliasedBytes() == uint64_t{kPictureSize} * kPictureSize * 4);
        // The smoothing is one pass more: the lighting, the final pass, and it.
        CHECK(bar.scene.renderer->statistics().screenPasses == 3);
        // Kept from frame to frame.
        nvrhi::ITexture* kept = bar.scene.targets.aliased();
        bar.render(finished(AntiAliasing::Fxaa, PostProcessQuality::High));
        CHECK(bar.scene.targets.aliased() == kept);

        // A view that is no longer smoothed gives it back, and is drawn as it was before.
        const Image plain = bar.render(finished(AntiAliasing::Off));
        CHECK(bar.scene.targets.aliased() == nullptr);
        CHECK(bar.scene.targets.aliasedBytes() == 0);
        CHECK(bar.scene.renderer->statistics().screenPasses == 2);
        // And gets one again when it is.
        const Image smoothed = bar.render(finished(AntiAliasing::Fxaa));
        CHECK(bar.scene.targets.aliased() != nullptr);
        CHECK(differingPixels(plain, smoothed, 8) > 50);

        // Targets of another size have an image of their own size.
        SceneTargets wide = unwrap(SceneTargets::create(bar.scene.context.device(), UVec2(160, 48), "Wide"));
        test::recordAndExecute(bar.scene.context, [&](nvrhi::ICommandList* commandList) {
            bar.scene.renderer->render(commandList, bar.scene.world,
                                       bar.scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), kPictureMetres),
                                       wide, finished(AntiAliasing::Fxaa));
        });
        REQUIRE(wide.aliased() != nullptr);
        CHECK(wide.aliased()->getDesc().width == 160);
        CHECK(wide.aliased()->getDesc().height == 48);
        CHECK(wide.aliasedBytes() == 160 * 48 * 4);

        // Asked for by hand: made once, kept, and given back; without a device, or for
        // targets that are none, there is nothing to make it with.
        wide.releaseAliased();
        CHECK(wide.aliased() == nullptr);
        CHECK(wide.aliasedFramebuffer() == nullptr);
        REQUIRE(wide.prepareAliased(bar.scene.context.device()).has_value());
        nvrhi::ITexture* made = wide.aliased();
        REQUIRE(made != nullptr);
        CHECK(wide.aliasedFramebuffer() != nullptr);
        REQUIRE(wide.prepareAliased(bar.scene.context.device()).has_value());
        CHECK(wide.aliased() == made);
        SceneTargets none;
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)wide.prepareAliased(nullptr), test::AssertionFailure);
        CHECK_THROWS_AS((void)none.prepareAliased(bar.scene.context.device()), test::AssertionFailure);
#else
        CHECK_FALSE(wide.prepareAliased(nullptr).has_value());
        CHECK_FALSE(none.prepareAliased(bar.scene.context.device()).has_value());
#endif
        CHECK(wide.aliased() == made);
        CHECK(none.aliased() == nullptr);
        CHECK(none.aliasedBytes() == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("film grain is laid over the smoothed picture, and is not smoothed away with it") {
        Bar bar(20.0f, Vec3(0.5f));
        SceneRenderSettings settings = finished(AntiAliasing::Off);
        settings.backgroundColor = Vec3(0.5f);
        settings.postProcess->filmGrain = 1.0f;
        const Image plain = bar.render(settings);
        settings.antiAliasing = AntiAliasing::Fxaa;
        const Image smoothed = bar.render(settings);

        // How much the pixels of an even grey differ from one another.
        const auto spreadOf = [](const Image& image) {
            double sum = 0.0;
            double squares = 0.0;
            const double count = double{kPictureSize} * kPictureSize;
            for (uint32_t y = 0; y < kPictureSize; ++y) {
                for (uint32_t x = 0; x < kPictureSize; ++x) {
                    const double value = image.pixel(x, y).g;
                    sum += value;
                    squares += value * value;
                }
            }
            return std::sqrt(std::max(squares / count - (sum / count) * (sum / count), 0.0));
        };
        // There is grain, and as much of it smoothed as not: grain that went through the
        // smoothing would have been taken for edges, and blurred.
        CHECK(spreadOf(plain) > 0.015);
        CHECK(spreadOf(smoothed) == doctest::Approx(spreadOf(plain)).epsilon(0.03));
        // The grains are where they were, give or take the rounding to 256 levels.
        CHECK(differingPixels(plain, smoothed, 2) == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a thing one pixel wide is softened, not lost") {
        // A line a pixel wide, a little off level: what is finer than a pixel cannot be
        // mended - it stands out from all around it, and is blended with that - but it
        // must not vanish either.
        Bar bar(4.0f);
        bar.scene.world.transform(bar.entity).scale = Vec3(2.0f, kPictureMetres / kPictureSize, 0.2f);
        const Image plain = bar.render(finished(AntiAliasing::Off));
        const Image smoothed = bar.render(finished(AntiAliasing::Fxaa, PostProcessQuality::High));
        const auto lightOf = [](const Image& image) {
            float total = 0.0f;
            for (uint32_t y = 0; y < kPictureSize; ++y) {
                for (uint32_t x = 0; x < kPictureSize; ++x) {
                    total += image.pixel(x, y).g;
                }
            }
            return total;
        };
        // About a third of its light goes; no more than that, and none is made up.
        CHECK(lightOf(plain) > 40.0f);
        CHECK(lightOf(smoothed) > lightOf(plain) * 0.55f);
        CHECK(lightOf(smoothed) < lightOf(plain));
        // In every column the line crosses, there is still a line to see.
        for (uint32_t x = 24; x < 72; x += 6) {
            CAPTURE(x);
            float brightest = 0.0f;
            for (uint32_t y = 0; y < kPictureSize; ++y) {
                brightest = std::max(brightest, smoothed.pixel(x, y).g);
            }
            CHECK(brightest > 0.3f);
        }
        checkNoValidationIssues(*bar.scene.context.graphics);
    }
}
