#include "cordite/assets/Image.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/core/Assert.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/PostProcessPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SceneTargets.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkShown;
using cordite::test::LogCapture;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// Settings that leave the light as it is - no curve, no bloom - for a test to change
/// one thing in.
PostProcessSettings plain() {
    PostProcessSettings look;
    look.toneMapper = ToneMapper::Linear;
    look.bloomIntensity = 0.0f;
    return look;
}

/// A black background, and the picture finished with `look` whatever the world says -
/// with its edges left as they are: these tests say what a pixel must be, and what
/// smoothing them does is tested on its own (AntiAliasingTests.cpp).
SceneRenderSettings finishedWith(const PostProcessSettings& look) {
    SceneRenderSettings settings;
    settings.backgroundColor = Vec3(0.0f);
    settings.antiAliasing = AntiAliasing::Off;
    settings.postProcess = look;
    return settings;
}

/// What finishing makes of the light at one place of a picture, as PostProcessSettings.h
/// has it, leaving out what needs the picture around the place: exposure, white balance,
/// vignette, the tone mapper, grading, and the encoding of a display.
Vec3 finished(const PostProcessSettings& look, const Vec3& light, const Vec2& fromCentre = Vec2(0.0f)) {
    const Vec3 exposed = light * exposureScale(look.exposure) * whiteBalanceScale(look.temperature, look.tint) *
                         vignetteScale(look.vignette, fromCentre);
    return linearToSrgb(applyGrading(look, applyToneMapper(look.toneMapper, exposed)));
}

/// Where a pixel is in its picture, from -1 to 1 both ways.
Vec2 fromCentreOf(uint32_t x, uint32_t y, UVec2 size) {
    return (Vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) / Vec2(size)) * 2.0f - 1.0f;
}

/// Eight columns of flat color that fill a picture of 64 by 64 pixels, each eight pixels
/// wide and giving off the light it is told to, seen from straight above.
struct Swatches {
    explicit Swatches(const std::vector<Vec3>& lights) : lights(lights) {
        REQUIRE(lights.size() == 8);
        for (size_t column = 0; column < lights.size(); ++column) {
            scene.writeFlatMaterial(fmt::format("swatch{}", column), lights[column]);
        }
        scene.open();
        for (size_t column = 0; column < lights.size(); ++column) {
            scene.add(BuiltinMesh::Plane, scene.material(fmt::format("swatch{}", column)),
                      at(Vec3(-1.75f + 0.5f * static_cast<float>(column), 0.0f, 0.0f), Vec3(0.5f, 1.0f, 4.0f)));
        }
        view = scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
    }

    Image render(const PostProcessSettings& look) { return scene.render(view, finishedWith(look)); }

    /// A pixel in the middle of a column, in the middle row of the picture.
    [[nodiscard]] static uint32_t middleOf(size_t column) { return static_cast<uint32_t>(column) * 8 + 4; }

    /// Checks the middle of every column against what `look` makes of its light.
    void check(const Image& image, const PostProcessSettings& look) const {
        for (size_t column = 0; column < lights.size(); ++column) {
            CAPTURE(column);
            const uint32_t x = middleOf(column);
            checkShown(image, x, 32, finished(look, lights[column], fromCentreOf(x, 32, UVec2(64))));
        }
    }

    SceneFixture scene{UVec2(64, 64)};
    std::vector<Vec3> lights;
    CameraView view;
};

/// The qualities at which a picture is finished.
constexpr std::array<PostProcessQuality, 3> kFinishing = {PostProcessQuality::Low, PostProcessQuality::Medium,
                                                          PostProcessQuality::High};

/// Greys from deep shadow to four times white.
std::vector<Vec3> greys() {
    return {Vec3(0.01f), Vec3(0.05f), Vec3(0.18f), Vec3(0.5f), Vec3(1.0f), Vec3(2.0f), Vec3(4.0f), Vec3(8.0f)};
}

/// A plane of one flat color that fills the picture, and a way to change the light it
/// gives off between frames: a second plane over it that can be put away.
struct Field {
    explicit Field(const Vec3& light, UVec2 size = UVec2(64, 64), const Vec3& otherLight = Vec3(0.0f)) : scene(size) {
        scene.writeFlatMaterial("field", light);
        scene.writeFlatMaterial("other", otherLight);
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("field"), at(Vec3(0.0f), Vec3(40.0f)));
        other = scene.add(BuiltinMesh::Plane, scene.material("other"), at(Vec3(0.0f, -1.0f, 0.0f), Vec3(40.0f)));
        view = scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
    }

    /// Shows the other light instead of the first, or the first again.
    void showOther(bool shown) { scene.world.transform(other).position.y = shown ? 1.0f : -1.0f; }

    Image render(const SceneRenderSettings& settings) { return scene.render(view, settings); }

    Image render(const PostProcessSettings& look) { return scene.render(view, finishedWith(look)); }

    SceneFixture scene;
    Entity other;
    CameraView view;
};

/// The linear brightness of a pixel of a picture that is grey there.
float brightnessAt(const Image& image, uint32_t x, uint32_t y) {
    return srgbToLinear(image.pixel(x, y).g);
}

/// A lookup table of `size` slices, each `size` pixels a side, side by side: the color
/// at each place is what `grade` makes of the color the place stands for. Colors are
/// those of a display, 0 to 1.
Image makeLut(uint32_t size, const std::function<Vec3(const Vec3&)>& grade) {
    Image image = unwrap(Image::create(size * size, size, ImageFormat::Rgba8));
    const auto last = static_cast<float>(size - 1);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size * size; ++x) {
            const uint32_t slice = x / size;
            const Vec3 color(static_cast<float>(x % size) / last, static_cast<float>(y) / last,
                             static_cast<float>(slice) / last);
            image.setPixel(x, y, Vec4(glm::clamp(grade(color), Vec3(0.0f), Vec3(1.0f)), 1.0f));
        }
    }
    return image;
}

} // namespace

TEST_SUITE("PostProcess") {
    TEST_CASE("each tone mapper shows light as its curve says") {
        Swatches swatches(greys());
        for (const ToneMapper toneMapper : kToneMappers) {
            CAPTURE(nameOf(toneMapper));
            PostProcessSettings look = plain();
            look.toneMapper = toneMapper;
            swatches.check(swatches.render(look), look);
        }
        // The curves are not one another: white is three different greys.
        PostProcessSettings look = plain();
        look.toneMapper = ToneMapper::Filmic;
        const float filmic = swatches.render(look).pixel(Swatches::middleOf(4), 32).g;
        look.toneMapper = ToneMapper::Aces;
        const float aces = swatches.render(look).pixel(Swatches::middleOf(4), 32).g;
        look.toneMapper = ToneMapper::Linear;
        const float linear = swatches.render(look).pixel(Swatches::middleOf(4), 32).g;
        CHECK(filmic < aces - 0.03f);
        CHECK(aces < linear - 0.03f);
        checkNoValidationIssues(*swatches.scene.context.graphics);
    }

    TEST_CASE("colors go through the curves one by one, and through grading as the settings say") {
        Swatches swatches({Vec3(0.5f, 0.25f, 0.1f), Vec3(0.1f, 0.6f, 0.9f), Vec3(2.0f, 0.2f, 0.05f),
                           Vec3(0.02f, 0.3f, 0.02f), Vec3(0.18f), Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 3.0f),
                           Vec3(0.7f, 0.7f, 0.75f)});

        std::vector<PostProcessSettings> looks(9, plain());
        looks[0].toneMapper = ToneMapper::Filmic;
        looks[1].toneMapper = ToneMapper::Aces;
        looks[2].saturation = 0.3f;
        looks[3].saturation = 1.6f;
        looks[4].contrast = 1.5f;
        looks[5].contrast = 0.6f;
        looks[6].lift = Vec3(0.05f, 0.0f, 0.1f);
        looks[6].gamma = Vec3(1.4f, 1.0f, 0.8f);
        looks[6].gain = Vec3(0.9f, 1.1f, 1.0f);
        looks[7].temperature = 0.7f;
        looks[7].tint = -0.4f;
        // All of it at once, over a curve.
        looks[8].toneMapper = ToneMapper::Filmic;
        looks[8].exposure = 0.8f;
        looks[8].temperature = -0.5f;
        looks[8].saturation = 0.8f;
        looks[8].contrast = 1.2f;
        looks[8].lift = Vec3(0.02f);
        looks[8].gamma = Vec3(1.1f);
        looks[8].gain = Vec3(0.95f, 1.0f, 1.05f);
        for (size_t index = 0; index < looks.size(); ++index) {
            CAPTURE(index);
            swatches.check(swatches.render(looks[index]), looks[index]);
        }
        checkNoValidationIssues(*swatches.scene.context.graphics);
    }

    TEST_CASE("a stop of exposure doubles the light before the curve") {
        Swatches swatches(greys());
        for (const float stops : {-2.0f, -1.0f, 0.0f, 1.0f, 2.5f}) {
            CAPTURE(stops);
            PostProcessSettings look = plain();
            look.exposure = stops;
            swatches.check(swatches.render(look), look);
            look.toneMapper = ToneMapper::Filmic;
            swatches.check(swatches.render(look), look);
        }
        // One stop up: the grey of 0.18 is now where 0.36 was.
        PostProcessSettings look = plain();
        look.exposure = 1.0f;
        CHECK(brightnessAt(swatches.render(look), Swatches::middleOf(2), 32) == doctest::Approx(0.36f).epsilon(0.03));
        checkNoValidationIssues(*swatches.scene.context.graphics);
    }

    TEST_CASE("a vignette darkens the picture towards its corners") {
        Field field(Vec3(0.6f));
        PostProcessSettings look = plain();
        look.vignette = 0.7f;
        const Image image = field.render(look);
        for (const UVec2 pixel : {UVec2(32, 32), UVec2(0, 0), UVec2(63, 0), UVec2(0, 63), UVec2(63, 63), UVec2(32, 4),
                                  UVec2(50, 40), UVec2(10, 30)}) {
            CAPTURE(pixel);
            checkShown(image, pixel.x, pixel.y, finished(look, Vec3(0.6f), fromCentreOf(pixel.x, pixel.y, UVec2(64))));
        }
        // The middle is as it was; a corner has lost nearly seven tenths of its light.
        CHECK(brightnessAt(image, 32, 32) == doctest::Approx(0.6f).epsilon(0.02));
        CHECK(brightnessAt(image, 0, 0) == doctest::Approx(0.6f * (1.0f - 0.7f * 0.9692f)).epsilon(0.04));
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("chromatic aberration parts red and blue towards the corners, and nothing in the middle") {
        // White to the left of the middle of each quarter of the picture, black to the
        // right: an edge that runs from top to bottom, 128 pixels right of the middle
        // of a picture 512 pixels wide.
        constexpr uint32_t kSize = 512;
        SceneFixture scene{UVec2(kSize, kSize)};
        scene.writeFlatMaterial("white", Vec3(1.0f));
        scene.open();
        // Four metres across: the plane ends one metre right of the middle.
        scene.add(BuiltinMesh::Plane, scene.material("white"), at(Vec3(-1.0f, 0.0f, 0.0f), Vec3(4.0f, 1.0f, 4.0f)));
        const CameraView view =
            scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));

        PostProcessSettings look = plain();
        look.chromaticAberration = 1.0f;
        const Image image = scene.render(view, finishedWith(look));
        const Image sharp = scene.render(view, finishedWith(plain()));

        // What the scene holds at a place across the picture, in pixels: white up to the
        // edge at 384, black from there, and between two pixel centres a blend.
        const auto sceneAt = [](float x) { return std::clamp(384.0f - x, 0.0f, 1.0f); };
        for (const uint32_t y : {8u, 128u, 256u, 400u, 500u}) {
            for (uint32_t x = 376; x < 392; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                // Red is read further out and blue further in, by a hundredth of the
                // picture's size between them in its very corners.
                const Vec2 fromCentre = fromCentreOf(x, y, UVec2(kSize));
                const float shift = fromCentre.x * glm::dot(fromCentre, fromCentre) * 0.25f * 0.01f * kSize;
                const float centre = static_cast<float>(x);
                // (Compared as light: a GPU places a read to a 256th of a texel, which
                // in the dark end of a display's colors is several levels.)
                const Vec3 expected(sceneAt(centre + shift), sceneAt(centre), sceneAt(centre - shift));
                CHECK_NEAR(srgbToLinear(Vec3(image.pixel(x, y))), expected, 0.02f);
                // Without it, the edge is one pixel sharp and has no color.
                const Vec4 plainPixel = sharp.pixel(x, y);
                CHECK(plainPixel.r == plainPixel.b);
            }
        }
        // At the top of the edge, blue has not reached black yet where red already has:
        // a fringe of blue... and at the height of the middle, hardly any.
        const Vec4 fringe = image.pixel(383, 8);
        CHECK(fringe.b - fringe.r > 0.2f);
        const Vec4 middle = image.pixel(383, 256);
        CHECK(std::abs(middle.b - middle.r) < 0.1f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("film grain differs from pixel to pixel and from one moment to the next") {
        Field field(Vec3(0.3f));
        PostProcessSettings look = plain();
        look.filmGrain = 1.0f;
        SceneRenderSettings settings = finishedWith(look);
        settings.timeSeconds = 2.0;
        const Image first = field.render(settings);
        const Image again = field.render(settings);
        settings.timeSeconds = 2.5;
        const Image later = field.render(settings);

        // Around what the picture would be without it, by a twentieth of the range at most.
        const float expected = finished(look, Vec3(0.3f)).g;
        double sum = 0.0;
        float lowest = 1.0f;
        float highest = 0.0f;
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                const float value = first.pixel(x, y).g;
                sum += value;
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
        }
        CHECK(sum / 4096.0 == doctest::Approx(expected).epsilon(0.01));
        CHECK(highest - lowest > 0.05f);
        CHECK(highest < expected + 0.06f);
        CHECK(lowest > expected - 0.06f);

        // The same moment has the same grain; another moment has other grain.
        CHECK(unwrap(compareImages(first, again, {.channelTolerance = 0})).differingPixels == 0);
        CHECK(unwrap(compareImages(first, later, {.channelTolerance = 2})).differingPixels > 2000);
        // Grain is grey: it changes all three colors alike.
        CHECK(first.pixel(10, 10).r == first.pixel(10, 10).b);

        // At a low quality there is none.
        settings.postProcessQuality = PostProcessQuality::Low;
        const Image cheap = field.render(settings);
        checkShown(cheap, 20, 20, Vec3(expected));
        checkShown(cheap, 40, 7, Vec3(expected));
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("the finished picture is dithered by a level at the most, the same way every time") {
        Field field(Vec3(0.2f));
        // A picture that is not finished is not dithered: every pixel is the same, the
        // level of a display nearest to the grey.
        SceneRenderSettings unfinished;
        unfinished.postProcessQuality = PostProcessQuality::Off;
        const Image flat = field.render(unfinished);
        CHECK(flat.pixel(3, 3) == flat.pixel(40, 21));
        CHECK(flat.pixel(3, 3).g == doctest::Approx(linearToSrgb(0.2f)).epsilon(0.01));
        const long nearest = std::lround(flat.pixel(3, 3).g * 255.0f);

        // Finished, with nothing else done to it, the same grey is that level or one of
        // its two neighbours - and not the same everywhere, so that a slow gradient
        // does not show bands.
        const PostProcessSettings look = plain();
        const Image image = field.render(look);
        std::set<long> levels;
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                levels.insert(std::lround(image.pixel(x, y).g * 255.0f));
            }
        }
        CHECK(levels.size() >= 2);
        CHECK(levels.size() <= 3);
        CHECK(*levels.begin() >= nearest - 1);
        CHECK(*levels.rbegin() <= nearest + 1);
        CHECK(unwrap(compareImages(image, field.render(look), {.channelTolerance = 0})).differingPixels == 0);
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("black stays black and white stays white, however the picture is finished") {
        Swatches swatches(
            {Vec3(0.0f), Vec3(1.0f), Vec3(0.0f), Vec3(50.0f), Vec3(0.0f), Vec3(1.0f), Vec3(0.0f), Vec3(1.0f)});
        for (const ToneMapper toneMapper : kToneMappers) {
            CAPTURE(nameOf(toneMapper));
            PostProcessSettings look = plain();
            look.toneMapper = toneMapper;
            look.exposure = 4.0f;
            const Image image = swatches.render(look);
            for (uint32_t y = 0; y < 64; ++y) {
                for (const size_t column : {size_t{0}, size_t{2}, size_t{4}, size_t{6}}) {
                    CHECK(image.pixel(Swatches::middleOf(column), y) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
                }
                // Sixteen times white is white under every curve.
                CHECK(image.pixel(Swatches::middleOf(1), y) == Vec4(1.0f));
                CHECK(image.pixel(Swatches::middleOf(3), y) == Vec4(1.0f));
            }
        }
        checkNoValidationIssues(*swatches.scene.context.graphics);
    }

    TEST_CASE("pictures that are not finished, and debug views, show the light as it is") {
        Swatches swatches(greys());
        PostProcessSettings look;
        look.exposure = 3.0f;
        look.saturation = 0.0f;
        look.vignette = 1.0f;

        // Not finished: the settings do nothing, and what is brighter than white clips.
        SceneRenderSettings settings = finishedWith(look);
        settings.postProcessQuality = PostProcessQuality::Off;
        Image image = swatches.scene.render(swatches.view, settings);
        for (size_t column = 0; column < 8; ++column) {
            CAPTURE(column);
            test::checkPixel(image, Swatches::middleOf(column), 32, swatches.lights[column]);
        }
        test::checkPixel(image, 2, 2, swatches.lights[0]);

        // A debug view shows numbers, whatever the picture would be finished with.
        settings.postProcessQuality = PostProcessQuality::High;
        settings.debugView = RenderDebugView::Emissive;
        image = swatches.scene.render(swatches.view, settings);
        test::checkPixel(image, Swatches::middleOf(3), 32, swatches.lights[3]);
        test::checkPixel(image, 2, 2, swatches.lights[0]);
        checkNoValidationIssues(*swatches.scene.context.graphics);
    }

    TEST_CASE("bloom spills what is brighter than the threshold over what is around it") {
        // A square of eight pixels in the middle of a black picture, fifty times white.
        constexpr uint32_t kSize = 128;
        SceneFixture scene{UVec2(kSize, kSize)};
        scene.writeFlatMaterial("glare", Vec3(50.0f));
        scene.writeFlatMaterial("dim", Vec3(0.4f));
        scene.open();
        const Entity square = scene.add(BuiltinMesh::Plane, scene.material("glare"), at(Vec3(0.0f), Vec3(0.25f)));
        const CameraView view =
            scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));

        PostProcessSettings look = plain();
        look.bloomIntensity = 1.0f;
        const Image image = scene.render(view, finishedWith(look));
        // Light where there was none, fading with the distance from the square...
        const float near = brightnessAt(image, 72, 64);
        const float further = brightnessAt(image, 80, 64);
        const float far = brightnessAt(image, 96, 64);
        CHECK(near > 0.2f);
        CHECK(further > 0.02f);
        CHECK(near > further);
        CHECK(further > far);
        // ... the same to every side.
        CHECK(brightnessAt(image, 55, 64) == doctest::Approx(near).epsilon(0.05));
        CHECK(brightnessAt(image, 64, 72) == doctest::Approx(near).epsilon(0.05));
        CHECK(brightnessAt(image, 64, 55) == doctest::Approx(near).epsilon(0.05));
        CHECK(brightnessAt(image, 47, 64) == doctest::Approx(further).epsilon(0.05));
        CHECK(brightnessAt(image, 64, 47) == doctest::Approx(further).epsilon(0.05));

        // Without bloom the picture is the square and nothing else.
        const Image none = scene.render(view, finishedWith(plain()));
        CHECK(brightnessAt(none, 72, 64) == 0.0f);
        CHECK(brightnessAt(none, 64, 64) == 1.0f);

        // Twice the intensity is twice the bloom.
        look.bloomIntensity = 0.25f;
        const float quarter = brightnessAt(scene.render(view, finishedWith(look)), 80, 64);
        look.bloomIntensity = 0.5f;
        const float half = brightnessAt(scene.render(view, finishedWith(look)), 80, 64);
        CHECK(half == doctest::Approx(2.0f * quarter).epsilon(0.08));

        // More scatter carries the light further out, and takes it from close by.
        look.bloomIntensity = 0.05f;
        look.bloomScatter = 0.3f;
        const Image tight = scene.render(view, finishedWith(look));
        look.bloomScatter = 0.9f;
        const Image wide = scene.render(view, finishedWith(look));
        CHECK(brightnessAt(wide, 110, 64) > 2.0f * brightnessAt(tight, 110, 64));
        CHECK(brightnessAt(tight, 70, 64) > brightnessAt(wide, 70, 64));

        // What is not brighter than the threshold does not bloom at all.
        scene.world.tryGet<MeshRendererComponent>(square)->materials = {scene.material("dim")};
        look = plain();
        look.bloomIntensity = 1.0f;
        const Image below = scene.render(view, finishedWith(look));
        CHECK(brightnessAt(below, 70, 64) == 0.0f);
        CHECK(brightnessAt(below, 64, 64) == doctest::Approx(0.4f).epsilon(0.03));
        // With the threshold lowered, it does.
        look.bloomThreshold = 0.1f;
        CHECK(brightnessAt(scene.render(view, finishedWith(look)), 70, 64) > 0.002f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("one glaring pixel does not bloom like a lamp") {
        // A single pixel ten thousand times white - the glint of a highlight - on black.
        constexpr uint32_t kSize = 128;
        SceneFixture scene{UVec2(kSize, kSize)};
        scene.writeFlatMaterial("glint", Vec3(10000.0f));
        scene.writeFlatMaterial("lamp", Vec3(10000.0f));
        scene.open();
        const float pixel = 4.0f / static_cast<float>(kSize);
        // Covers the middle of one pixel, and of no other.
        const Entity glint = scene.add(BuiltinMesh::Plane, scene.material("glint"),
                                       at(Vec3(0.5f * pixel, 0.0f, 0.5f * pixel), Vec3(0.4f * pixel)));
        const CameraView view =
            scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
        PostProcessSettings look = plain();
        look.bloomIntensity = 1.0f;

        const Image image = scene.render(view, finishedWith(look));
        // The pixel itself is as bright as a screen gets ...
        CHECK(brightnessAt(image, 64, 64) == 1.0f);
        // ... and what it spills is a glow, not a blot: a few pixels away, well short of
        // white. Averaged plainly, its bloom there would be dozens of times white.
        CHECK(brightnessAt(image, 68, 64) < 0.5f);
        CHECK(brightnessAt(image, 64, 70) < 0.5f);
        CHECK(brightnessAt(image, 68, 64) > 0.0f);

        // Something of a size - sixteen pixels a side - at the same brightness is a
        // lamp, and blooms like one.
        scene.world.transform(glint).scale = Vec3(16.0f * pixel);
        const Image lamp = scene.render(view, finishedWith(look));
        CHECK(brightnessAt(lamp, 76, 64) == 1.0f);
        CHECK(brightnessAt(lamp, 64, 78) == 1.0f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what is kept for the targets of a view is dropped when the view is gone") {
        test::RenderContext context;
        {
            PostProcessPass pass = unwrap(PostProcessPass::create(context.device(), *context.shaders));
            CHECK(pass.cachedViewCount() == 0);
            PostProcessSettings look;
            look.exposureMode = ExposureMode::Automatic;

            // A view that is drawn all the time, and many that come and go.
            SceneTargets lasting = unwrap(SceneTargets::create(context.device(), UVec2(32, 32), "Lasting"));
            for (uint32_t round = 0; round < 200; ++round) {
                SceneTargets passing = unwrap(SceneTargets::create(context.device(), UVec2(8, 8), "Passing"));
                test::recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                    commandList->clearTextureFloat(lasting.sceneColor(), nvrhi::AllSubresources, nvrhi::Color(0.5f));
                    commandList->clearTextureFloat(passing.sceneColor(), nvrhi::AllSubresources, nvrhi::Color(0.5f));
                    CHECK(pass.draw(commandList, lasting, look, {}) == 11);
                    CHECK(pass.draw(commandList, passing, look, {}) > 0);
                });
                // Frames are not left to pile up on the GPU: each uses a version of the
                // pass's constants until it is done.
                context.graphics->waitIdle();
                // Never more than the views of the last sixty-four calls, and the one
                // that is always among them.
                CHECK(pass.cachedViewCount() <= 66);
            }
            CHECK(pass.cachedViewCount() >= 2);
            // The view that lasted kept its exposure all along.
            CHECK(lasting.hasExposure());

            // A set of targets that changes its size is a new one.
            const uint64_t serial = lasting.serial();
            REQUIRE(lasting.resize(context.device(), UVec2(32, 32)).has_value());
            CHECK(lasting.serial() == serial);
            REQUIRE(lasting.resize(context.device(), UVec2(24, 16)).has_value());
            CHECK(lasting.serial() != serial);
            CHECK_FALSE(lasting.hasExposure());

#if defined(CORDITE_ENABLE_ASSERTS)
            // Targets that are no targets, and no command list, are mistakes of the caller.
            SceneTargets none;
            test::recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                CHECK_THROWS_AS(pass.draw(commandList, none, look, {}), test::AssertionFailure);
            });
            CHECK_THROWS_AS(pass.draw(nullptr, lasting, look, {}), test::AssertionFailure);
#endif
            context.graphics->waitIdle();
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the bloom of an even picture is that picture, at every quality") {
        // With no threshold everything blooms; blurring a picture that is the same
        // everywhere changes nothing, so the bloom adds its intensity times the picture.
        Field field(Vec3(0.3f, 0.2f, 0.1f), UVec2(96, 64));
        PostProcessSettings look = plain();
        look.bloomThreshold = 0.0f;
        look.bloomIntensity = 0.5f;
        for (const PostProcessQuality quality : kFinishing) {
            CAPTURE(nameOf(quality));
            SceneRenderSettings settings = finishedWith(look);
            settings.postProcessQuality = quality;
            const Image image = field.render(settings);
            for (const UVec2 pixel : {UVec2(48, 32), UVec2(1, 1), UVec2(94, 62), UVec2(20, 50)}) {
                CAPTURE(pixel);
                checkShown(image, pixel.x, pixel.y, linearToSrgb(Vec3(0.3f, 0.2f, 0.1f) * 1.5f));
            }
        }
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("a higher quality carries the bloom further") {
        constexpr uint32_t kSize = 256;
        SceneFixture scene{UVec2(kSize, kSize)};
        scene.writeFlatMaterial("glare", Vec3(200.0f));
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("glare"), at(Vec3(0.0f), Vec3(0.125f)));
        const CameraView view =
            scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
        PostProcessSettings look = plain();
        look.bloomIntensity = 1.0f;
        look.bloomScatter = 0.8f;

        std::vector<Image> images;
        for (const PostProcessQuality quality : kFinishing) {
            SceneRenderSettings settings = finishedWith(look);
            settings.postProcessQuality = quality;
            images.push_back(scene.render(view, settings));
            // Whatever the quality: light beside the square, less of it further away.
            CAPTURE(nameOf(quality));
            CHECK(brightnessAt(images.back(), 140, 128) > 0.05f);
            CHECK(brightnessAt(images.back(), 140, 128) > brightnessAt(images.back(), 170, 128));
        }
        // Far from the square - a hundred pixels - only the high quality has light left
        // to speak of: its bloom goes over more levels.
        CHECK(brightnessAt(images[2], 228, 128) > 1.5f * brightnessAt(images[1], 228, 128));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("automatic exposure makes the picture mid-grey on average") {
        for (const float light : {0.02f, 0.18f, 0.9f, 6.0f}) {
            CAPTURE(light);
            Field field{Vec3(light)};
            PostProcessSettings look = plain();
            look.exposureMode = ExposureMode::Automatic;
            look.autoExposureMinimum = -8.0f;
            look.autoExposureMaximum = 8.0f;
            // The first frame of a view takes what it measures; so does every frame
            // after it, while nothing changes.
            for (int frame = 0; frame < 3; ++frame) {
                CHECK(brightnessAt(field.render(look), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));
            }
            // Exposure by hand comes on top of it.
            look.exposure = 1.0f;
            CHECK(brightnessAt(field.render(look), 30, 30) == doctest::Approx(0.36f).epsilon(0.03));
            checkNoValidationIssues(*field.scene.context.graphics);
        }
    }

    TEST_CASE("automatic exposure stays within the stops it is allowed") {
        PostProcessSettings look = plain();
        look.exposureMode = ExposureMode::Automatic;
        look.autoExposureMinimum = -1.0f;
        look.autoExposureMaximum = 2.0f;
        {
            Field dark{Vec3(0.01f)};
            // Mid-grey would take more than four stops; two are allowed.
            CHECK(brightnessAt(dark.render(look), 30, 30) == doctest::Approx(0.04f).epsilon(0.05));
            checkNoValidationIssues(*dark.scene.context.graphics);
        }
        {
            Field bright{Vec3(3.0f)};
            // Four stops down would be needed; one is allowed.
            CHECK(brightnessAt(bright.render(look), 30, 30) == doctest::Approx(1.0f).epsilon(0.01));
            look.toneMapper = ToneMapper::Filmic;
            checkShown(bright.render(look), 30, 30, finished(look, Vec3(1.5f)));
            checkNoValidationIssues(*bright.scene.context.graphics);
        }
    }

    TEST_CASE("automatic exposure gets used to a new brightness over time, faster to light than to dark") {
        // 0.5 at first; then four times as bright, which is two stops to go down.
        Field field(Vec3(0.5f), UVec2(64, 64), Vec3(2.0f));
        PostProcessSettings look = plain();
        look.exposureMode = ExposureMode::Automatic;
        look.autoExposureMinimum = -8.0f;
        look.autoExposureMaximum = 8.0f;
        look.adaptationSpeedUp = 2.0f;
        look.adaptationSpeedDown = 0.5f;
        SceneRenderSettings settings = finishedWith(look);
        settings.deltaSeconds = 0.25f;

        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));
        float stops = std::log2(0.18f / 0.5f);

        // A quarter of a second later: part of the way, as the speed says.
        field.showOther(true);
        const float wantedBright = std::log2(0.18f / 2.0f);
        stops += (wantedBright - stops) * (1.0f - std::exp(-0.25f * 2.0f));
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(2.0f * std::exp2(stops)).epsilon(0.03));
        // And further with every frame.
        for (int frame = 0; frame < 3; ++frame) {
            stops += (wantedBright - stops) * (1.0f - std::exp(-0.25f * 2.0f));
            CHECK(brightnessAt(field.render(settings), 30, 30) ==
                  doctest::Approx(2.0f * std::exp2(stops)).epsilon(0.03));
        }
        // A frame that takes no time changes nothing.
        settings.deltaSeconds = 0.0f;
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(2.0f * std::exp2(stops)).epsilon(0.03));

        // Back to the darker picture: the same way, at the slower speed.
        field.showOther(false);
        settings.deltaSeconds = 0.25f;
        const float wantedDark = std::log2(0.18f / 0.5f);
        stops += (wantedDark - stops) * (1.0f - std::exp(-0.25f * 0.5f));
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.5f * std::exp2(stops)).epsilon(0.03));

        // After a long time it has arrived.
        settings.deltaSeconds = 60.0f;
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("exposure set by hand has nothing to get used to, and starts afresh when it is automatic again") {
        Field field(Vec3(0.5f), UVec2(64, 64), Vec3(4.0f));
        PostProcessSettings automatic = plain();
        automatic.exposureMode = ExposureMode::Automatic;
        automatic.autoExposureMinimum = -8.0f;
        automatic.autoExposureMaximum = 8.0f;
        SceneRenderSettings settings = finishedWith(automatic);
        settings.deltaSeconds = 0.01f;
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));

        // By hand: the light as it is.
        CHECK(brightnessAt(field.render(plain()), 30, 30) == doctest::Approx(0.5f).epsilon(0.02));

        // Automatic again, on a picture eight times as bright: no slow way from where
        // it was before, but what the new picture needs, at once.
        field.showOther(true);
        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("each view has an exposure of its own, and a view that changes its size starts afresh") {
        Field field(Vec3(0.5f), UVec2(64, 64), Vec3(4.0f));
        PostProcessSettings look = plain();
        look.exposureMode = ExposureMode::Automatic;
        look.autoExposureMinimum = -8.0f;
        look.autoExposureMaximum = 8.0f;
        SceneRenderSettings settings = finishedWith(look);
        settings.deltaSeconds = 0.01f;

        // A second view of the same world, through targets of its own.
        SceneTargets otherTargets = unwrap(SceneTargets::create(field.scene.context.device(), UVec2(32, 32), "Other"));
        const auto renderOther = [&] {
            field.scene.world.updateWorldTransforms();
            test::recordAndExecute(field.scene.context, [&](nvrhi::ICommandList* commandList) {
                field.scene.renderer->render(commandList, field.scene.world, field.view, otherTargets, settings);
            });
            return test::readToImage(*field.scene.context.graphics, otherTargets.display());
        };

        CHECK(brightnessAt(field.render(settings), 30, 30) == doctest::Approx(0.18f).epsilon(0.03));
        // The picture gets eight times as bright. The first view has an exposure to get
        // used from, and a hundredth of a second is no time for it...
        field.showOther(true);
        CHECK(brightnessAt(field.render(settings), 30, 30) > 1.0f - 0.02f);
        // ... the second has none yet, and takes what it measures.
        CHECK(brightnessAt(renderOther(), 15, 15) == doctest::Approx(0.18f).epsilon(0.03));
        CHECK(otherTargets.hasExposure());

        // With a new size a view has new textures, and no exposure from before.
        REQUIRE(otherTargets.resize(field.scene.context.device(), UVec2(48, 24)).has_value());
        CHECK_FALSE(otherTargets.hasExposure());
        field.showOther(false);
        CHECK(brightnessAt(renderOther(), 15, 15) == doctest::Approx(0.18f).epsilon(0.03));

        otherTargets = SceneTargets();
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("a small glaring light does not outweigh the room it is in") {
        // A grey room with a patch of four by four pixels a thousand times white: a
        // thousandth of the picture.
        SceneFixture scene{UVec2(128, 128)};
        scene.writeFlatMaterial("room", Vec3(0.18f));
        scene.writeFlatMaterial("glare", Vec3(1000.0f));
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("room"), at(Vec3(0.0f), Vec3(40.0f)));
        scene.add(BuiltinMesh::Plane, scene.material("glare"), at(Vec3(0.0f, 0.1f, 0.0f), Vec3(0.125f)));
        const CameraView view =
            scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));

        PostProcessSettings look = plain();
        look.exposureMode = ExposureMode::Automatic;
        look.autoExposureMinimum = -8.0f;
        look.autoExposureMaximum = 8.0f;
        // The room stays close to mid-grey: the average is of the logarithms, in which
        // the patch counts for its size. By the plain average it would be a fifth of
        // that or less.
        const float room = brightnessAt(scene.render(view, finishedWith(look)), 20, 100);
        CHECK(room > 0.14f);
        CHECK(room < 0.181f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a lookup table changes the colors of the finished picture") {
        Swatches swatches({Vec3(0.5f, 0.25f, 0.1f), Vec3(0.1f, 0.6f, 0.9f), Vec3(1.0f, 0.2f, 0.05f),
                           Vec3(0.02f, 0.3f, 0.02f), Vec3(0.18f), Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f),
                           Vec3(0.7f, 0.7f, 0.75f)});
        SceneFixture& scene = swatches.scene;
        scene.writeImage("same", makeLut(16, [](const Vec3& color) { return color; }));
        scene.writeImage("negative", makeLut(16, [](const Vec3& color) { return Vec3(1.0f) - color; }));
        scene.writeImage("swapped", makeLut(32, [](const Vec3& color) { return Vec3(color.b, color.r, color.g); }));
        scene.writeImage("teal", makeLut(16, [](const Vec3&) { return Vec3(0.0f, 0.5f, 0.5f); }));
        REQUIRE(scene.assets->refresh().has_value());

        const auto shownWith = [&](const char* table, float intensity) {
            PostProcessSettings look = plain();
            look.colorLut = scene.asset(table);
            look.colorLutIntensity = intensity;
            return swatches.render(look);
        };
        const PostProcessSettings look = plain();

        for (size_t column = 0; column < 8; ++column) {
            CAPTURE(column);
            const uint32_t x = Swatches::middleOf(column);
            const Vec3 shown = finished(look, swatches.lights[column]);
            // A table that holds every color at its own place changes nothing...
            CHECK_NEAR(Vec3(shownWith("same.png", 1.0f).pixel(x, 32)), shown, 3.5f / 255.0f);
            // ... one that holds the opposite color makes a negative, ...
            CHECK_NEAR(Vec3(shownWith("negative.png", 1.0f).pixel(x, 32)), Vec3(1.0f) - shown, 3.5f / 255.0f);
            // ... one with red, green and blue changed round changes them round, at any
            // size of table, ...
            CHECK_NEAR(Vec3(shownWith("swapped.png", 1.0f).pixel(x, 32)), Vec3(shown.b, shown.r, shown.g),
                       3.5f / 255.0f);
            // ... and its intensity says how much of that is taken.
            CHECK_NEAR(Vec3(shownWith("teal.png", 1.0f).pixel(x, 32)), Vec3(0.0f, 0.5f, 0.5f), 3.5f / 255.0f);
            CHECK_NEAR(Vec3(shownWith("teal.png", 0.25f).pixel(x, 32)), glm::mix(shown, Vec3(0.0f, 0.5f, 0.5f), 0.25f),
                       3.5f / 255.0f);
            CHECK_NEAR(Vec3(shownWith("negative.png", 0.0f).pixel(x, 32)), shown, 2.5f / 255.0f);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a lookup table of the wrong shape, or one that is not there, is left out and reported once") {
        Swatches swatches(greys());
        SceneFixture& scene = swatches.scene;
        scene.writeImage("odd", unwrap(Image::create(100, 12, ImageFormat::Rgba8)));
        scene.writeImage("square", unwrap(Image::create(16, 16, ImageFormat::Rgba8)));
        REQUIRE(scene.assets->refresh().has_value());

        const LogCapture log;
        PostProcessSettings look = plain();
        look.colorLut = scene.asset("odd.png");
        swatches.check(swatches.render(look), plain());
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("a color lookup table of 100x12 pixels is left out"));
        look.colorLut = scene.asset("square.png");
        swatches.check(swatches.render(look), plain());
        swatches.check(swatches.render(look), plain());
        CHECK(log.count(LogLevel::Warn) == 1);

        // An id that names nothing: the renderer says so, and the picture is as without.
        const std::optional<Uuid> nothing = Uuid::parse("0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f");
        REQUIRE(nothing.has_value());
        look.colorLut = *nothing;
        swatches.check(swatches.render(look), plain());
        swatches.check(swatches.render(look), plain());
        CHECK(log.count(LogLevel::Error) == 1);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the passes over the whole picture are counted: the lighting, and what finishes the picture") {
        // 256 pixels a side: a bloom chain of all eight levels.
        Field field(Vec3(0.5f), UVec2(256, 256));
        REQUIRE(field.scene.targets.bloomLevelCount() == kMaxBloomLevels);
        const auto passesWith = [&](const SceneRenderSettings& settings) {
            field.render(settings);
            return field.scene.renderer->statistics().screenPasses;
        };

        // Not finished: the lighting, and the encoding for a display.
        SceneRenderSettings settings = finishedWith(plain());
        settings.postProcessQuality = PostProcessQuality::Off;
        CHECK(passesWith(settings) == 2);
        // Finished without bloom and with the exposure set by hand: one pass does it.
        settings.postProcessQuality = PostProcessQuality::Medium;
        CHECK(passesWith(settings) == 2);
        // Smoothing its edges is one more; a picture that is not finished has none.
        settings.antiAliasing = AntiAliasing::Fxaa;
        CHECK(passesWith(settings) == 3);
        settings.postProcessQuality = PostProcessQuality::Off;
        CHECK(passesWith(settings) == 2);

        // With bloom: into its first level, down the others and up again. Four levels
        // at a low quality, five at a medium one, seven at a high one.
        PostProcessSettings look = plain();
        look.bloomIntensity = 0.2f;
        settings = finishedWith(look);
        settings.postProcessQuality = PostProcessQuality::Low;
        CHECK(passesWith(settings) == 1 + 1 + 3 + 3 + 1);
        settings.postProcessQuality = PostProcessQuality::Medium;
        CHECK(passesWith(settings) == 1 + 1 + 4 + 4 + 1);
        settings.postProcessQuality = PostProcessQuality::High;
        CHECK(passesWith(settings) == 1 + 1 + 6 + 6 + 1);

        // Automatic exposure is one more, and needs the way down even without bloom.
        look.exposureMode = ExposureMode::Automatic;
        settings = finishedWith(look);
        CHECK(passesWith(settings) == 1 + 1 + 4 + 1 + 4 + 1);
        look.bloomIntensity = 0.0f;
        settings = finishedWith(look);
        CHECK(passesWith(settings) == 1 + 1 + 4 + 1 + 1);

        // A debug view of the G-buffer takes the place of the lighting and is not
        // finished; one that draws the meshes itself has only the encoding.
        settings.debugView = RenderDebugView::Emissive;
        CHECK(passesWith(settings) == 2);
        settings.debugView = RenderDebugView::Overdraw;
        CHECK(passesWith(settings) == 1);

        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("a view too small for all the levels of a quality uses the levels it has") {
        // Eight pixels a side: levels of four, two and one.
        Field field(Vec3(0.5f), UVec2(8, 8));
        REQUIRE(field.scene.targets.bloomLevelCount() == 3);
        PostProcessSettings look = plain();
        look.bloomIntensity = 0.5f;
        look.bloomThreshold = 0.0f;
        for (const PostProcessQuality quality : kFinishing) {
            CAPTURE(nameOf(quality));
            SceneRenderSettings settings = finishedWith(look);
            settings.postProcessQuality = quality;
            const Image image = field.render(settings);
            // An even picture: its bloom is the picture, whichever levels it went through.
            checkShown(image, 4, 4, linearToSrgb(Vec3(0.75f)));
            // From the first level the quality starts at, down to the last there is.
            const uint32_t levels = quality == PostProcessQuality::Low ? 2 : 3;
            CHECK(field.scene.renderer->statistics().screenPasses == 1 + 1 + (levels - 1) * 2 + 1);
        }
        checkNoValidationIssues(*field.scene.context.graphics);
    }

    TEST_CASE("a view of one pixel is finished like any other") {
        // The bloom chain of such a view is one level of one texel.
        Field field(Vec3(0.5f), UVec2(1, 1));
        CHECK(field.scene.targets.bloomLevelCount() == 1);
        PostProcessSettings look;
        look.exposureMode = ExposureMode::Automatic;
        look.bloomIntensity = 0.3f;
        look.bloomThreshold = 0.0f;
        look.vignette = 0.5f;
        look.chromaticAberration = 1.0f;
        look.filmGrain = 0.2f;
        for (const PostProcessQuality quality : kFinishing) {
            SceneRenderSettings settings = finishedWith(look);
            settings.postProcessQuality = quality;
            const Image image = field.render(settings);
            CHECK(image.pixel(0, 0).g > 0.05f);
            CHECK(image.pixel(0, 0).g < 0.95f);
        }
        checkNoValidationIssues(*field.scene.context.graphics);
    }
}
