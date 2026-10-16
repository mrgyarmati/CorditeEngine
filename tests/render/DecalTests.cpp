#include "cordite/core/Log.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/DecalPass.h"
#include "cordite/renderer/Decals.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/SceneRenderer.h"

#include "render/SceneTestSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::checkShown;
using cordite::test::kBlue;
using cordite::test::kGreen;
using cordite::test::kRed;
using cordite::test::LogCapture;
using cordite::test::makeChecker;
using cordite::test::SceneFixture;

namespace {

/// The pictures are this many pixels wide and high. Seen from straight above, they show
/// four metres of the ground: 16 pixels a metre, the middle at pixel 32, +X to the right
/// and -Z up.
constexpr uint32_t kSize = 64;

/// The color of the ground.
constexpr Vec3 kGround(0.2f, 0.2f, 0.2f);

/// A rotation that turns an entity's -Z axis to point straight down, and its +Y axis to
/// -Z of the world: the top of a decal's image is the top of the picture.
Quat lookingDown() {
    return lookRotation(Vec3(0.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));
}

/// An image of `size` pixels on each side whose every pixel is the normal map's encoding
/// of `normal`: a surface leaning that way.
Image uniformNormalMap(const Vec3& normal) {
    const Vec3 encoded = glm::normalize(normal) * 0.5f + 0.5f;
    return makeChecker(8, 1, Vec4(encoded, 1.0f), Vec4(encoded, 1.0f));
}

/// The normal a normal map leaning towards `mapped` (in the frame of the decal's image)
/// makes of `normal`, when the decal's image runs along `across` and is up along `up`: the
/// frame the decal pass builds around the surface's normal.
Vec3 bent(const Vec3& normal, const Vec3& mapped, const Vec3& across, const Vec3& up) {
    const Vec3 tangent = glm::normalize(across - normal * glm::dot(normal, across));
    Vec3 bitangent = glm::cross(normal, tangent);
    if (glm::dot(bitangent, up) < 0.0f) {
        bitangent = -bitangent;
    }
    const Vec3 unit = glm::normalize(mapped);
    return glm::normalize(tangent * unit.x + bitangent * unit.y + normal * unit.z);
}

/// How the Normals debug view shows a unit normal.
Vec3 shownNormal(const Vec3& normal) {
    return normal * 0.5f + 0.5f;
}

/// The ground, seen from straight above, and decals laid on it.
struct Ground {
    explicit Ground(const RenderResourcesDesc& resources = {}) {
        scene.writeImage("leanRight", uniformNormalMap(Vec3(1.0f, 0.0f, 1.0f)));
        scene.writeImage("leanUp", uniformNormalMap(Vec3(0.0f, 1.0f, 1.0f)));
        // Rows of one pixel, black and white by turns: all one grey where many are seen at
        // once.
        Image stripes = test::unwrap(Image::create(64, 64, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                stripes.setPixel(x, y, y % 2 == 0 ? Vec4(0.0f, 0.0f, 0.0f, 1.0f) : Vec4(1.0f));
            }
        }
        scene.writeImage("stripes", stripes);
        scene.open(resources);

        // Of a known color, roughness, metal and specular: the debug views read what the
        // G-buffer holds.
        scene.writeMaterial(
            "ground", fmt::format(R"("baseColor": [{}, {}, {}, 1], "roughness": 0.8, "metallic": 0, "specular": 1)",
                                  kGround.r, kGround.g, kGround.b));
        scene.writeMaterial("red", R"("baseColor": [1, 0, 0, 0.5], "alphaMode": "blended", "roughness": 0.2, )"
                                   R"("metallic": 1, "specular": 0)");
        scene.writeMaterial("blue", R"("baseColor": [0, 0, 1, 1], "alphaMode": "blended")");
        scene.writeMaterial("faintBlueOpaque", R"("baseColor": [0, 0, 1, 0.25])");
        scene.writeMaterial("halfBlueMaskedOut", R"("baseColor": [0, 0, 1, 0.5], "alphaMode": "masked", )"
                                                 R"("alphaCutoff": 0.6)");
        scene.writeMaterial("halfBlueMaskedIn", R"("baseColor": [0, 0, 1, 0.5], "alphaMode": "masked", )"
                                                R"("alphaCutoff": 0.4)");
        scene.writeMaterial("halfBlueBlended", R"("baseColor": [0, 0, 1, 0.5], "alphaMode": "blended")");
        scene.writeMaterial("leanRight", fmt::format(R"("alphaMode": "blended", "normalTexture": "{}")",
                                                     scene.asset("leanRight.png").toString()));
        scene.writeMaterial("halfLeanUp",
                            fmt::format(R"("baseColor": [1, 1, 1, 0.5], "alphaMode": "blended", "normalTexture": "{}")",
                                        scene.asset("leanUp.png").toString()));
        scene.writeMaterial("stripes",
                            fmt::format(R"("baseColorTexture": "{}")", scene.asset("stripes.png").toString()));
        scene.writeMaterial("glass", R"("baseColor": [0, 0, 0, 1], "emissiveColor": [0, 1, 0], "specular": 0, )"
                                     R"("alphaMode": "blended")");
        REQUIRE(scene.assets->refresh().has_value());

        ground = scene.add(BuiltinMesh::Plane, scene.material("ground"), at(Vec3(0.0f), Vec3(40.0f)));
        view = scene.orthographicView(Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f), 4.0f, Vec3(0.0f, 0.0f, -1.0f));
    }

    /// A decal of `material`, its box `size` metres - across its image, up it and along its
    /// projection - about `center`, projected straight down.
    Entity decal(std::string_view material, const Vec3& center, const Vec3& size, int32_t order = 0) {
        const Entity entity = scene.world.createEntity("Decal");
        scene.world.transform(entity) = at(center, size, lookingDown());
        scene.world.add<DecalComponent>(entity) = {.material = scene.material(material), .order = order};
        return entity;
    }

    Image render(RenderDebugView debugView = RenderDebugView::None) { return render(view, debugView); }

    Image render(const CameraView& camera, RenderDebugView debugView) {
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = debugView;
        return scene.render(camera, settings);
    }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene{UVec2(kSize)};
    Entity ground;
    CameraView view;
};

/// How many pixels of `a` and `b` differ.
uint32_t differences(const Image& a, const Image& b) {
    uint32_t count = 0;
    for (uint32_t y = 0; y < a.height(); ++y) {
        for (uint32_t x = 0; x < a.width(); ++x) {
            count += a.pixel(x, y) == b.pixel(x, y) ? 0 : 1;
        }
    }
    return count;
}

/// The difference between the brightest and the darkest red of the pixels of `image` in
/// columns `left` to `right` and rows `top` to `bottom`, both included; and their middle.
struct Spread {
    float range;
    float middle;
};

Spread spreadOf(const Image& image, uint32_t left, uint32_t right, uint32_t top, uint32_t bottom) {
    float darkest = 1.0f;
    float brightest = 0.0f;
    for (uint32_t y = top; y <= bottom; ++y) {
        for (uint32_t x = left; x <= right; ++x) {
            darkest = std::min(darkest, image.pixel(x, y).r);
            brightest = std::max(brightest, image.pixel(x, y).r);
        }
    }
    return {.range = brightest - darkest, .middle = (brightest + darkest) * 0.5f};
}

} // namespace

TEST_SUITE("Decals") {
    TEST_CASE("a decal lays its color on what is in its box by how much of it there is, and nothing elsewhere") {
        Ground ground;
        const Image before = ground.render(RenderDebugView::BaseColor);
        CHECK(ground.statistics().decals == 0);
        CHECK(ground.statistics().decalBytes == 0);

        // Two metres square, about the middle: pixels 16 to 47 both ways.
        ground.decal("red", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        const Image after = ground.render(RenderDebugView::BaseColor);
        // Half of the red, by the alpha of its color, over the ground.
        for (const auto [x, y] : std::initializer_list<std::pair<uint32_t, uint32_t>>{{32, 32}, {16, 16}, {47, 47}}) {
            checkPixel(after, x, y, glm::mix(kGround, kRed, 0.5f));
        }
        // Outside the box, the ground exactly as it was.
        uint32_t changed = 0;
        for (uint32_t y = 0; y < kSize; ++y) {
            for (uint32_t x = 0; x < kSize; ++x) {
                const bool inside = x >= 16 && x < 48 && y >= 16 && y < 48;
                changed += !inside && after.pixel(x, y) != before.pixel(x, y) ? 1 : 0;
            }
        }
        CHECK(changed == 0);
        CHECK(ground.statistics().decals == 1);
        CHECK(ground.statistics().droppedDecals == 0);
        CHECK(ground.statistics().decalBytes == uint64_t{kSize} * kSize * 4);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a decal lays on its roughness, metal and specular by the same amount, and adds its light") {
        Ground ground;
        const Image occlusionBefore = ground.render(RenderDebugView::Occlusion);
        ground.decal("red", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        checkShown(ground.render(RenderDebugView::Roughness), 32, 32, Vec3(glm::mix(0.8f, 0.2f, 0.5f)));
        checkShown(ground.render(RenderDebugView::Metallic), 32, 32, Vec3(0.5f));
        checkShown(ground.render(RenderDebugView::Specular), 32, 32, Vec3(0.5f));
        checkShown(ground.render(RenderDebugView::Roughness), 4, 4, Vec3(0.8f));
        // The occlusion of the surface is kept.
        CHECK(differences(ground.render(RenderDebugView::Occlusion), occlusionBefore) == 0);

        // A material that glows adds its light, by how much of it there is.
        const Entity glow = ground.decal("glass", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f), 1);
        ground.scene.world.tryGet<DecalComponent>(glow)->opacity = 0.25f;
        const Image emissive = ground.render(RenderDebugView::Emissive);
        CHECK(differences(emissive, ground.render(RenderDebugView::Emissive)) == 0);
        checkPixel(emissive, 32, 32, kGreen * 0.25f);
        checkPixel(emissive, 4, 4, Vec3(0.0f));
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a decal keeps the light channels of what it lands on") {
        Ground ground;
        // The ground is in the second channel, which the sun shines on; another light, in
        // the third, does not reach it.
        ground.scene.world.tryGet<MeshRendererComponent>(ground.ground)->lightChannels = 2;
        const LightComponent sun{.type = LightType::Directional, .intensity = 1.0f, .channels = 2};
        ground.scene.addLight(sun, Vec3(0.0f, 5.0f, 0.0f), glm::normalize(Vec3(-0.3f, -1.0f, -0.2f)));
        const LightComponent other{.type = LightType::Directional, .intensity = 2.0f, .channels = 4};
        ground.scene.addLight(other, Vec3(0.0f, 5.0f, 0.0f), glm::normalize(Vec3(0.2f, -1.0f, 0.4f)));
        const Image lit = ground.render();
        // The same as the ground in all but its color, which a decal of the ground's own
        // material does not change either.
        ground.decal("ground", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        const Image covered = ground.render();
        CHECK(ground.statistics().decals == 1);
        CHECK(Vec3(lit.pixel(32, 32)).r > 0.1f);
        CHECK_NEAR(Vec3(covered.pixel(32, 32)), Vec3(lit.pixel(32, 32)), 1.5f / 255.0f);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("how much of a decal there is goes by the alpha mode of its material, as for a mesh") {
        Ground ground;
        const Entity entity = ground.decal("faintBlueOpaque", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        const std::initializer_list<std::pair<std::string_view, float>> cases = {{"faintBlueOpaque", 1.0f},
                                                                                 {"halfBlueMaskedOut", 0.0f},
                                                                                 {"halfBlueMaskedIn", 1.0f},
                                                                                 {"halfBlueBlended", 0.5f}};
        for (const auto& [material, amount] : cases) {
            CAPTURE(std::string(material));
            ground.scene.world.tryGet<DecalComponent>(entity)->material = ground.scene.material(material);
            checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, glm::mix(kGround, kBlue, amount));
        }
        // The decal's own opacity on top of that.
        ground.scene.world.tryGet<DecalComponent>(entity)->material = ground.scene.material("faintBlueOpaque");
        ground.scene.world.tryGet<DecalComponent>(entity)->opacity = 0.25f;
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, glm::mix(kGround, kBlue, 0.25f));
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("decals of a higher order are laid on over those of a lower, whatever order they were made in") {
        Ground ground;
        const Entity red = ground.decal("red", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f), 1);
        const Entity blue = ground.decal("blue", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f), 0);
        // Blue below, all of it; half of the red over that.
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, glm::mix(kBlue, kRed, 0.5f));
        ground.scene.world.tryGet<DecalComponent>(red)->order = 0;
        ground.scene.world.tryGet<DecalComponent>(blue)->order = 1;
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, kBlue);
        CHECK(ground.statistics().decals == 2);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a surface that turns away from the projection fades out, and one at right angles to it gets nothing") {
        Ground ground;
        // Projected sideways, along -X of the world, onto the ground that faces up.
        const Entity entity = ground.decal("blue", Vec3(0.0f), Vec3(2.0f, 2.0f, 2.0f));
        ground.scene.world.transform(entity).rotation = lookRotation(Vec3(-1.0f, 0.0f, 0.0f));
        ground.scene.world.tryGet<DecalComponent>(entity)->fadeAngle = 90.0f;
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, kGround);

        // Tilted forty-five degrees from straight down: all of it while the fade angle is
        // ten degrees more than that, part of it in between, none at the angle itself.
        const float tilt = glm::radians(45.0f);
        const Vec3 along(-std::sin(tilt), -std::cos(tilt), 0.0f);
        ground.scene.world.transform(entity).rotation = lookRotation(along);
        for (const auto [fadeAngle, roughly] :
             std::initializer_list<std::pair<float, float>>{{60.0f, 1.0f}, {50.0f, 0.5f}, {45.0f, 0.0f}}) {
            CAPTURE(fadeAngle);
            ground.scene.world.tryGet<DecalComponent>(entity)->fadeAngle = fadeAngle;
            const float amount = decalFade(Vec3(0.0f, 1.0f, 0.0f), along, fadeAngle);
            CHECK(std::abs(amount - roughly) < 0.1f);
            checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, glm::mix(kGround, kBlue, amount));
        }
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a camera inside a decal's box sees it, also when the box is mirrored") {
        Ground ground;
        const Entity entity = ground.decal("blue", Vec3(0.0f), Vec3(20.0f, 20.0f, 20.0f));
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, kBlue);
        for (const Vec3 scale : {Vec3(-20.0f, 20.0f, 20.0f), Vec3(20.0f, -20.0f, 20.0f), Vec3(-20.0f, -20.0f, 20.0f)}) {
            CAPTURE(scale.x);
            CAPTURE(scale.y);
            ground.scene.world.transform(entity).scale = scale;
            checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, kBlue);
        }
        // Mirrored along its depth, it is projected up: the ground, which faces up, gets none.
        ground.scene.world.transform(entity).scale = Vec3(20.0f, 20.0f, -20.0f);
        checkPixel(ground.render(RenderDebugView::BaseColor), 32, 32, kGround);
        CHECK(ground.statistics().decals == 1);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("where nothing was drawn there is nothing to lay a decal on") {
        Ground ground;
        ground.scene.world.destroyEntity(ground.ground);
        const Image empty = ground.render(RenderDebugView::BaseColor);
        ground.decal("blue", Vec3(0.0f), Vec3(20.0f, 20.0f, 20.0f));
        CHECK(differences(ground.render(RenderDebugView::BaseColor), empty) == 0);
        // It was drawn, and found nothing.
        CHECK(ground.statistics().decals == 1);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("translucent surfaces are drawn after the decals and get none") {
        Ground ground;
        // Green glass that hides what is behind it, a metre above the ground and inside the
        // box of a blue decal.
        ground.scene.add(BuiltinMesh::Plane, ground.scene.material("glass"), at(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f)));
        ground.decal("blue", Vec3(0.0f, 0.5f, 0.0f), Vec3(2.0f, 2.0f, 2.0f));
        const Image picture = ground.render();
        CHECK(ground.statistics().decals == 1);
        checkPixel(picture, 32, 32, kGreen);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a decal's normal map bends the normal of what it lands on in the frame of its image") {
        Ground ground;
        const Vec3 up(0.0f, 1.0f, 0.0f);
        const Entity entity = ground.decal("leanRight", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        // Its image runs along +X and is up along -Z: leaning right is leaning towards +X.
        checkShown(ground.render(RenderDebugView::Normals), 32, 32,
                   shownNormal(glm::normalize(Vec3(1.0f, 1.0f, 0.0f))));
        checkShown(ground.render(RenderDebugView::Normals), 4, 4, shownNormal(up));
        // Mirrored across, the image runs along -X, and so does the lean.
        ground.scene.world.transform(entity).scale = Vec3(-2.0f, 2.0f, 1.0f);
        checkShown(ground.render(RenderDebugView::Normals), 32, 32,
                   shownNormal(glm::normalize(Vec3(-1.0f, 1.0f, 0.0f))));
        // Leaning up its image is leaning towards -Z, half of it.
        ground.scene.world.transform(entity).scale = Vec3(2.0f, 2.0f, 1.0f);
        ground.scene.world.tryGet<DecalComponent>(entity)->material = ground.scene.material("halfLeanUp");
        const Vec3 half = glm::normalize(glm::mix(up, glm::normalize(Vec3(0.0f, 1.0f, -1.0f)), 0.5f));
        checkShown(ground.render(RenderDebugView::Normals), 32, 32, shownNormal(half));
        // Mirrored up and down, the image is up along +Z, and so is the lean.
        ground.scene.world.transform(entity).scale = Vec3(2.0f, -2.0f, 1.0f);
        const Vec3 mirrored = glm::normalize(glm::mix(up, glm::normalize(Vec3(0.0f, 1.0f, 1.0f)), 0.5f));
        checkShown(ground.render(RenderDebugView::Normals), 32, 32, shownNormal(mirrored));
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a decal bends the normals the decals before it left, and one without a normal map leaves them") {
        Ground ground;
        const Vec3 up(0.0f, 1.0f, 0.0f);
        const Vec3 across(1.0f, 0.0f, 0.0f);
        const Vec3 imageUp(0.0f, 0.0f, -1.0f);
        ground.decal("leanRight", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f), 0);
        const Entity second = ground.decal("halfLeanUp", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f), 1);
        const Vec3 first = bent(up, Vec3(1.0f, 0.0f, 1.0f), across, imageUp);
        const Vec3 both = glm::normalize(glm::mix(first, bent(first, Vec3(0.0f, 1.0f, 1.0f), across, imageUp), 0.5f));
        checkShown(ground.render(RenderDebugView::Normals), 32, 32, shownNormal(both));
        // Once before the first, once more before the second, which comes to where the first
        // bent them.
        CHECK(ground.statistics().decalNormalCopies == 2);

        // Half over the first: where both are, the second bends what the first left, and
        // the ground's own normal where it is alone.
        ground.scene.world.transform(second).position = Vec3(1.5f, 0.0f, 0.0f);
        const Image half = ground.render(RenderDebugView::Normals);
        checkShown(half, 20, 32, shownNormal(first));
        checkShown(half, 44, 32, shownNormal(both));
        checkShown(half, 52, 32,
                   shownNormal(glm::normalize(glm::mix(up, bent(up, Vec3(0.0f, 1.0f, 1.0f), across, imageUp), 0.5f))));
        CHECK(ground.statistics().decalNormalCopies == 2);

        // Apart, the normals are copied once.
        ground.scene.world.transform(second).position = Vec3(3.0f, 0.0f, 0.0f);
        static_cast<void>(ground.render(RenderDebugView::Normals));
        CHECK(ground.statistics().decals == 2);
        CHECK(ground.statistics().decalNormalCopies == 1);

        // Blue over the first, without a normal map: the normals stay as the first left them.
        ground.scene.world.transform(second).position = Vec3(0.0f);
        ground.scene.world.tryGet<DecalComponent>(second)->material = ground.scene.material("blue");
        checkShown(ground.render(RenderDebugView::Normals), 32, 32, shownNormal(first));
        CHECK(ground.statistics().decalNormalCopies == 2);
        // Under it, it bends nothing that the first comes to: one copy does.
        ground.scene.world.tryGet<DecalComponent>(second)->order = -1;
        checkShown(ground.render(RenderDebugView::Normals), 32, 32, shownNormal(first));
        CHECK(ground.statistics().decalNormalCopies == 1);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("a decal whose material cannot be loaded lays nothing, and is reported once") {
        Ground ground;
        const Image before = ground.render(RenderDebugView::BaseColor);
        const Entity entity = ground.decal("blue", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        ground.scene.world.tryGet<DecalComponent>(entity)->material = Uuid::generate();
        LogCapture capture;
        CHECK(differences(ground.render(RenderDebugView::BaseColor), before) == 0);
        CHECK(differences(ground.render(RenderDebugView::BaseColor), before) == 0);
        CHECK(ground.statistics().decals == 0);
        CHECK(ground.statistics().decalBytes == 0);
        CHECK(capture.count(LogLevel::Error) == 1);
        CHECK(capture.contains("decals not at all"));
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("more decals than a view can hold are left out, counted and reported once") {
        Ground ground;
        const LogCapture log;
        // Small ones side by side, of the lowest orders first: those of the highest orders
        // are left out.
        for (uint32_t index = 0; index < kMaxDecals + 10; ++index) {
            const Vec3 place(static_cast<float>(index % 32) * 0.1f - 1.6f, 0.0f,
                             static_cast<float>(index / 32) * 0.1f - 1.6f);
            ground.decal("blue", place, Vec3(0.05f, 0.05f, 0.2f), static_cast<int32_t>(index));
        }
        static_cast<void>(ground.render(RenderDebugView::BaseColor));
        CHECK(ground.statistics().decals == kMaxDecals);
        CHECK(ground.statistics().droppedDecals == 10);
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("too many decals for one view: 10 left out"));

        // Not again every frame.
        static_cast<void>(ground.render(RenderDebugView::BaseColor));
        static_cast<void>(ground.render(RenderDebugView::BaseColor));
        CHECK(log.count(LogLevel::Warn) == 1);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("decals out of view are not drawn, and give back what they read") {
        Ground ground;
        const Entity entity = ground.decal("blue", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));
        static_cast<void>(ground.render());
        CHECK(ground.statistics().decals == 1);
        CHECK(ground.statistics().decalBytes > 0);
        ground.scene.world.transform(entity).position = Vec3(50.0f, 0.0f, 0.0f);
        static_cast<void>(ground.render());
        CHECK(ground.statistics().decals == 0);
        CHECK(ground.statistics().decalBytes == 0);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }

    TEST_CASE("an image seen at a slant is read from the levels as small as a pixel spans of it") {
        // Without anisotropic filtering, the level read is the one whose texels are as large
        // as the longer side of a pixel's footprint: at a slant, the way the surface runs
        // away. That is many rows of the stripes, which all of its levels but the first
        // average to one grey; the first, read where it is not, shows black and white.
        Ground ground({.maxAnisotropy = 1.0f});
        ground.decal("stripes", Vec3(0.0f), Vec3(2.0f, 2.0f, 1.0f));

        // Five degrees above the ground, without perspective: half a metre of the picture
        // is the two metres of the decal, sixty-four rows of it per pixel.
        const float elevation = glm::radians(5.0f);
        const Vec3 eye = Vec3(0.0f, std::sin(elevation), std::cos(elevation)) * 10.0f;
        const Spread flat =
            spreadOf(ground.render(ground.scene.orthographicView(eye, Vec3(0.0f), 0.5f), RenderDebugView::BaseColor), 8,
                     56, 26, 38);
        CHECK(flat.range < 0.05f);
        CHECK(flat.middle > 0.3f);
        CHECK(flat.middle < 0.8f);

        // The same with perspective, from twenty centimetres above the ground, three metres
        // away.
        const Spread deep =
            spreadOf(ground.render(ground.scene.perspectiveView(Vec3(0.0f, 0.2f, 3.0f), Vec3(0.0f), 30.0f),
                                   RenderDebugView::BaseColor),
                     20, 44, 31, 35);
        CHECK(deep.range < 0.05f);
        CHECK(deep.middle > 0.3f);
        CHECK(deep.middle < 0.8f);
        checkNoValidationIssues(*ground.scene.context.graphics);
    }
}
