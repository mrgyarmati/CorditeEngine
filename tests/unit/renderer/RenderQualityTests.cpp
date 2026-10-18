#include "cordite/renderer/SceneRenderer.h"

#include "support/TestSupport.h"

using namespace cordite;

TEST_SUITE("RenderQuality") {
    TEST_CASE("qualities have names, and are found by them") {
        CHECK(nameOf(RenderQuality::Low) == "low");
        CHECK(nameOf(RenderQuality::Medium) == "medium");
        CHECK(nameOf(RenderQuality::High) == "high");
        CHECK(displayNameOf(RenderQuality::Low) == "Low");
        CHECK(displayNameOf(RenderQuality::Medium) == "Medium");
        CHECK(displayNameOf(RenderQuality::High) == "High");

        CHECK(kRenderQualities.size() == 3);
        for (const RenderQuality quality : kRenderQualities) {
            CHECK(renderQualityFromName(nameOf(quality)) == quality);
        }
        CHECK(renderQualityFromName("MEDIUM") == RenderQuality::Medium);
        CHECK_FALSE(renderQualityFromName("off").has_value());
        CHECK_FALSE(renderQualityFromName("ultra").has_value());
        CHECK_FALSE(renderQualityFromName("").has_value());
    }

    TEST_CASE("a quality sets the shadows, the screen effects, the finishing of the picture and the levels of detail, "
              "and nothing else") {
        SceneRenderSettings settings;
        settings.backgroundColor = Vec3(0.1f, 0.2f, 0.3f);
        settings.defaultLighting = true;
        settings.debugView = RenderDebugView::Normals;
        settings.deltaSeconds = 0.5f;

        applyRenderQuality(RenderQuality::Low, settings);
        CHECK(settings.shadowQuality == ShadowQuality::Low);
        CHECK(settings.screenEffectsQuality == ScreenEffectsQuality::Low);
        CHECK(settings.postProcessQuality == PostProcessQuality::Low);
        // Coarser levels of detail at a lower quality: errors of two pixels.
        CHECK(settings.lodErrorPixels == 2.0f);
        applyRenderQuality(RenderQuality::High, settings);
        CHECK(settings.shadowQuality == ShadowQuality::High);
        CHECK(settings.screenEffectsQuality == ScreenEffectsQuality::High);
        CHECK(settings.postProcessQuality == PostProcessQuality::High);
        CHECK(settings.lodErrorPixels == 0.5f);
        applyRenderQuality(RenderQuality::Medium, settings);
        CHECK(settings.shadowQuality == ShadowQuality::Medium);
        CHECK(settings.screenEffectsQuality == ScreenEffectsQuality::Medium);
        CHECK(settings.postProcessQuality == PostProcessQuality::Medium);
        CHECK(settings.lodErrorPixels == 1.0f);

        CHECK(settings.backgroundColor == Vec3(0.1f, 0.2f, 0.3f));
        CHECK(settings.defaultLighting);
        CHECK(settings.debugView == RenderDebugView::Normals);
        CHECK(settings.deltaSeconds == 0.5f);
    }

    TEST_CASE("a quality keeps textures up to a size of its own, larger at a higher quality") {
        CHECK(textureLimitFor(RenderQuality::Low) == 1024);
        CHECK(textureLimitFor(RenderQuality::Medium) == 2048);
        CHECK(textureLimitFor(RenderQuality::High) == 4096);
    }

    TEST_CASE("settings say which quality they are at, when they are at one") {
        // As they come, a view is drawn at medium quality.
        SceneRenderSettings settings;
        CHECK(renderQualityOf(settings) == RenderQuality::Medium);
        for (const RenderQuality quality : kRenderQualities) {
            applyRenderQuality(quality, settings);
            CHECK(renderQualityOf(settings) == quality);
        }
        // Set piece by piece to what no quality has: none.
        settings.shadowQuality = ShadowQuality::Low;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        settings.shadowQuality = ShadowQuality::High;
        settings.postProcessQuality = PostProcessQuality::Off;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        settings.shadowQuality = ShadowQuality::Off;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        // The screen effects are a piece like the others.
        applyRenderQuality(RenderQuality::High, settings);
        settings.screenEffectsQuality = ScreenEffectsQuality::Low;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        settings.screenEffectsQuality = ScreenEffectsQuality::Off;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        settings.screenEffectsQuality = ScreenEffectsQuality::High;
        CHECK(renderQualityOf(settings) == RenderQuality::High);
        // And so are the levels of detail.
        settings.lodErrorPixels = 0.0f;
        CHECK_FALSE(renderQualityOf(settings).has_value());
        settings.lodErrorPixels = 0.5f;
        CHECK(renderQualityOf(settings) == RenderQuality::High);
    }
}
