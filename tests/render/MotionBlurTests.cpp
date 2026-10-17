#include "cordite/renderer/MotionBlur.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::displayed;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels wide and high, and show this many metres of the
/// ground, seen from straight above: 32 pixels a metre. The longest blur is seven pixels
/// either way (motionBlurRadius()).
constexpr uint32_t kSize = 256;
constexpr float kSeen = 8.0f;
constexpr float kPixelsPerMetre = static_cast<float>(kSize) / kSeen;
/// How high above the ground the camera is.
constexpr float kEyeHeight = 8.0f;

/// Settings that leave the light as it is - no curve, no bloom - and blur what moves by
/// `amount`.
PostProcessSettings blurredBy(float amount) {
    PostProcessSettings look;
    look.toneMapper = ToneMapper::Linear;
    look.bloomIntensity = 0.0f;
    look.motionBlur = amount;
    return look;
}

/// A black background, and the picture finished with `look` whatever the world says -
/// with its edges left as they are, which anti-aliasing would change.
SceneRenderSettings finishedWith(const PostProcessSettings& look,
                                 PostProcessQuality quality = PostProcessQuality::Medium) {
    SceneRenderSettings settings;
    settings.backgroundColor = Vec3(0.0f);
    settings.antiAliasing = AntiAliasing::Off;
    settings.postProcess = look;
    settings.postProcessQuality = quality;
    return settings;
}

/// A black ground seen from straight above, and on it, a metre up, a white bar half a
/// metre wide - sixteen pixels - from the top of the picture to its bottom.
struct Stage {
    Stage() {
        scene.writeFlatMaterial("white", Vec3(1.0f));
        scene.writeFlatMaterial("black", Vec3(0.0f));
        scene.open();
        ground = scene.add(BuiltinMesh::Plane, scene.material("black"), at(Vec3(0.0f), Vec3(80.0f, 1.0f, 80.0f)));
        bar =
            scene.add(BuiltinMesh::Plane, scene.material("white"), at(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.5f, 1.0f, 40.0f)));
    }

    /// Looks straight down from above the point (x, 0, 0), with -Z up in the picture.
    [[nodiscard]] CameraView view(float x = 0.0f) const {
        return scene.orthographicView(Vec3(x, kEyeHeight, 0.0f), Vec3(x, 0.0f, 0.0f), kSeen, kWorldForward);
    }

    /// White stripes a centimetre above the ground, two pixels wide and six apart, across
    /// all of it: what a blur that runs across them makes grey.
    void stripes() {
        for (int stripe = -64; stripe < 64; ++stripe) {
            scene.add(
                BuiltinMesh::Plane, scene.material("white"),
                at(Vec3((static_cast<float>(stripe) + 0.5f) * 0.25f, 0.01f, 0.0f), Vec3(1.0f / 16.0f, 1.0f, 80.0f)));
        }
    }

    Image render(const PostProcessSettings& look, float cameraX = 0.0f,
                 PostProcessQuality quality = PostProcessQuality::Medium) {
        return scene.render(view(cameraX), finishedWith(look, quality));
    }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene{UVec2(kSize)};
    Entity ground;
    Entity bar;
};

/// By how many of 255 steps two pictures differ at the most.
uint32_t mostDifference(const Image& first, const Image& second) {
    return unwrap(compareImages(first, second)).maxChannelDifference;
}

/// What motion blur makes of a picture of kSize pixels a side, by the arithmetic of
/// MotionBlur.h: the light, the depth and the motion vector of every pixel in, the light
/// out. The steps of MotionBlurPass, one after another, on the CPU.
struct Reference {
    std::vector<float> light;
    std::vector<float> depth;
    std::vector<Vec2> motion;
    float amount = 1.0f;
    uint32_t samples = 12;

    [[nodiscard]] size_t at(int x, int y) const {
        const int last = static_cast<int>(kSize) - 1;
        return static_cast<size_t>(std::clamp(y, 0, last)) * kSize + static_cast<size_t>(std::clamp(x, 0, last));
    }

    [[nodiscard]] Vec2 blurAt(int x, int y) const {
        return blurOf(motion[at(x, y)], Vec2(static_cast<float>(kSize)), amount,
                      static_cast<float>(motionBlurRadius(kSize)));
    }

    [[nodiscard]] std::vector<float> blurred() const {
        const auto radius = static_cast<int>(motionBlurRadius(kSize));
        const int tiles = (static_cast<int>(kSize) + radius - 1) / radius;
        const auto longerOf = [](Vec2 first, Vec2 second) {
            return glm::dot(second, second) > glm::dot(first, first) ? second : first;
        };
        std::vector<Vec2> longest(static_cast<size_t>(tiles * tiles), Vec2(0.0f));
        for (int y = 0; y < static_cast<int>(kSize); ++y) {
            for (int x = 0; x < static_cast<int>(kSize); ++x) {
                Vec2& tile = longest[static_cast<size_t>((y / radius) * tiles + x / radius)];
                tile = longerOf(tile, blurAt(x, y));
            }
        }
        std::vector<Vec2> neighbours(longest.size(), Vec2(0.0f));
        for (int y = 0; y < tiles; ++y) {
            for (int x = 0; x < tiles; ++x) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int otherX = std::clamp(x + dx, 0, tiles - 1);
                        const int otherY = std::clamp(y + dy, 0, tiles - 1);
                        Vec2& tile = neighbours[static_cast<size_t>(y * tiles + x)];
                        tile = longerOf(tile, longest[static_cast<size_t>(otherY * tiles + otherX)]);
                    }
                }
            }
        }
        std::vector<float> result(light.size());
        for (int y = 0; y < static_cast<int>(kSize); ++y) {
            for (int x = 0; x < static_cast<int>(kSize); ++x) {
                const Vec2 along = neighbours[static_cast<size_t>((y / radius) * tiles + x / radius)];
                if (glm::dot(along, along) < 0.25f) {
                    result[at(x, y)] = light[at(x, y)];
                    continue;
                }
                const MotionBlurPoint pixel{.speed = std::max(glm::length(blurAt(x, y)), 0.5f),
                                            .depth = depth[at(x, y)]};
                float weight = 1.0f / pixel.speed;
                float sum = light[at(x, y)] * weight;
                for (uint32_t index = 0; index < samples; ++index) {
                    const Vec2 offset =
                        along *
                        motionBlurSampleAt(index, samples, UVec2(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                    const Vec2 other = glm::floor(Vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f + offset);
                    const int otherX = static_cast<int>(other.x);
                    const int otherY = static_cast<int>(other.y);
                    const float sampleWeight =
                        motionBlurWeight({.distance = glm::length(offset),
                                          .speed = std::max(glm::length(blurAt(otherX, otherY)), 0.5f),
                                          .depth = depth[at(otherX, otherY)]},
                                         pixel);
                    weight += sampleWeight;
                    sum += light[at(otherX, otherY)] * sampleWeight;
                }
                result[at(x, y)] = sum / weight;
            }
        }
        return result;
    }
};

/// The pixels of the picture whose middles are within [left, right) metres of the middle,
/// across: what a plane between those draws.
bool covers(uint32_t x, float left, float right) {
    const float metres = (static_cast<float>(x) + 0.5f - static_cast<float>(kSize) * 0.5f) / kPixelsPerMetre;
    return metres >= left && metres < right;
}

/// Draws the bar, moves it a quarter of a metre to the right - eight pixels - and draws it
/// again, blurred by `amount` at `quality`; and checks that picture against what the
/// arithmetic of MotionBlur.h makes of the same.
void checkAgainstReference(float amount, PostProcessQuality quality) {
    CAPTURE(amount);
    CAPTURE(quality);
    Stage stage;
    static_cast<void>(stage.render(blurredBy(amount), 0.0f, quality));
    stage.scene.world.transform(stage.bar).position.x = 0.25f;
    const Image image = stage.render(blurredBy(amount), 0.0f, quality);

    Reference reference;
    reference.amount = amount;
    reference.samples = postProcessSettingsFor(quality).motionBlurSamples;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const bool onBar = covers(x, 0.0f, 0.5f);
            reference.light.push_back(onBar ? 1.0f : 0.0f);
            reference.depth.push_back(onBar ? kEyeHeight - 1.0f : kEyeHeight);
            reference.motion.push_back(onBar ? Vec2(8.0f / static_cast<float>(kSize), 0.0f) : Vec2(0.0f));
        }
    }
    const std::vector<float> expected = reference.blurred();
    // The noise that spreads the points along a blur is worked out in floats on both
    // sides, which do not always round alike: a point can fall on the pixel beside the one
    // the other side took. Nearly all pixels agree within the dither, and none by much
    // more.
    uint32_t differing = 0;
    float worst = 0.0f;
    uint32_t smeared = 0;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const float shown = image.pixel(x, y).r;
            const float wanted = displayed(Vec3(expected[reference.at(static_cast<int>(x), static_cast<int>(y))])).r;
            const float difference = std::abs(shown - wanted);
            worst = std::max(worst, difference);
            differing += difference > 2.5f / 255.0f ? 1u : 0u;
            smeared += shown > 4.0f / 255.0f && shown < 251.0f / 255.0f ? 1u : 0u;
        }
    }
    CHECK(differing < kSize * kSize / 100);
    CHECK(worst < 24.0f / 255.0f);
    // And there is a smear: about either edge of the bar, `amount` times four pixels
    // either way, every row is neither black nor white.
    CHECK(smeared >= static_cast<uint32_t>(static_cast<float>(kSize) * 2.0f * 4.0f * amount));
    checkNoValidationIssues(*stage.scene.context.graphics);
}

} // namespace

TEST_SUITE("MotionBlur") {
    TEST_CASE("a picture in which nothing moved is left exactly as it was, and blurring costs three passes") {
        Stage stage;
        // The first picture of a view has nothing before it: nothing moved, nothing to do.
        const Image first = stage.render(blurredBy(1.0f));
        const uint32_t passesWithout = stage.statistics().screenPasses;
        CHECK(stage.statistics().motionBlurBytes == 0);
        // The next has, and nothing in it moved: three passes, and the picture to the bit.
        CHECK(mostDifference(stage.render(blurredBy(1.0f)), first) == 0);
        CHECK(stage.statistics().screenPasses == passesWithout + 3);
        // A copy of the picture, and two images of a texel for every tile of seven pixels a
        // side.
        const uint64_t tiles = (kSize + 6) / 7;
        CHECK(stage.statistics().motionBlurBytes == uint64_t{kSize} * kSize * 8 + tiles * tiles * 4 * 2);

        // A view that has none any more gives the images back.
        CHECK(mostDifference(stage.render(blurredBy(0.0f)), first) == 0);
        CHECK(stage.statistics().screenPasses == passesWithout);
        CHECK(stage.statistics().motionBlurBytes == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what moves is smeared along the way it went, as the reconstruction filter says") {
        checkAgainstReference(1.0f, PostProcessQuality::Medium);
        checkAgainstReference(0.5f, PostProcessQuality::Medium);
    }

    TEST_CASE("the quality says how many points a pixel is blurred from") {
        checkAgainstReference(1.0f, PostProcessQuality::Low);
        checkAgainstReference(1.0f, PostProcessQuality::High);
        // Off finishes nothing, and blurs nothing.
        Stage stage;
        static_cast<void>(stage.render(blurredBy(1.0f), 0.0f, PostProcessQuality::Off));
        stage.scene.world.transform(stage.bar).position.x = 0.25f;
        const Image image = stage.render(blurredBy(1.0f), 0.0f, PostProcessQuality::Off);
        CHECK(image.pixel(126, 100).r == 0.0f);
        CHECK(image.pixel(146, 100).r == 0.0f);
        CHECK(stage.statistics().motionBlurBytes == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a blur is never longer than a fortieth of the picture either way") {
        Stage stage;
        static_cast<void>(stage.render(blurredBy(1.0f)));
        // Two metres - 64 pixels - in one picture.
        stage.scene.world.transform(stage.bar).position.x = 2.0f;
        const Image image = stage.render(blurredBy(1.0f));
        // The bar now covers the pixels 184 to 199; seven pixels beyond either edge, and a
        // little more, nothing reaches.
        for (uint32_t y = 16; y < kSize; y += 32) {
            CAPTURE(y);
            CHECK(image.pixel(184 - 9, y).r == 0.0f);
            CHECK(image.pixel(199 + 9, y).r == 0.0f);
            CHECK(image.pixel(191, y).r > 0.0f);
            // Within reach of the blur, the bar's light is spread out.
            CHECK(image.pixel(184 - 3, y).r > 0.0f);
            CHECK(image.pixel(199 + 3, y).r > 0.0f);
        }
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("what the camera follows stays sharp, and what it passes is blurred") {
        Stage stage;
        stage.stripes();
        static_cast<void>(stage.render(blurredBy(1.0f)));
        // Bar and camera a quarter of a metre to the right.
        stage.scene.world.transform(stage.bar).position.x = 0.25f;
        const Image image = stage.render(blurredBy(1.0f), 0.25f);
        const Image sharp = stage.render(blurredBy(0.0f), 0.25f);
        for (uint32_t y = 8; y < kSize; y += 40) {
            CAPTURE(y);
            // The bar, as it is.
            for (uint32_t x = 120; x < 136; ++x) {
                CHECK(image.pixel(x, y) == sharp.pixel(x, y));
            }
            // The stripes further out: no longer black and white.
            float darkest = 1.0f;
            float brightest = 0.0f;
            for (uint32_t x = 16; x < 96; ++x) {
                darkest = std::min(darkest, image.pixel(x, y).r);
                brightest = std::max(brightest, image.pixel(x, y).r);
            }
            CHECK(darkest > 0.0f);
            CHECK(brightest < 1.0f);
            CHECK(mostDifference(sharp, image) > 0);
        }
        checkNoValidationIssues(*stage.scene.context.graphics);
    }

    TEST_CASE("a picture after a cut is not blurred, nor one of a view that does not ask for it") {
        Stage stage;
        stage.stripes();
        static_cast<void>(stage.render(blurredBy(1.0f)));
        const uint32_t passes = stage.statistics().screenPasses;
        // Eleven metres in one picture is another shot.
        const Image cut = stage.render(blurredBy(1.0f), 11.0f);
        CHECK(stage.statistics().screenPasses == passes);
        CHECK(mostDifference(cut, stage.render(blurredBy(0.0f), 11.0f)) == 0);

        // A view whose settings say no, however the scene asks.
        SceneRenderSettings settings = finishedWith(blurredBy(1.0f));
        settings.motionBlur = false;
        static_cast<void>(stage.scene.render(stage.view(11.0f), settings));
        const Image still = stage.scene.render(stage.view(11.5f), settings);
        CHECK(stage.statistics().motionBlurBytes == 0);
        CHECK(mostDifference(still, stage.render(blurredBy(0.0f), 11.5f)) == 0);
        checkNoValidationIssues(*stage.scene.context.graphics);
    }
}
