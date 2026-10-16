#include "cordite/renderer/PostProcessSettings.h"

#include "cordite/core/Uuid.h"
#include "cordite/math/Color.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <initializer_list>
#include <limits>

using namespace cordite;

namespace {

Uuid idOf(const char* text) {
    const std::optional<Uuid> id = Uuid::parse(text);
    REQUIRE(id.has_value());
    return *id;
}

} // namespace

TEST_SUITE("PostProcessSettings") {
    TEST_CASE("tone mappers, exposure modes and qualities have names") {
        CHECK(nameOf(ToneMapper::Filmic) == "filmic");
        CHECK(nameOf(ToneMapper::Aces) == "aces");
        CHECK(nameOf(ToneMapper::Linear) == "linear");
        CHECK(kToneMappers.size() == 3);
        CHECK(nameOf(ExposureMode::Manual) == "manual");
        CHECK(nameOf(ExposureMode::Automatic) == "automatic");

        CHECK(nameOf(PostProcessQuality::Off) == "off");
        CHECK(nameOf(PostProcessQuality::Low) == "low");
        CHECK(nameOf(PostProcessQuality::Medium) == "medium");
        CHECK(nameOf(PostProcessQuality::High) == "high");
        CHECK(displayNameOf(PostProcessQuality::Off) == "Off");
        CHECK(displayNameOf(PostProcessQuality::Low) == "Low");
        CHECK(displayNameOf(PostProcessQuality::Medium) == "Medium");
        CHECK(displayNameOf(PostProcessQuality::High) == "High");
        for (const PostProcessQuality quality : kPostProcessQualities) {
            CHECK(postProcessQualityFromName(nameOf(quality)) == quality);
        }
        CHECK(kPostProcessQualities.size() == 4);
        CHECK(postProcessQualityFromName("HIGH") == PostProcessQuality::High);
        CHECK(postProcessQualityFromName("Off") == PostProcessQuality::Off);
        CHECK_FALSE(postProcessQualityFromName("ultra").has_value());
        CHECK_FALSE(postProcessQualityFromName("").has_value());

        CHECK(nameOf(AntiAliasing::Off) == "off");
        CHECK(nameOf(AntiAliasing::Fxaa) == "fxaa");
        CHECK(displayNameOf(AntiAliasing::Off) == "Off");
        CHECK(displayNameOf(AntiAliasing::Fxaa) == "FXAA");
        for (const AntiAliasing antiAliasing : kAntiAliasingModes) {
            CHECK(antiAliasingFromName(nameOf(antiAliasing)) == antiAliasing);
        }
        CHECK(kAntiAliasingModes.size() == 2);
        CHECK(antiAliasingFromName("FXAA") == AntiAliasing::Fxaa);
        CHECK(antiAliasingFromName("Off") == AntiAliasing::Off);
        CHECK_FALSE(antiAliasingFromName("msaa").has_value());
        CHECK_FALSE(antiAliasingFromName("").has_value());
    }

    TEST_CASE("a quality says where the bloom starts, how far it goes, whether the lens shows, and how "
              "closely edges are looked at") {
        CHECK(postProcessSettingsFor(PostProcessQuality::Low) ==
              PostProcessQualitySettings{.bloomFirstLevel = 1,
                                         .bloomLevels = 4,
                                         .lensEffects = false,
                                         .antiAliasingSteps = 5,
                                         .antiAliasingEdgeThreshold = 0.25f,
                                         .antiAliasingEdgeMinimum = 0.0833f,
                                         .depthOfFieldTaps = 16});
        CHECK(postProcessSettingsFor(PostProcessQuality::Medium) ==
              PostProcessQualitySettings{.bloomFirstLevel = 0,
                                         .bloomLevels = 5,
                                         .lensEffects = true,
                                         .antiAliasingSteps = 8,
                                         .antiAliasingEdgeThreshold = 0.166f,
                                         .antiAliasingEdgeMinimum = 0.0625f,
                                         .depthOfFieldTaps = 32});
        CHECK(postProcessSettingsFor(PostProcessQuality::High) ==
              PostProcessQualitySettings{.bloomFirstLevel = 0,
                                         .bloomLevels = 7,
                                         .lensEffects = true,
                                         .antiAliasingSteps = 12,
                                         .antiAliasingEdgeThreshold = 0.125f,
                                         .antiAliasingEdgeMinimum = 0.0312f,
                                         .depthOfFieldTaps = 64});
        // The higher the quality, the further an edge is followed and the fainter the
        // edges that are looked at; an edge always has to be something.
        PostProcessQualitySettings before = postProcessSettingsFor(PostProcessQuality::Low);
        for (const PostProcessQuality quality : {PostProcessQuality::Medium, PostProcessQuality::High}) {
            const PostProcessQualitySettings settings = postProcessSettingsFor(quality);
            CHECK(settings.antiAliasingSteps > before.antiAliasingSteps);
            CHECK(settings.antiAliasingEdgeThreshold < before.antiAliasingEdgeThreshold);
            CHECK(settings.antiAliasingEdgeMinimum < before.antiAliasingEdgeMinimum);
            CHECK(settings.antiAliasingEdgeMinimum > 0.0f);
            CHECK(settings.depthOfFieldTaps > before.depthOfFieldTaps);
            before = settings;
        }
        // What the defaults of the struct are, a medium quality is.
        CHECK(postProcessSettingsFor(PostProcessQuality::Medium) == PostProcessQualitySettings());
        // Off finishes nothing; asked all the same, it answers as the lowest quality.
        CHECK(postProcessSettingsFor(PostProcessQuality::Off) == postProcessSettingsFor(PostProcessQuality::Low));
    }

    TEST_CASE("the filmic curve keeps mid-grey, rolls highlights off and ends at white") {
        // Worked out by hand from J. Hable's formula, scaled by 2.9275 and divided by its
        // value at 11.2.
        const auto filmic = [](float light) { return applyToneMapper(ToneMapper::Filmic, Vec3(light)).g; };
        CHECK(filmic(0.0f) == doctest::Approx(0.0f).epsilon(1.0e-6));
        CHECK(filmic(0.01f) == doctest::Approx(0.011213f).epsilon(0.001));
        CHECK(filmic(0.05f) == doctest::Approx(0.054992f).epsilon(0.001));
        CHECK(filmic(0.18f) == doctest::Approx(0.18f).epsilon(0.001));
        CHECK(filmic(0.5f) == doctest::Approx(0.401799f).epsilon(0.001));
        CHECK(filmic(1.0f) == doctest::Approx(0.612944f).epsilon(0.001));
        CHECK(filmic(2.0f) == doctest::Approx(0.830684f).epsilon(0.001));
        CHECK(filmic(3.0f) == doctest::Approx(0.942115f).epsilon(0.001));
        // White is reached a little short of four, and nothing gets brighter than it.
        CHECK(filmic(3.8258f) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(filmic(8.0f) == 1.0f);
        CHECK(filmic(1.0e6f) == 1.0f);
    }

    TEST_CASE("the ACES curve keeps mid-grey too, with more contrast") {
        // K. Narkowicz's fit, with the light scaled by 0.7232 first.
        const auto aces = [](float light) { return applyToneMapper(ToneMapper::Aces, Vec3(light)).g; };
        CHECK(aces(0.0f) == doctest::Approx(0.0f).epsilon(1.0e-6));
        CHECK(aces(0.01f) == doctest::Approx(0.002412f).epsilon(0.002));
        CHECK(aces(0.05f) == doctest::Approx(0.026544f).epsilon(0.001));
        CHECK(aces(0.18f) == doctest::Approx(0.18f).epsilon(0.001));
        CHECK(aces(0.5f) == doctest::Approx(0.50522f).epsilon(0.001));
        CHECK(aces(1.0f) == doctest::Approx(0.726195f).epsilon(0.001));
        CHECK(aces(2.0f) == doctest::Approx(0.871219f).epsilon(0.001));
        CHECK(aces(8.0f) == doctest::Approx(0.99173f).epsilon(0.001));
        CHECK(aces(1.0e6f) == 1.0f);
        // Darker in the shadows and brighter in the highlights than the filmic curve.
        CHECK(aces(0.05f) < applyToneMapper(ToneMapper::Filmic, Vec3(0.05f)).g);
        CHECK(aces(1.0f) > applyToneMapper(ToneMapper::Filmic, Vec3(1.0f)).g);
    }

    TEST_CASE("every tone mapper rises with the light, stays in range and takes negative light as none") {
        for (const ToneMapper toneMapper : kToneMappers) {
            CAPTURE(nameOf(toneMapper));
            float before = 0.0f;
            for (float light = 0.0f; light < 20.0f; light += 0.01f) {
                const Vec3 shown = applyToneMapper(toneMapper, Vec3(light));
                CHECK(shown.r >= before);
                CHECK(shown.r <= 1.0f);
                // Grey stays grey.
                CHECK(shown.r == shown.g);
                CHECK(shown.g == shown.b);
                before = shown.r;
            }
            // (Nothing, to within what the arithmetic of a curve leaves of it.)
            CHECK_NEAR(applyToneMapper(toneMapper, Vec3(-1.0f, -0.5f, -1.0e6f)), Vec3(0.0f), 1.0e-6f);
            // Each color by itself.
            const Vec3 mixed = applyToneMapper(toneMapper, Vec3(0.18f, 4.0f, 0.0f));
            CHECK(mixed.r == applyToneMapper(toneMapper, Vec3(0.18f)).r);
            CHECK(mixed.g == applyToneMapper(toneMapper, Vec3(4.0f)).g);
            CHECK(mixed.b < 1.0e-6f);
        }
        // Without a curve, light is what it is until it clips.
        CHECK(applyToneMapper(ToneMapper::Linear, Vec3(0.25f, 1.0f, 3.0f)) == Vec3(0.25f, 1.0f, 1.0f));
    }

    TEST_CASE("a stop of exposure doubles the light") {
        CHECK(exposureScale(0.0f) == 1.0f);
        CHECK(exposureScale(1.0f) == 2.0f);
        CHECK(exposureScale(3.0f) == 8.0f);
        CHECK(exposureScale(-2.0f) == 0.25f);
        CHECK(exposureScale(0.5f) == doctest::Approx(std::sqrt(2.0f)));
    }

    TEST_CASE("white balance shifts the colors and keeps white as bright as it was") {
        CHECK_NEAR(whiteBalanceScale(0.0f, 0.0f), Vec3(1.0f), 1.0e-6f);
        // Warmer: more red, less blue, by the same factor either way.
        CHECK_NEAR(whiteBalanceScale(1.0f, 0.0f), Vec3(1.325517f, 0.937282f, 0.662758f), 1.0e-4f);
        // Cooler and towards magenta.
        CHECK_NEAR(whiteBalanceScale(-0.5f, 0.5f), Vec3(0.970962f, 0.970962f, 1.373148f), 1.0e-4f);
        for (const float temperature : {-1.0f, -0.3f, 0.0f, 0.7f, 1.0f}) {
            for (const float tint : {-1.0f, 0.0f, 0.4f, 1.0f}) {
                CAPTURE(temperature);
                CAPTURE(tint);
                CHECK(luminance(whiteBalanceScale(temperature, tint)) == doctest::Approx(1.0f));
            }
        }
        // Beyond the range is the end of the range; no number is no change.
        CHECK(whiteBalanceScale(5.0f, -7.0f) == whiteBalanceScale(1.0f, -1.0f));
        CHECK_NEAR(whiteBalanceScale(std::numeric_limits<float>::quiet_NaN(), 0.0f), Vec3(1.0f), 1.0e-6f);
    }

    TEST_CASE("grading with nothing set leaves a color as it is") {
        const PostProcessSettings settings;
        for (const Vec3& color :
             {Vec3(0.0f), Vec3(0.18f), Vec3(1.0f), Vec3(0.5f, 0.25f, 0.1f), Vec3(0.0f, 1.0f, 0.3f)}) {
            CHECK_NEAR(applyGrading(settings, color), color, 1.0e-5f);
        }
        // What comes in out of range is brought into it.
        CHECK(applyGrading(settings, Vec3(-1.0f, 2.0f, 0.5f)) == Vec3(0.0f, 1.0f, 0.5f));
    }

    TEST_CASE("saturation, contrast, lift, gamma and gain do what they say, in that order") {
        const Vec3 color(0.5f, 0.25f, 0.1f);

        PostProcessSettings settings;
        settings.saturation = 0.5f;
        CHECK_NEAR(applyGrading(settings, color), Vec3(0.39616f, 0.27116f, 0.19616f), 1.0e-5f);
        settings.saturation = 0.0f;
        CHECK_NEAR(applyGrading(settings, color), Vec3(luminance(color)), 1.0e-5f);

        settings = {};
        settings.contrast = 1.5f;
        CHECK_NEAR(applyGrading(settings, color), Vec3(0.833333f, 0.294628f, 0.074536f), 1.0e-5f);
        settings.contrast = 0.5f;
        CHECK_NEAR(applyGrading(settings, color), Vec3(0.3f, 0.212132f, 0.134164f), 1.0e-5f);
        // Mid-grey is where contrast turns: it stays, and so do black and (clipped) white.
        CHECK_NEAR(applyGrading(settings, Vec3(0.18f)), Vec3(0.18f), 1.0e-5f);
        CHECK(applyGrading(settings, Vec3(0.0f)) == Vec3(0.0f));

        settings = {};
        settings.lift = Vec3(0.1f);
        // Lift raises black and leaves white.
        CHECK_NEAR(applyGrading(settings, Vec3(0.0f)), Vec3(0.1f), 1.0e-5f);
        CHECK_NEAR(applyGrading(settings, Vec3(1.0f)), Vec3(1.0f), 1.0e-5f);
        settings.gamma = Vec3(2.0f);
        settings.gain = Vec3(0.8f);
        CHECK_NEAR(applyGrading(settings, Vec3(0.25f)), Vec3(0.509902f), 1.0e-5f);

        // Each color has its own.
        settings = {};
        settings.gain = Vec3(1.0f, 0.5f, 0.0f);
        CHECK_NEAR(applyGrading(settings, Vec3(0.4f)), Vec3(0.4f, 0.2f, 0.0f), 1.0e-5f);
        settings = {};
        settings.gamma = Vec3(1.0f, 2.0f, 0.5f);
        CHECK_NEAR(applyGrading(settings, Vec3(0.25f)), Vec3(0.25f, 0.5f, 0.0625f), 1.0e-5f);

        // Saturation comes before contrast: a color made grey first has one contrast.
        settings = {};
        settings.saturation = 0.0f;
        settings.contrast = 1.5f;
        const float grey = luminance(color);
        CHECK_NEAR(applyGrading(settings, color), Vec3(0.18f * std::pow(grey / 0.18f, 1.5f)), 1.0e-5f);
    }

    TEST_CASE("a vignette leaves the middle alone and darkens the corners") {
        CHECK(vignetteScale(0.0f, Vec2(1.0f, 1.0f)) == 1.0f);
        CHECK(vignetteScale(0.6f, Vec2(0.0f)) == 1.0f);
        CHECK(vignetteScale(0.6f, Vec2(1.0f, 1.0f)) == doctest::Approx(0.4f));
        CHECK(vignetteScale(0.6f, Vec2(-1.0f, 1.0f)) == doctest::Approx(0.4f));
        // Half way to a side: an eighth of what the corners lose.
        CHECK(vignetteScale(0.8f, Vec2(0.5f, 0.0f)) == doctest::Approx(0.9f));
        CHECK(vignetteScale(1.0f, Vec2(1.0f, 1.0f)) == doctest::Approx(0.0f));
        // Never less than nothing, however far out or however much.
        CHECK(vignetteScale(1.0f, Vec2(3.0f, 3.0f)) == doctest::Approx(0.0f));
        CHECK(vignetteScale(9.0f, Vec2(1.0f, 1.0f)) == doctest::Approx(0.0f));
    }

    TEST_CASE("depth of field blurs what is not at the focus, more the further it is, up to its most") {
        PostProcessSettings look;
        look.focusDistance = 10.0f;
        look.focusRange = 2.0f;
        look.focusTransition = 4.0f;
        look.nearBlur = 0.5f;
        look.farBlur = 1.0f;
        CHECK(hasDepthOfField(look));
        // Sharp at the focus, and as far as its range goes - its ends included.
        CHECK(focusBlur(look, 10.0f) == 0.0f);
        CHECK(focusBlur(look, 12.0f) == 0.0f);
        CHECK(focusBlur(look, 8.0f) == 0.0f);
        // Behind: half way through the transition, half the blur; beyond it, all.
        CHECK(focusBlur(look, 14.0f) == 0.5f);
        CHECK(focusBlur(look, 16.0f) == 1.0f);
        CHECK(focusBlur(look, 1000.0f) == 1.0f);
        // In front: of the near blur, and negative.
        CHECK(focusBlur(look, 6.0f) == -0.25f);
        CHECK(focusBlur(look, 0.5f) == -0.5f);
        // Where nothing was drawn is infinitely far; a depth that is no number is sharp.
        CHECK(focusBlur(look, std::numeric_limits<float>::infinity()) == 1.0f);
        CHECK(focusBlur(look, std::numeric_limits<float>::quiet_NaN()) == 0.0f);

        // Without a transition, what is not sharp is blurred at its most.
        look.focusTransition = 0.0f;
        CHECK(focusBlur(look, 12.0f) == 0.0f);
        CHECK(focusBlur(look, 12.01f) == 1.0f);
        CHECK(focusBlur(look, 7.99f) == -0.5f);

        // Without blur there is none, anywhere - and no depth of field at all.
        look.nearBlur = 0.0f;
        look.farBlur = 0.0f;
        CHECK(focusBlur(look, 100.0f) == 0.0f);
        CHECK(focusBlur(look, 0.1f) == 0.0f);
        CHECK_FALSE(hasDepthOfField(look));
        CHECK_FALSE(hasDepthOfField(PostProcessSettings{}));
        look.farBlur = 1.0f;
        look.blurSize = 0.0f;
        CHECK_FALSE(hasDepthOfField(look));
    }

    TEST_CASE("settings are brought into their ranges, and what is not a number becomes the default") {
        const PostProcessSettings defaults;
        // What is in range is left alone.
        PostProcessSettings fine;
        fine.exposure = -2.5f;
        fine.bloomIntensity = 0.7f;
        fine.contrast = 1.3f;
        fine.gamma = Vec3(0.8f, 1.0f, 1.4f);
        fine.colorLut = idOf("11111111-2222-4333-8444-555555555555");
        const PostProcessSettings same = sanitized(fine);
        CHECK(same.exposure == -2.5f);
        CHECK(same.bloomIntensity == 0.7f);
        CHECK(same.contrast == 1.3f);
        CHECK(same.gamma == Vec3(0.8f, 1.0f, 1.4f));
        CHECK(same.colorLut == fine.colorLut);
        CHECK(same.toneMapper == fine.toneMapper);

        PostProcessSettings wild;
        wild.exposure = 100.0f;
        wild.autoExposureMinimum = 2.0f;
        wild.autoExposureMaximum = -2.0f;
        wild.adaptationSpeedUp = -1.0f;
        wild.bloomIntensity = -3.0f;
        wild.bloomThreshold = -1.0f;
        wild.bloomScatter = 2.0f;
        wild.temperature = -4.0f;
        wild.tint = 4.0f;
        wild.contrast = 0.0f;
        wild.saturation = 9.0f;
        wild.lift = Vec3(-5.0f, 0.2f, 5.0f);
        wild.gamma = Vec3(0.0f, 1.0f, 99.0f);
        wild.gain = Vec3(-1.0f, 1.0f, 99.0f);
        wild.colorLutIntensity = 3.0f;
        wild.vignette = 2.0f;
        wild.chromaticAberration = -1.0f;
        wild.filmGrain = 7.0f;
        wild.focusDistance = -5.0f;
        wild.focusRange = 1.0e9f;
        wild.focusTransition = std::numeric_limits<float>::quiet_NaN();
        wild.nearBlur = 3.0f;
        wild.farBlur = -1.0f;
        wild.blurSize = 0.5f;
        const PostProcessSettings tamed = sanitized(wild);
        CHECK(tamed.exposure == 16.0f);
        CHECK(tamed.autoExposureMinimum == 2.0f);
        // The most is not less than the fewest.
        CHECK(tamed.autoExposureMaximum == 2.0f);
        CHECK(tamed.adaptationSpeedUp == 0.0f);
        CHECK(tamed.bloomIntensity == 0.0f);
        CHECK(tamed.bloomThreshold == 0.0f);
        CHECK(tamed.bloomScatter == 1.0f);
        CHECK(tamed.temperature == -1.0f);
        CHECK(tamed.tint == 1.0f);
        CHECK(tamed.contrast == 0.5f);
        CHECK(tamed.saturation == 2.0f);
        CHECK(tamed.lift == Vec3(-1.0f, 0.2f, 1.0f));
        CHECK(tamed.gamma == Vec3(0.2f, 1.0f, 5.0f));
        CHECK(tamed.gain == Vec3(0.0f, 1.0f, 4.0f));
        CHECK(tamed.colorLutIntensity == 1.0f);
        CHECK(tamed.vignette == 1.0f);
        CHECK(tamed.chromaticAberration == 0.0f);
        CHECK(tamed.filmGrain == 1.0f);
        CHECK(tamed.focusDistance == 0.0f);
        CHECK(tamed.focusRange == kMaxFocusDistance);
        CHECK(tamed.focusTransition == defaults.focusTransition);
        CHECK(tamed.nearBlur == 1.0f);
        CHECK(tamed.farBlur == 0.0f);
        CHECK(tamed.blurSize == kMaxBlurSize);

        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        PostProcessSettings broken;
        broken.exposure = notANumber;
        broken.autoExposureMinimum = notANumber;
        broken.autoExposureMaximum = notANumber;
        broken.adaptationSpeedUp = notANumber;
        broken.adaptationSpeedDown = notANumber;
        broken.bloomIntensity = notANumber;
        broken.bloomThreshold = notANumber;
        broken.bloomScatter = notANumber;
        broken.temperature = notANumber;
        broken.tint = notANumber;
        broken.contrast = notANumber;
        broken.saturation = notANumber;
        broken.lift = Vec3(notANumber);
        broken.gamma = Vec3(notANumber, 2.0f, notANumber);
        broken.gain = Vec3(notANumber);
        broken.colorLutIntensity = notANumber;
        broken.vignette = notANumber;
        broken.chromaticAberration = notANumber;
        broken.filmGrain = notANumber;
        const PostProcessSettings mended = sanitized(broken);
        CHECK(mended.exposure == defaults.exposure);
        CHECK(mended.autoExposureMinimum == defaults.autoExposureMinimum);
        CHECK(mended.autoExposureMaximum == defaults.autoExposureMaximum);
        CHECK(mended.adaptationSpeedUp == defaults.adaptationSpeedUp);
        CHECK(mended.adaptationSpeedDown == defaults.adaptationSpeedDown);
        CHECK(mended.bloomIntensity == defaults.bloomIntensity);
        CHECK(mended.bloomThreshold == defaults.bloomThreshold);
        CHECK(mended.bloomScatter == defaults.bloomScatter);
        CHECK(mended.temperature == defaults.temperature);
        CHECK(mended.tint == defaults.tint);
        CHECK(mended.contrast == defaults.contrast);
        CHECK(mended.saturation == defaults.saturation);
        CHECK(mended.lift == defaults.lift);
        CHECK(mended.gamma == Vec3(1.0f, 2.0f, 1.0f));
        CHECK(mended.gain == defaults.gain);
        CHECK(mended.colorLutIntensity == defaults.colorLutIntensity);
        CHECK(mended.vignette == defaults.vignette);
        CHECK(mended.chromaticAberration == defaults.chromaticAberration);
        CHECK(mended.filmGrain == defaults.filmGrain);
        CHECK(mended.ambientOcclusionIntensity == defaults.ambientOcclusionIntensity);
        CHECK(mended.ambientOcclusionRadius == defaults.ambientOcclusionRadius);
        CHECK(mended.ambientOcclusionBias == defaults.ambientOcclusionBias);

        // The most stops are not fewer than the fewest, even when it is the fewest that
        // were out of range, or the most that were no number.
        PostProcessSettings crossed;
        crossed.autoExposureMinimum = 5.0f;
        crossed.autoExposureMaximum = notANumber;
        CHECK(sanitized(crossed).autoExposureMinimum == 5.0f);
        CHECK(sanitized(crossed).autoExposureMaximum == 5.0f);
        crossed.autoExposureMinimum = 99.0f;
        crossed.autoExposureMaximum = 1.0f;
        CHECK(sanitized(crossed).autoExposureMinimum == 16.0f);
        CHECK(sanitized(crossed).autoExposureMaximum == 16.0f);

        // Without end: the end of the range.
        PostProcessSettings endless;
        endless.exposure = std::numeric_limits<float>::infinity();
        endless.vignette = -std::numeric_limits<float>::infinity();
        endless.ambientOcclusionRadius = std::numeric_limits<float>::infinity();
        CHECK(sanitized(endless).exposure == 16.0f);
        CHECK(sanitized(endless).vignette == 0.0f);
        CHECK(sanitized(endless).ambientOcclusionRadius == 10.0f);

        // The ambient occlusion: no stronger than four times, no further than ten metres
        // and no nearer than a centimetre, a bias of at most thirty degrees.
        PostProcessSettings occluded;
        occluded.ambientOcclusionIntensity = 1.5f;
        occluded.ambientOcclusionRadius = 2.0f;
        occluded.ambientOcclusionBias = 12.0f;
        CHECK(sanitized(occluded).ambientOcclusionIntensity == 1.5f);
        CHECK(sanitized(occluded).ambientOcclusionRadius == 2.0f);
        CHECK(sanitized(occluded).ambientOcclusionBias == 12.0f);
        occluded.ambientOcclusionIntensity = 9.0f;
        occluded.ambientOcclusionRadius = 0.0f;
        occluded.ambientOcclusionBias = 90.0f;
        CHECK(sanitized(occluded).ambientOcclusionIntensity == 4.0f);
        CHECK(sanitized(occluded).ambientOcclusionRadius == 0.01f);
        CHECK(sanitized(occluded).ambientOcclusionBias == 30.0f);
        occluded.ambientOcclusionIntensity = -1.0f;
        occluded.ambientOcclusionBias = -5.0f;
        CHECK(sanitized(occluded).ambientOcclusionIntensity == 0.0f);
        CHECK(sanitized(occluded).ambientOcclusionBias == 0.0f);
    }

    TEST_CASE("blending settings blends their numbers and switches the rest half way") {
        PostProcessSettings from;
        from.exposureMode = ExposureMode::Manual;
        from.exposure = -1.0f;
        from.toneMapper = ToneMapper::Filmic;
        from.bloomIntensity = 0.0f;
        from.saturation = 1.0f;
        from.lift = Vec3(0.0f, 0.1f, 0.2f);
        from.vignette = 0.2f;
        from.ambientOcclusionIntensity = 0.0f;
        from.ambientOcclusionRadius = 0.2f;
        from.ambientOcclusionBias = 0.0f;
        from.focusDistance = 4.0f;
        from.focusRange = 0.0f;
        from.focusTransition = 2.0f;
        from.nearBlur = 0.0f;
        from.farBlur = 1.0f;
        from.blurSize = 0.02f;
        PostProcessSettings to;
        to.exposureMode = ExposureMode::Automatic;
        to.exposure = 1.0f;
        to.toneMapper = ToneMapper::Aces;
        to.bloomIntensity = 1.0f;
        to.saturation = 0.0f;
        to.lift = Vec3(0.2f, 0.1f, 0.0f);
        to.vignette = 0.6f;
        to.ambientOcclusionIntensity = 2.0f;
        to.ambientOcclusionRadius = 1.0f;
        to.ambientOcclusionBias = 8.0f;
        to.focusDistance = 8.0f;
        to.focusRange = 4.0f;
        to.focusTransition = 6.0f;
        to.nearBlur = 1.0f;
        to.farBlur = 0.0f;
        to.blurSize = 0.0f;

        const PostProcessSettings start = blend(from, to, 0.0f);
        CHECK(start.exposure == -1.0f);
        CHECK(start.exposureMode == ExposureMode::Manual);
        CHECK(start.toneMapper == ToneMapper::Filmic);
        CHECK(start.vignette == 0.2f);

        const PostProcessSettings quarter = blend(from, to, 0.25f);
        CHECK(quarter.exposure == doctest::Approx(-0.5f));
        CHECK(quarter.bloomIntensity == doctest::Approx(0.25f));
        CHECK(quarter.saturation == doctest::Approx(0.75f));
        CHECK_NEAR(quarter.lift, Vec3(0.05f, 0.1f, 0.15f), 1.0e-6f);
        CHECK(quarter.vignette == doctest::Approx(0.3f));
        CHECK(quarter.ambientOcclusionIntensity == doctest::Approx(0.5f));
        CHECK(quarter.ambientOcclusionRadius == doctest::Approx(0.4f));
        CHECK(quarter.ambientOcclusionBias == doctest::Approx(2.0f));
        CHECK(quarter.focusDistance == doctest::Approx(5.0f));
        CHECK(quarter.focusRange == doctest::Approx(1.0f));
        CHECK(quarter.focusTransition == doctest::Approx(3.0f));
        CHECK(quarter.nearBlur == doctest::Approx(0.25f));
        CHECK(quarter.farBlur == doctest::Approx(0.75f));
        CHECK(quarter.blurSize == doctest::Approx(0.015f));
        CHECK(quarter.exposureMode == ExposureMode::Manual);
        CHECK(quarter.toneMapper == ToneMapper::Filmic);

        // Half way, what cannot be blended is the new one.
        const PostProcessSettings half = blend(from, to, 0.5f);
        CHECK(half.exposure == doctest::Approx(0.0f));
        CHECK(half.exposureMode == ExposureMode::Automatic);
        CHECK(half.toneMapper == ToneMapper::Aces);

        const PostProcessSettings end = blend(from, to, 1.0f);
        CHECK(end.exposure == 1.0f);
        CHECK(end.bloomIntensity == 1.0f);
        CHECK(end.lift == to.lift);
        CHECK(end.toneMapper == ToneMapper::Aces);

        // Amounts outside the way are its ends; no number is the start.
        CHECK(blend(from, to, -3.0f).exposure == -1.0f);
        CHECK(blend(from, to, 7.0f).exposure == 1.0f);
        CHECK(blend(from, to, std::numeric_limits<float>::quiet_NaN()).exposure == -1.0f);
    }

    TEST_CASE("a lookup table fades in and out rather than jumping") {
        const Uuid first = idOf("11111111-2222-4333-8444-555555555555");
        const Uuid second = idOf("66666666-7777-4888-8999-aaaaaaaaaaaa");

        PostProcessSettings none;
        PostProcessSettings graded;
        graded.colorLut = first;
        graded.colorLutIntensity = 0.8f;

        // From no table to one: it is there from the start, and fades in.
        CHECK(blend(none, graded, 0.0f).colorLutIntensity == 0.0f);
        CHECK(blend(none, graded, 0.25f).colorLut == first);
        CHECK(blend(none, graded, 0.25f).colorLutIntensity == doctest::Approx(0.2f));
        CHECK(blend(none, graded, 1.0f).colorLutIntensity == doctest::Approx(0.8f));
        // From one to none: it stays to the end, and fades out.
        CHECK(blend(graded, none, 0.75f).colorLut == first);
        CHECK(blend(graded, none, 0.75f).colorLutIntensity == doctest::Approx(0.2f));
        CHECK(blend(graded, none, 1.0f).colorLutIntensity == doctest::Approx(0.0f));

        // The same table on both sides: its intensity blends.
        PostProcessSettings weaker = graded;
        weaker.colorLutIntensity = 0.4f;
        CHECK(blend(graded, weaker, 0.5f).colorLut == first);
        CHECK(blend(graded, weaker, 0.5f).colorLutIntensity == doctest::Approx(0.6f));

        // Two tables: the one fades out to the middle, the other in from there.
        PostProcessSettings other;
        other.colorLut = second;
        other.colorLutIntensity = 1.0f;
        CHECK(blend(graded, other, 0.25f).colorLut == first);
        CHECK(blend(graded, other, 0.25f).colorLutIntensity == doctest::Approx(0.4f));
        CHECK(blend(graded, other, 0.5f).colorLut == second);
        CHECK(blend(graded, other, 0.5f).colorLutIntensity == doctest::Approx(0.0f));
        CHECK(blend(graded, other, 0.75f).colorLut == second);
        CHECK(blend(graded, other, 0.75f).colorLutIntensity == doctest::Approx(0.5f));
        CHECK(blend(graded, other, 1.0f).colorLutIntensity == doctest::Approx(1.0f));
    }
}
