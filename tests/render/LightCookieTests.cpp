#include "cordite/assets/Image.h"
#include "cordite/core/Log.h"
#include "cordite/math/Color.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/LightCookies.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdint>
#include <string>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::LogCapture;
using cordite::test::SceneFixture;

namespace {

/// The wall the lights of these tests shine on is in the plane z = 0 and faces +Z; a
/// camera without perspective looks at it head-on, 64 pixels showing four metres.
constexpr float kWallView = 4.0f;
constexpr uint32_t kPictureSize = 64;

/// Where the centre of a pixel of the picture is on the wall.
Vec3 onWall(uint32_t x, uint32_t y) {
    const float metresPerPixel = kWallView / static_cast<float>(kPictureSize);
    const float half = 0.5f * static_cast<float>(kPictureSize);
    return {(static_cast<float>(x) + 0.5f - half) * metresPerPixel,
            (half - (static_cast<float>(y) + 0.5f)) * metresPerPixel, 0.0f};
}

/// An image of 16 by 16 pixels whose four quarters have the given display colors: top
/// left, top right, bottom left, bottom right.
Image quarters(const Vec4& topLeft, const Vec4& topRight, const Vec4& bottomLeft, const Vec4& bottomRight) {
    Image image = test::unwrap(Image::create(16, 16, ImageFormat::Rgba8));
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            image.setPixel(x, y, y < 8 ? (x < 8 ? topLeft : topRight) : (x < 8 ? bottomLeft : bottomRight));
        }
    }
    return image;
}

constexpr Vec4 kWhite(1.0f);
constexpr Vec4 kBlack(0.0f, 0.0f, 0.0f, 1.0f);

/// A white, matte wall that fills the view, and the cookies of the tests: halves - white
/// on the left, black on the right, and the other way round - a checker, a tint and black.
struct CookieWall {
    CookieWall() {
        scene.writeMatteMaterial("matte");
        scene.writeImage("halves", quarters(kWhite, kBlack, kWhite, kBlack));
        scene.writeImage("checker", quarters(kWhite, kBlack, kBlack, kWhite));
        scene.writeImage("tint", quarters(kTint, kTint, kTint, kTint));
        scene.writeImage("black", quarters(kBlack, kBlack, kBlack, kBlack));
        scene.writeImage("otherHalves", quarters(kBlack, kWhite, kBlack, kWhite));
        scene.open();
        // The built-in plane lies flat; stood up, it faces +Z.
        scene.add(BuiltinMesh::Plane, scene.material("matte"),
                  at(Vec3(0.0f), Vec3(8.0f), glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f))));
        view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), kWallView);
    }

    /// A spot light two metres in front of the wall, shining straight at it, with a cone
    /// of forty degrees: a disc of 3.4 metres on the wall.
    Entity addSpot(std::string_view cookie) {
        return scene.addLight({.type = LightType::Spot,
                               .intensity = 2.0f,
                               .range = 100.0f,
                               .innerConeDegrees = 39.0f,
                               .outerConeDegrees = 40.0f,
                               .cookie = cookie.empty() ? Uuid{} : scene.asset(fmt::format("{}.png", cookie))},
                              Vec3(0.0f, 0.0f, 2.0f), Vec3(0.0f, 0.0f, -1.0f));
    }

    Image render() { return scene.render(view); }

    /// The light of a pixel of the picture, as it was worked out.
    static Vec3 lightAt(const Image& image, uint32_t x, uint32_t y) {
        const Vec4 pixel = image.pixel(x, y);
        return {srgbToLinear(pixel.r), srgbToLinear(pixel.g), srgbToLinear(pixel.b)};
    }

    static constexpr Vec4 kTint{1.0f, 0.5f, 0.25f, 1.0f};

    SceneFixture scene{UVec2(kPictureSize, kPictureSize)};
    CameraView view;
};

} // namespace

TEST_SUITE("LightCookies") {
    TEST_CASE("a spot light throws its cookie across its cone, the right way round") {
        CookieWall wall;
        const Entity spot = wall.addSpot("halves");
        Image image = wall.render();
        // Half a metre to the left of the middle the cookie is white; as far to the right,
        // black: nothing of the light gets there.
        CHECK(CookieWall::lightAt(image, 24, 32).r > 0.2f);
        CHECK(image.pixel(40, 32) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        // Up and down it is the same.
        CHECK(CookieWall::lightAt(image, 24, 20).r > 0.2f);
        CHECK(image.pixel(40, 44) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));

        // Where the cookie is white the light is all there, to the bit.
        const Vec4 white = image.pixel(24, 32);
        wall.scene.world.tryGet<LightComponent>(spot)->cookie = Uuid{};
        CHECK(wall.render().pixel(24, 32) == white);

        // Turned half round about the way it shines, the light throws its cookie the
        // other way round.
        wall.scene.world.tryGet<LightComponent>(spot)->cookie = wall.scene.asset("halves.png");
        Transform& placement = wall.scene.world.transform(spot);
        placement.rotation = placement.rotation * glm::angleAxis(kPi, Vec3(0.0f, 0.0f, 1.0f));
        image = wall.render();
        CHECK(image.pixel(24, 32) == Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        CHECK(CookieWall::lightAt(image, 40, 32).r > 0.2f);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a cookie filters the light by its colors") {
        CookieWall wall;
        const Entity spot = wall.addSpot("");
        const Vec3 plain = CookieWall::lightAt(wall.render(), 32, 32);
        REQUIRE(plain.r > 0.2f);
        REQUIRE(plain.r < 0.8f);
        wall.scene.world.tryGet<LightComponent>(spot)->cookie = wall.scene.asset("tint.png");
        const Vec3 tinted = CookieWall::lightAt(wall.render(), 32, 32);
        // The tint is a color of a display; it filters light as the light it stands for.
        const Vec3 filter(srgbToLinear(CookieWall::kTint.r), srgbToLinear(CookieWall::kTint.g),
                          srgbToLinear(CookieWall::kTint.b));
        for (int channel = 0; channel < 3; ++channel) {
            CAPTURE(channel);
            CHECK(std::abs(tinted[channel] - plain[channel] * filter[channel]) < 0.01f);
        }
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("every light shines through a cookie of its own") {
        CookieWall wall;
        // Two lights in one place, through pictures that are each other's negative: each
        // lights its half, and together they light both.
        wall.addSpot("halves");
        wall.addSpot("otherHalves");
        const Image image = wall.render();
        const float left = CookieWall::lightAt(image, 24, 32).r;
        const float right = CookieWall::lightAt(image, 40, 32).r;
        CHECK(left > 0.2f);
        CHECK(right > 0.2f);
        CHECK(std::abs(left - right) < 0.02f);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a cookie seen from afar is read at a level of its own, without shimmer") {
        CookieWall wall;
        // A checker that repeats every centimetre, six times across the sixteenth of a metre
        // a pixel covers: what a pixel gets is the half of the light that comes through it
        // on average, everywhere - not whichever square its middle happens to fall on.
        wall.scene.addLight({.type = LightType::Directional,
                             .intensity = 0.5f,
                             .cookie = wall.scene.asset("checker.png"),
                             .cookieSize = 0.01f},
                            Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
        const Image image = wall.render();
        for (uint32_t y = 4; y < kPictureSize; y += 8) {
            for (uint32_t x = 4; x < kPictureSize; x += 8) {
                CAPTURE(x);
                CAPTURE(y);
                // Half of the light of a white matte wall facing the sun: 0.5 * 0.5.
                CHECK(std::abs(CookieWall::lightAt(image, x, y).r - 0.25f) < 0.05f);
            }
        }
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a light without a cookie is not filtered by the cookie of another") {
        CookieWall wall;
        wall.addSpot("");
        const Image alone = wall.render();
        // Another light, black all over: it adds nothing, and takes nothing away.
        wall.addSpot("black");
        const Image both = wall.render();
        for (const uint32_t x : {16u, 32u, 48u}) {
            CAPTURE(x);
            CHECK(both.pixel(x, 32) == alone.pixel(x, 32));
        }
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a directional light lays its cookie out over the world, again and again, from where its entity is") {
        CookieWall wall;
        LightComponent sun{.type = LightType::Directional,
                           .intensity = 0.5f,
                           .cookie = wall.scene.asset("checker.png"),
                           .cookieSize = 2.0f};
        const Entity entity = wall.scene.addLight(sun, Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
        const auto check = [&](const Image& image) {
            wall.scene.world.updateWorldTransforms();
            SceneLight light = makeSceneLight(sun, wall.scene.world.worldMatrix(entity));
            GpuLight gpu;
            gpu.typeAndChannels = static_cast<uint32_t>(GpuLightType::Directional);
            gpu.position = light.position;
            gpu.direction = light.direction;
            gpu.cookieRight = cookieAxisOf(light);
            // The middles of the squares of the checker, a quarter of a metre either way of
            // the middle of a square of the wall: away from where one square meets another.
            for (const uint32_t y : {4u, 12u, 20u, 28u, 36u, 44u, 52u, 60u}) {
                for (const uint32_t x : {4u, 12u, 20u, 28u, 36u, 44u, 52u, 60u}) {
                    CAPTURE(x);
                    CAPTURE(y);
                    const Vec2 uv = cookieUvOf(gpu, onWall(x, y)).value();
                    const Vec2 within = uv - glm::floor(uv);
                    const bool white = (within.x < 0.5f) == (within.y < 0.5f);
                    if (white) {
                        CHECK(CookieWall::lightAt(image, x, y).r > 0.1f);
                    } else {
                        CHECK(CookieWall::lightAt(image, x, y).r < 0.02f);
                    }
                }
            }
        };
        check(wall.render());
        // A metre along the light's right: the squares go with it.
        const Vec4 before = wall.render().pixel(20, 20);
        wall.scene.world.transform(entity).position = Vec3(1.0f, 0.0f, 0.0f);
        const Image moved = wall.render();
        check(moved);
        CHECK(moved.pixel(20, 20) != before);
        checkNoValidationIssues(*wall.scene.context.graphics);
    }

    TEST_CASE("a cookie is a picture, one of a few: an image of light, and one more, is said once and not used") {
        SceneFixture scene;
        for (uint32_t index = 0; index <= kMaxLightCookies; ++index) {
            scene.writeImage(fmt::format("cookie{}", index), quarters(kWhite, kBlack, kBlack, kWhite));
        }
        Image sky = test::unwrap(Image::create(4, 4, ImageFormat::Rgba32F));
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                sky.setPixel(x, y, Vec4(4.0f, 3.0f, 2.0f, 1.0f));
            }
        }
        scene.writeHdrImage("sky", sky);
        scene.open();
        RenderResources& resources = scene.renderer->resources();
        const uint64_t bytesBefore = resources.statistics().textureBytes;
        const nvrhi::ITexture* none = resources.cookies();
        CHECK(none->getDesc().width == 1);
        CHECK(resources.cookieLayer(Uuid{}) == -1);

        const LogCapture log(LogLevel::Error);
        // The first cookie makes an array of one layer: the memory follows what is used.
        CHECK(resources.cookieLayer(scene.asset("cookie0.png")) == 0);
        CHECK(resources.cookies()->getDesc().arraySize == 1);
        CHECK(resources.statistics().textureBytes - bytesBefore == 349524);
        // An image of light is no cookie, while there is room as much as when there is none.
        CHECK(resources.cookieLayer(scene.asset("sky.hdr")) == -1);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("an image of light"));
        // The array grows, twice as large each time, and keeps what it held.
        for (uint32_t index = 1; index < kMaxLightCookies; ++index) {
            CHECK(resources.cookieLayer(scene.asset(fmt::format("cookie{}.png", index))) ==
                  static_cast<int32_t>(index));
        }
        // Asked again, the same layer.
        CHECK(resources.cookieLayer(scene.asset("cookie3.png")) == 3);
        // Sixteen layers at last, counted once: the smaller arrays are gone. The image of
        // light was said once, and is not tried again.
        CHECK(resources.cookieLayer(scene.asset("sky.hdr")) == -1);
        const nvrhi::TextureDesc& array = resources.cookies()->getDesc();
        CHECK(array.width == kLightCookieSize);
        CHECK(array.arraySize == kMaxLightCookies);
        CHECK(array.mipLevels == 9);
        CHECK(resources.statistics().textureBytes - bytesBefore == uint64_t{349524} * kMaxLightCookies);
        CHECK(log.count(LogLevel::Error) == 1);

        // One more than there is room for.
        const Uuid extra = scene.asset(fmt::format("cookie{}.png", kMaxLightCookies));
        CHECK(resources.cookieLayer(extra) == -1);
        CHECK(log.count(LogLevel::Error) == 2);
        CHECK(log.contains("cookies there is room for are in use"));
        CHECK(resources.cookies()->getDesc().arraySize == kMaxLightCookies);
        CHECK(resources.cookieLayer(extra) == -1);
        CHECK(log.count(LogLevel::Error) == 2);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
