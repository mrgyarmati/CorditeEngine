#include "cordite/renderer/SceneRenderer.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/core/FileSystem.h"
#include "cordite/math/Color.h"
#include "cordite/math/Projection.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/MathTestSupport.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::checkShown;
using cordite::test::displayed;
using cordite::test::kBackground;
using cordite::test::kBlue;
using cordite::test::kGreen;
using cordite::test::kRed;
using cordite::test::LogCapture;
using cordite::test::makeBumpNormalMap;
using cordite::test::makeChecker;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::SceneFixture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

TEST_SUITE("SceneTargets") {
    TEST_CASE("scene targets have the formats and sizes the renderer expects") {
        RenderContext context;
        {
            const SceneTargets targets = unwrap(SceneTargets::create(context.device(), UVec2(320, 180), "View"));
            CHECK(targets.isValid());
            CHECK(targets.size() == UVec2(320, 180));
            CHECK(targets.sceneColor()->getDesc().format == nvrhi::Format::RGBA16_FLOAT);
            CHECK(targets.depth()->getDesc().format == nvrhi::Format::D32);
            CHECK(targets.display()->getDesc().format == nvrhi::Format::RGBA8_UNORM);
            CHECK(targets.sceneColor()->getDesc().width == 320);
            CHECK(targets.display()->getDesc().height == 180);
            CHECK(targets.sceneColor()->getDesc().debugName == "View scene color");

            // The G-buffer, in the layout the base pass and the lighting agree on.
            CHECK(targets.gbuffer(GBufferTarget::BaseColor)->getDesc().format == nvrhi::Format::SRGBA8_UNORM);
            CHECK(targets.gbuffer(GBufferTarget::Normal)->getDesc().format == nvrhi::Format::RG16_UNORM);
            CHECK(targets.gbuffer(GBufferTarget::Parameters)->getDesc().format == nvrhi::Format::RGBA8_UNORM);
            CHECK(targets.gbuffer(GBufferTarget::Emissive)->getDesc().format == nvrhi::Format::R11G11B10_FLOAT);
            CHECK(targets.gbuffer(GBufferTarget::Normal)->getDesc().width == 320);
            CHECK(targets.gbuffer(GBufferTarget::Normal)->getDesc().debugName == "View G-buffer normal");
            const nvrhi::FramebufferDesc& gbuffer = targets.gbufferFramebuffer()->getDesc();
            REQUIRE(gbuffer.colorAttachments.size() == kGBufferTargetCount);
            CHECK(gbuffer.colorAttachments[0].texture == targets.gbuffer(GBufferTarget::BaseColor));
            CHECK(gbuffer.colorAttachments[3].texture == targets.gbuffer(GBufferTarget::Emissive));
            CHECK(gbuffer.depthAttachment.texture == targets.depth());

            // The pre-pass draws depth alone; the lighting reads the depth, so the
            // framebuffer it draws into has none.
            CHECK(targets.depthFramebuffer()->getDesc().colorAttachments.empty());
            CHECK(targets.depthFramebuffer()->getDesc().depthAttachment.texture == targets.depth());
            CHECK(targets.lightingFramebuffer()->getDesc().colorAttachments[0].texture == targets.sceneColor());
            CHECK_FALSE(targets.lightingFramebuffer()->getDesc().depthAttachment.valid());

            // The overlay framebuffer shares the display image and the scene's depth.
            CHECK(targets.overlayFramebuffer()->getDesc().colorAttachments[0].texture == targets.display());
            CHECK(targets.overlayFramebuffer()->getDesc().depthAttachment.texture == targets.depth());
            CHECK(targets.sceneFramebuffer()->getDesc().colorAttachments[0].texture == targets.sceneColor());
            CHECK_FALSE(targets.displayFramebuffer()->getDesc().depthAttachment.valid());
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("an empty set of targets is not valid") {
        const SceneTargets targets;
        CHECK_FALSE(targets.isValid());
        CHECK(targets.size() == UVec2(0, 0));
        CHECK(targets.display() == nullptr);
    }

    TEST_CASE("resizing replaces the textures only when the size changes") {
        RenderContext context;
        {
            SceneTargets targets = unwrap(SceneTargets::create(context.device(), UVec2(64, 32)));
            const nvrhi::TextureHandle before = targets.display();

            REQUIRE(targets.resize(context.device(), UVec2(64, 32)).has_value());
            CHECK(targets.display() == before.Get());

            REQUIRE(targets.resize(context.device(), UVec2(128, 16)).has_value());
            CHECK(targets.display() != before.Get());
            CHECK(targets.size() == UVec2(128, 16));
            CHECK(targets.depth()->getDesc().width == 128);
            // The name survives.
            CHECK(targets.depth()->getDesc().debugName == "Scene depth");

            // A size that cannot be created leaves the targets as they were.
            const Status tooLarge = targets.resize(context.device(), UVec2(100000, 16));
            REQUIRE_FALSE(tooLarge.has_value());
            CHECK(targets.size() == UVec2(128, 16));
            CHECK(targets.isValid());
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("sizes outside what a GPU can create are refused") {
        RenderContext context;
        for (const UVec2 size : {UVec2(0, 16), UVec2(16, 0), UVec2(16385, 16), UVec2(16, 16385)}) {
            const Result<SceneTargets> targets = SceneTargets::create(context.device(), size);
            REQUIRE_FALSE(targets.has_value());
            CHECK(targets.error().code == ErrorCode::InvalidArgument);
        }
    }

    TEST_CASE("an empty set of targets can be resized into existence") {
        RenderContext context;
        {
            SceneTargets targets;
            REQUIRE(targets.resize(context.device(), UVec2(8, 8)).has_value());
            CHECK(targets.isValid());
            CHECK(targets.sceneColor()->getDesc().debugName == "Scene scene color");
        }
        checkNoValidationIssues(*context.graphics);
    }
}

TEST_SUITE("SceneRenderer") {
    TEST_CASE("an empty world shows the background") {
        SceneFixture scene;
        scene.open();
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        CHECK(image.width() == 64);
        checkPixel(image, 0, 0, kBackground);
        checkPixel(image, 32, 32, kBackground);
        CHECK(scene.renderer->statistics().meshRenderers == 0);
        CHECK(scene.renderer->statistics().meshes.drawCalls == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a mesh covers the pixels it should, in the color of its material") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        // A unit cube seen head-on fills the middle half of a view two metres high.
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        checkPixel(image, 32, 32, kRed);
        checkPixel(image, 17, 17, kRed);
        checkPixel(image, 46, 46, kRed);
        checkPixel(image, 14, 32, kBackground);
        checkPixel(image, 49, 32, kBackground);
        checkPixel(image, 32, 14, kBackground);
        checkPixel(image, 32, 49, kBackground);

        const SceneRenderStatistics& statistics = scene.renderer->statistics();
        CHECK(statistics.meshRenderers == 1);
        CHECK(statistics.culled == 0);
        // A solid surface is drawn twice: its depth first, then what it is made of.
        CHECK(statistics.depthPass == MeshPassStatistics{.drawCalls = 1, .instances = 1, .triangles = 12});
        CHECK(statistics.basePass == statistics.depthPass);
        CHECK(statistics.forwardPass == MeshPassStatistics{});
        CHECK(statistics.meshes == MeshPassStatistics{.drawCalls = 2, .instances = 2, .triangles = 24});
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the image is upright and not mirrored") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.writeFlatMaterial("green", kGreen);
        scene.writeFlatMaterial("blue", kBlue);
        scene.open();
        // Seen from +Z: red to the right (+X), green above (+Y), blue in the middle.
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.6f, 0.0f, 0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("green"), at(Vec3(0.0f, 0.6f, 0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("blue"), at(Vec3(0.0f), Vec3(0.3f)));
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        checkPixel(image, 32, 32, kBlue);
        checkPixel(image, 51, 32, kRed);
        checkPixel(image, 13, 32, kBackground);
        checkPixel(image, 32, 13, kGreen);
        checkPixel(image, 32, 51, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("nearer surfaces hide farther ones, whatever the order they are drawn in") {
        for (const bool nearFirst : {true, false}) {
            CAPTURE(nearFirst);
            SceneFixture scene;
            scene.writeFlatMaterial("red", kRed);
            scene.writeFlatMaterial("blue", kBlue);
            scene.open();
            const Transform nearPlace = at(Vec3(0.0f, 0.0f, 1.0f), Vec3(0.5f));
            const Transform farPlace = at(Vec3(0.0f, 0.0f, -1.0f), Vec3(1.5f));
            if (nearFirst) {
                scene.add(BuiltinMesh::Cube, scene.material("red"), nearPlace);
                scene.add(BuiltinMesh::Cube, scene.material("blue"), farPlace);
            } else {
                scene.add(BuiltinMesh::Cube, scene.material("blue"), farPlace);
                scene.add(BuiltinMesh::Cube, scene.material("red"), nearPlace);
            }
            const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

            // The small red cube is in front of the large blue one.
            checkPixel(image, 32, 32, kRed);
            checkPixel(image, 32, 12, kBlue);
            checkPixel(image, 10, 32, kBlue);
            checkNoValidationIssues(*scene.context.graphics);
        }
    }

    TEST_CASE("surfaces are seen from their front only, unless the material is double-sided") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.writeFlatMaterial("green", kGreen, R"("doubleSided": true)");
        scene.open();
        // The built-in plane faces up. One half of the view shows a one-sided plane,
        // the other a double-sided one.
        scene.add(BuiltinMesh::Plane, scene.material("red"), at(Vec3(-0.5f, 0.0f, 0.0f), Vec3(0.8f)));
        scene.add(BuiltinMesh::Plane, scene.material("green"), at(Vec3(0.5f, 0.0f, 0.0f), Vec3(0.8f)));

        SUBCASE("from above, both are visible") {
            const Image image =
                scene.render(scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f)));
            checkPixel(image, 16, 32, kRed);
            checkPixel(image, 48, 32, kGreen);
        }
        SUBCASE("from below, only the double-sided one") {
            // Looking up, with +Z at the top of the image, +X is still to the right.
            const Image image =
                scene.render(scene.orthographicView(Vec3(0.0f, -5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, 1.0f)));
            checkPixel(image, 16, 32, kBackground);
            checkPixel(image, 48, 32, kGreen);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a mirrored instance still shows its front") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        // Scaling by -1 along one axis turns every triangle the other way round. The
        // cube must still be seen from outside: drawn inside out, its far faces would
        // show, which here would look the same - so a plane is used as well.
        scene.add(BuiltinMesh::Plane, scene.material("red"), at(Vec3(0.0f), Vec3(-1.0f, 1.0f, 1.0f)));
        const Image above =
            scene.render(scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f)));
        checkPixel(above, 32, 32, kRed);

        const Image below =
            scene.render(scene.orthographicView(Vec3(0.0f, -5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, 1.0f)));
        checkPixel(below, 32, 32, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("equal draws become one instanced draw call") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.writeFlatMaterial("blue", kBlue);
        scene.open();
        for (int index = 0; index < 5; ++index) {
            scene.add(BuiltinMesh::Cube, scene.material("red"),
                      at(Vec3(-0.8f + 0.4f * static_cast<float>(index), 0.5f, 0.0f), Vec3(0.2f)));
        }
        for (int index = 0; index < 3; ++index) {
            scene.add(BuiltinMesh::Sphere, scene.material("red"),
                      at(Vec3(-0.4f + 0.4f * static_cast<float>(index), -0.5f, 0.0f), Vec3(0.2f)));
        }
        scene.add(BuiltinMesh::Cube, scene.material("blue"), at(Vec3(0.0f), Vec3(0.2f)));
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        // Red cubes, red spheres, one blue cube.
        const SceneRenderStatistics& statistics = scene.renderer->statistics();
        CHECK(statistics.meshRenderers == 9);
        CHECK(statistics.basePass.drawCalls == 3);
        CHECK(statistics.basePass.instances == 9);
        CHECK(statistics.depthPass == statistics.basePass);

        // Every instance is where its own transform puts it.
        for (int index = 0; index < 5; ++index) {
            checkPixel(image, static_cast<uint32_t>(32.0f + (-0.8f + 0.4f * static_cast<float>(index)) * 32.0f), 16,
                       kRed);
        }
        checkPixel(image, 32, 32, kBlue);
        checkPixel(image, 32, 48, kRed);
        checkPixel(image, 26, 16, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("what is outside the view is not drawn") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(50.0f, 0.0f, 0.0f)));
        // Behind the camera.
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f, 0.0f, 20.0f)));
        const Image image = scene.render(scene.perspectiveView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f)));

        const SceneRenderStatistics& statistics = scene.renderer->statistics();
        CHECK(statistics.meshRenderers == 3);
        CHECK(statistics.culled == 2);
        CHECK(statistics.basePass.instances == 1);
        checkPixel(image, 32, 32, kRed);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("disabled entities and those below them are not drawn") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        const Entity parent = scene.world.createEntity("Group");
        const Entity child = scene.add(BuiltinMesh::Cube, scene.material("red"));
        REQUIRE(scene.world.setParent(child, parent).has_value());
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        checkPixel(scene.render(view), 32, 32, kRed);

        scene.world.setEnabled(parent, false);
        checkPixel(scene.render(view), 32, 32, kBackground);
        CHECK(scene.renderer->statistics().meshRenderers == 0);

        scene.world.setEnabled(parent, true);
        checkPixel(scene.render(view), 32, 32, kRed);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an entity follows its parent") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        const Entity parent = scene.world.createEntity("Arm");
        scene.world.transform(parent).position = Vec3(0.5f, 0.0f, 0.0f);
        const Entity child =
            scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.4f)));
        REQUIRE(scene.world.setParent(child, parent, false).has_value());

        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));
        // At (0.5, 0.5): right of and above the centre.
        checkPixel(image, 48, 16, kRed);
        checkPixel(image, 32, 32, kBackground);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a texture is shown the right way up, in the right colors") {
        SceneFixture scene(UVec2(128, 128));
        // Four quadrants: red top-left, green top-right, blue bottom-left, white bottom-right.
        Image quadrants = unwrap(Image::create(64, 64, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 64; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                const Vec3 color = y < 32 ? (x < 32 ? kRed : kGreen) : (x < 32 ? kBlue : Vec3(1.0f));
                quadrants.setPixel(x, y, Vec4(color, 1.0f));
            }
        }
        scene.writeImage("quadrants", quadrants);
        scene.open();
        scene.writeMaterial("picture",
                            fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, "emissiveColor": [1, 1, 1],
                                           "emissiveTexture": "{}")",
                                        scene.asset("quadrants.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());

        // The +Z face of a cube, seen from +Z: the image as it is.
        scene.add(BuiltinMesh::Cube, scene.material("picture"), at(Vec3(0.0f), Vec3(2.0f)));
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        checkPixel(image, 32, 32, kRed);
        checkPixel(image, 96, 32, kGreen);
        checkPixel(image, 32, 96, kBlue);
        checkPixel(image, 96, 96, Vec3(1.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("color textures are decoded from sRGB, so that a mid-grey image stays mid-grey") {
        SceneFixture scene;
        // 128 of 255 in the file. Read as a color, emitted and encoded again it must
        // come out as 128; treated as a linear number it would be far brighter.
        scene.writeImage("grey", makeChecker(8, 1, Vec4(128.0f / 255.0f), Vec4(128.0f / 255.0f)));
        scene.open();
        scene.writeMaterial("grey", fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, "emissiveColor": [1, 1, 1],
                                                    "emissiveTexture": "{}")",
                                                scene.asset("grey.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        scene.add(BuiltinMesh::Cube, scene.material("grey"), at(Vec3(0.0f), Vec3(2.0f)));

        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));
        CHECK(image.pixel(32, 32).r == doctest::Approx(128.0f / 255.0f).epsilon(0.02));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a normal map bends the surface the way its colors say") {
        SceneFixture scene;
        // Uniform normal maps: one leans towards +X of the image (red above the middle),
        // one towards +Y, which is up in the image (green above the middle).
        const auto uniformNormal = [](const Vec3& normal) {
            const Vec3 encoded = glm::normalize(normal) * 0.5f + 0.5f;
            return makeChecker(8, 1, Vec4(encoded, 1.0f), Vec4(encoded, 1.0f));
        };
        scene.writeImage("leanRight", uniformNormal(Vec3(1.0f, 0.0f, 1.0f)));
        scene.writeImage("leanUp", uniformNormal(Vec3(0.0f, 1.0f, 1.0f)));
        scene.open();
        // White and perfectly matte: brightness is the cosine of the angle to the light.
        const auto matte = [&](std::string_view texture, std::string_view more) {
            return fmt::format(R"("specular": 0, "roughness": 1, "normalTexture": "{}"{})",
                               scene.asset(texture).toString(), more);
        };
        scene.writeMaterial("right", matte("leanRight.png", ""));
        scene.writeMaterial("up", matte("leanUp.png", ""));
        scene.writeMaterial("flatter", matte("leanRight.png", R"(, "normalScale": 0)"));
        scene.writeMaterial("reversed", matte("leanRight.png", R"(, "normalScale": -1)"));
        REQUIRE(scene.assets->refresh().has_value());

        // The +Z face of a cube, seen head-on: the image's right is +X, its top is +Y.
        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("right"), at(Vec3(0.0f), Vec3(2.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        // The brightness of the face when lit from `toLight` by a light that would make
        // a surface facing it exactly white.
        const auto brightness = [&](std::string_view material, const Vec3& toLight) {
            scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material(material)};
            SceneRenderSettings settings = SceneFixture::flatSettings();
            scene.setSun(toLight, Vec3(kPi));
            const Vec4 pixel = scene.render(view, settings).pixel(32, 32);
            CHECK(pixel.r == doctest::Approx(pixel.g).epsilon(0.01));
            return srgbToLinear(pixel.r);
        };
        const float halfTurn = std::sqrt(0.5f);

        // Leaning right: facing a light from the upper right of the image plane's
        // normal, and edge-on to one from the left.
        CHECK(brightness("right", Vec3(1.0f, 0.0f, 1.0f)) == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(brightness("right", Vec3(-1.0f, 0.0f, 1.0f)) == doctest::Approx(0.0f).epsilon(0.02));
        CHECK(brightness("right", Vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(halfTurn).epsilon(0.02));

        // Leaning up: green is up in the image, which is +Y on this face.
        CHECK(brightness("up", Vec3(0.0f, 1.0f, 1.0f)) == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(brightness("up", Vec3(0.0f, -1.0f, 1.0f)) == doctest::Approx(0.0f).epsilon(0.02));

        // A scale of zero ignores the map; a negative one leans the other way.
        CHECK(brightness("flatter", Vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(brightness("reversed", Vec3(-1.0f, 0.0f, 1.0f)) == doctest::Approx(1.0f).epsilon(0.02));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the tangent frame turns with the object and survives mirroring") {
        SceneFixture scene;
        const Vec3 encoded = glm::normalize(Vec3(1.0f, 0.0f, 1.0f)) * 0.5f + 0.5f;
        scene.writeImage("leanRight", makeChecker(8, 1, Vec4(encoded, 1.0f), Vec4(encoded, 1.0f)));
        scene.open();
        scene.writeMaterial("right", fmt::format(R"("specular": 0, "roughness": 1, "normalTexture": "{}")",
                                                 scene.asset("leanRight.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        const auto brightness = [&](const Transform& transform, const Vec3& toLight) {
            scene.world.clear();
            scene.add(BuiltinMesh::Plane, scene.material("right"), transform);
            SceneRenderSettings settings = SceneFixture::flatSettings();
            scene.setSun(toLight, Vec3(kPi));
            return srgbToLinear(scene.render(view, settings).pixel(32, 32).r);
        };

        // The plane stood up to face the camera: its +X stays +X, so "right" is +X.
        const Quat upright = glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f));
        CHECK(brightness(at(Vec3(0.0f), Vec3(2.0f), upright), Vec3(1.0f, 0.0f, 1.0f)) ==
              doctest::Approx(1.0f).epsilon(0.02));

        // Turned a quarter turn about the view axis as well: its +X now points up.
        const Quat turned = glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f)) * upright;
        CHECK(brightness(at(Vec3(0.0f), Vec3(2.0f), turned), Vec3(0.0f, 1.0f, 1.0f)) ==
              doctest::Approx(1.0f).epsilon(0.02));
        CHECK(brightness(at(Vec3(0.0f), Vec3(2.0f), turned), Vec3(1.0f, 0.0f, 1.0f)) ==
              doctest::Approx(0.5f).epsilon(0.03));

        // Mirrored left to right: the image is flipped, and what leaned right leans left.
        CHECK(brightness(at(Vec3(0.0f), Vec3(-2.0f, 2.0f, 2.0f), upright), Vec3(-1.0f, 0.0f, 1.0f)) ==
              doctest::Approx(1.0f).epsilon(0.02));
        CHECK(brightness(at(Vec3(0.0f), Vec3(-2.0f, 2.0f, 2.0f), upright), Vec3(1.0f, 0.0f, 1.0f)) ==
              doctest::Approx(0.0f).epsilon(0.02));

        // Stretched: a normal stays perpendicular to its surface, so nothing changes.
        CHECK(brightness(at(Vec3(0.0f), Vec3(8.0f, 2.0f, 0.5f), upright), Vec3(1.0f, 0.0f, 1.0f)) ==
              doctest::Approx(1.0f).epsilon(0.02));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("textures tile and shift with the material's scale and offset") {
        SceneFixture scene(UVec2(128, 128));
        // Left half red, right half blue.
        Image halves = unwrap(Image::create(32, 32, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                halves.setPixel(x, y, Vec4(x < 16 ? kRed : kBlue, 1.0f));
            }
        }
        scene.writeImage("halves", halves);
        scene.open();
        const std::string texture = scene.asset("halves.png").toString();
        const auto members = [&](std::string_view more) {
            return fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, "emissiveColor": [1, 1, 1],
                                  "emissiveTexture": "{}", {})",
                               texture, more);
        };
        scene.writeMaterial("tiled", members(R"("uvScale": [2, 1])"));
        scene.writeMaterial("shifted", members(R"("uvOffset": [0.5, 0])"));
        scene.writeMaterial("clamped", members(R"("uvScale": [2, 1], "wrapU": "clamp")"));
        scene.writeMaterial("mirrored", members(R"("uvScale": [2, 1], "wrapU": "mirroredRepeat")"));
        scene.writeMaterial("nearest", members(R"("filter": "nearest")"));
        REQUIRE(scene.assets->refresh().has_value());

        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("tiled"), at(Vec3(0.0f), Vec3(2.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const auto renderWith = [&](std::string_view material) {
            scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material(material)};
            return scene.render(view);
        };

        SUBCASE("twice across: red, blue, red, blue") {
            const Image image = renderWith("tiled");
            checkPixel(image, 16, 64, kRed);
            checkPixel(image, 48, 64, kBlue);
            checkPixel(image, 80, 64, kRed);
            checkPixel(image, 112, 64, kBlue);
        }
        SUBCASE("shifted by half: blue, then red again") {
            const Image image = renderWith("shifted");
            checkPixel(image, 32, 64, kBlue);
            checkPixel(image, 96, 64, kRed);
        }
        SUBCASE("clamped: the last column goes on") {
            const Image image = renderWith("clamped");
            checkPixel(image, 16, 64, kRed);
            checkPixel(image, 48, 64, kBlue);
            checkPixel(image, 80, 64, kBlue);
            checkPixel(image, 112, 64, kBlue);
        }
        SUBCASE("mirrored: red, blue, blue, red") {
            const Image image = renderWith("mirrored");
            checkPixel(image, 16, 64, kRed);
            checkPixel(image, 48, 64, kBlue);
            checkPixel(image, 80, 64, kBlue);
            checkPixel(image, 112, 64, kRed);
        }
        SUBCASE("unfiltered: the edge between the halves is hard") {
            const Image image = renderWith("nearest");
            checkPixel(image, 62, 64, kRed);
            checkPixel(image, 65, 64, kBlue);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a highlight is as bright as the reflection model says, and where the camera sees it") {
        SceneFixture scene;
        // A black, glossy non-metal: all it shows of a light is the highlight.
        scene.writeMaterial("gloss", R"("baseColor": [0, 0, 0, 1], "roughness": 0.35)");
        scene.open();
        scene.add(BuiltinMesh::Plane, scene.material("gloss"), at(Vec3(0.0f), Vec3(4.0f)));

        // The light is straight above the floor and the camera looks straight down from
        // one metre up, so the floor mirrors the light exactly into the eye below it.
        SceneRenderSettings settings = SceneFixture::flatSettings();
        scene.setSun(kWorldUp, Vec3(1.0f));
        const Vec3 eye(0.0f, 1.0f, 0.0f);
        const Vec3 up(0.0f, 0.0f, -1.0f);

        // There the reflection is F * D * V with every angle zero: 4% reflectance, the
        // peak of the GGX distribution, 1 / (pi * alpha^2), and a visibility of 1/4.
        const float alpha = 0.35f * 0.35f;
        const Vec3 peak(0.04f / (4.0f * kPi * alpha * alpha));

        // Without perspective, every point of the floor is seen from that same direction:
        // the highlight covers all of it.
        const Image flat = scene.render(scene.orthographicView(eye, Vec3(0.0f), 3.0f, up), settings);
        checkPixel(flat, 32, 32, peak);
        checkPixel(flat, 3, 3, peak);
        checkPixel(flat, 60, 32, peak);
        checkPixel(flat, 32, 60, peak);

        // With perspective it is a spot below the eye, and gone towards the edges.
        CameraComponent lens;
        lens.verticalFovDegrees = 110.0f;
        const CameraView wide = makeCameraView(glm::inverse(lookAt(eye, Vec3(0.0f), up)), lens, 1.0f);
        const Image spot = scene.render(wide, settings);
        checkPixel(spot, 32, 32, peak);
        CHECK(spot.pixel(3, 32).r < 0.1f);
        CHECK(spot.pixel(32, 60).r < 0.1f);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("an occlusion texture is laid out like the others, unless it has coordinates of its own") {
        SceneFixture scene(UVec2(128, 128));
        // Occlusion in red: none in the top-left and bottom-right quarters, full in the others.
        scene.writeImage("shade", makeChecker(64, 2, Vec4(1.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)));
        scene.open();
        // White and matte, the textures repeated twice each way.
        const auto shaded = [&](std::string_view more) {
            return fmt::format(R"("specular": 0, "roughness": 1, "uvScale": [2, 2], "occlusionTexture": "{}"{})",
                               scene.asset("shade.png").toString(), more);
        };
        scene.writeMaterial("packed", shaded(""));
        scene.writeMaterial("own", shaded(R"(, "occlusionUvChannel": 1)"));
        REQUIRE(scene.assets->refresh().has_value());

        // Ambient light alone, the same from everywhere: what is seen is the occlusion.
        SceneRenderSettings settings = SceneFixture::flatSettings();
        scene.setAmbient(Vec3(1.0f), Vec3(1.0f));
        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("packed"), at(Vec3(0.0f), Vec3(2.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        // Sharing the coordinates of the other textures, it repeats with them: four by
        // four squares of 32 pixels.
        const Image packed = scene.render(view, settings);
        checkPixel(packed, 16, 16, Vec3(1.0f));
        checkPixel(packed, 48, 16, Vec3(0.0f));
        checkPixel(packed, 80, 16, Vec3(1.0f));
        checkPixel(packed, 48, 48, Vec3(1.0f));
        checkPixel(packed, 112, 112, Vec3(1.0f));

        // In the other set of coordinates - the same ones, on a built-in mesh, but its
        // own - it is used as it is: two by two squares of 64.
        scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material("own")};
        const Image own = scene.render(view, settings);
        checkPixel(own, 16, 16, Vec3(1.0f));
        checkPixel(own, 48, 16, Vec3(1.0f));
        checkPixel(own, 80, 16, Vec3(0.0f));
        checkPixel(own, 48, 80, Vec3(0.0f));
        checkPixel(own, 112, 112, Vec3(1.0f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a masked material has holes where its opacity is below the cutoff") {
        SceneFixture scene(UVec2(128, 128));
        // Opaque on the left, transparent on the right. The color is the same everywhere.
        Image mask = unwrap(Image::create(32, 32, ImageFormat::Rgba8));
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                mask.setPixel(x, y, Vec4(0.0f, 0.0f, 0.0f, x < 16 ? 1.0f : 0.2f));
            }
        }
        scene.writeImage("mask", mask);
        scene.open();
        const std::string texture = scene.asset("mask.png").toString();
        const auto members = [&](std::string_view more) {
            return fmt::format(R"("specular": 0, "emissiveColor": [1, 0, 0], "baseColorTexture": "{}", {})", texture,
                               more);
        };
        scene.writeMaterial("masked", members(R"("alphaMode": "masked")"));
        scene.writeMaterial("generous", members(R"("alphaMode": "masked", "alphaCutoff": 0.1)"));
        scene.writeMaterial("opaque", members(R"("alphaMode": "opaque")"));
        REQUIRE(scene.assets->refresh().has_value());

        const Entity cube = scene.add(BuiltinMesh::Cube, scene.material("masked"), at(Vec3(0.0f), Vec3(2.0f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const auto renderWith = [&](std::string_view material) {
            scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material(material)};
            return scene.render(view);
        };

        SUBCASE("at the default cutoff the right half is cut out") {
            const Image image = renderWith("masked");
            checkPixel(image, 32, 64, kRed);
            // Through the hole in the front face; the far side of the cube faces away
            // from the camera and is not drawn at all.
            checkPixel(image, 96, 64, kBackground);
        }
        SUBCASE("below a lower cutoff nothing is") {
            const Image image = renderWith("generous");
            checkPixel(image, 32, 64, kRed);
            checkPixel(image, 96, 64, kRed);
        }
        SUBCASE("an opaque material ignores opacity") {
            const Image image = renderWith("opaque");
            checkPixel(image, 96, 64, kRed);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("blended surfaces let what is behind them show through, far ones first") {
        SceneFixture scene;
        scene.writeFlatMaterial("wall", kBlue);
        // Half-transparent: the color of the pane is its emission, blended at 50%.
        scene.writeFlatMaterial("redGlass", kRed, R"("baseColor": [0, 0, 0, 0.5], "alphaMode": "blended")");
        scene.writeFlatMaterial("greenGlass", kGreen, R"("baseColor": [0, 0, 0, 0.5], "alphaMode": "blended")");
        scene.open();

        const Transform nearPane = at(Vec3(0.2f, 0.0f, 1.0f), Vec3(0.8f, 0.8f, 0.01f));
        const Transform farPane = at(Vec3(-0.2f, 0.0f, 0.0f), Vec3(0.8f, 0.8f, 0.01f));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        // The result must not depend on the order the panes are added in.
        for (const bool nearFirst : {true, false}) {
            CAPTURE(nearFirst);
            scene.world.clear();
            scene.add(BuiltinMesh::Cube, scene.material("wall"), at(Vec3(0.0f, 0.0f, -2.0f), Vec3(1.5f, 1.5f, 0.1f)));
            if (nearFirst) {
                scene.add(BuiltinMesh::Cube, scene.material("greenGlass"), nearPane);
                scene.add(BuiltinMesh::Cube, scene.material("redGlass"), farPane);
            } else {
                scene.add(BuiltinMesh::Cube, scene.material("redGlass"), farPane);
                scene.add(BuiltinMesh::Cube, scene.material("greenGlass"), nearPane);
            }
            const Image image = scene.render(view);

            // Each pane is a thin box of which only the front faces the camera: one
            // layer, half of which is the pane and half what is behind it.
            const auto through = [](const Vec3& behind, const Vec3& pane) { return glm::mix(behind, pane, 0.5f); };
            const Vec3 redOverWall = through(kBlue, kRed);
            // Only the red pane (left of the overlap).
            checkPixel(image, 16, 32, redOverWall);
            // Only the green pane (right of the overlap).
            checkPixel(image, 48, 32, through(kBlue, kGreen));
            // Green in front of red in front of the wall.
            checkPixel(image, 32, 32, through(redOverWall, kGreen));
            // Neither: above the panes, still in front of the wall.
            checkPixel(image, 32, 12, kBlue);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("blended surfaces are hidden by solid ones in front of them and do not hide solid ones behind") {
        SceneFixture scene;
        scene.writeFlatMaterial("solid", kBlue);
        scene.writeFlatMaterial("glass", kRed, R"("baseColor": [0, 0, 0, 0.5], "alphaMode": "blended")");
        scene.open();
        // Glass in front on the left, behind the solid cube on the right.
        scene.add(BuiltinMesh::Plane, scene.material("glass"),
                  at(Vec3(0.0f, 0.0f, 0.0f), Vec3(2.0f), glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f))));
        scene.add(BuiltinMesh::Cube, scene.material("solid"), at(Vec3(-0.5f, 0.0f, -1.0f), Vec3(0.6f)));
        scene.add(BuiltinMesh::Cube, scene.material("solid"), at(Vec3(0.5f, 0.0f, 1.0f), Vec3(0.6f)));

        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));
        checkPixel(image, 16, 32, glm::mix(kBlue, kRed, 0.5f));
        checkPixel(image, 48, 32, kBlue);
        checkPixel(image, 32, 4, glm::mix(kBackground, kRed, 0.5f));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("each submesh is drawn with its own material, and the default where there is none") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();

        // The default material is lit, so give the scene some ambient light to see it by.
        SceneRenderSettings settings = SceneFixture::flatSettings();
        scene.setAmbient(Vec3(1.0f), Vec3(1.0f));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        const Entity cube = scene.add(BuiltinMesh::Cube, Uuid());
        const Image withDefault = scene.render(view, settings);
        // White and mid-rough under uniform white light: clearly lit, neither black nor red.
        const Vec4 defaultPixel = withDefault.pixel(32, 32);
        CHECK(defaultPixel.r > 0.5f);
        CHECK(defaultPixel.r == doctest::Approx(defaultPixel.g).epsilon(0.02));
        CHECK(defaultPixel.r == doctest::Approx(defaultPixel.b).epsilon(0.02));

        // No entry at all is the same as a nil one.
        scene.world.tryGet<MeshRendererComponent>(cube)->materials.clear();
        CHECK(scene.render(view, settings).pixel(32, 32) == defaultPixel);
        // More entries than submeshes: the extra ones are ignored.
        scene.world.tryGet<MeshRendererComponent>(cube)->materials = {scene.material("red"), Uuid(), Uuid()};
        checkPixel(scene.render(view, settings), 32, 32, kRed);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("assets that cannot be loaded are reported once and do not stop the rest from being drawn") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.writeMaterial("broken", R"("rougness": 1)");
        scene.open();
        const Uuid missing = *Uuid::parse("11111111-1111-4111-8111-111111111111");

        // A mesh that does not exist, a material that does not exist, one that is broken.
        const Entity ghost = scene.world.createEntity("Ghost");
        scene.world.add<MeshRendererComponent>(ghost).mesh = missing;
        scene.add(BuiltinMesh::Cube, missing, at(Vec3(-0.6f, 0.0f, 0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("broken"), at(Vec3(0.6f, 0.0f, 0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f), Vec3(0.3f)));
        // Not a mesh at all.
        const Entity wrong = scene.world.createEntity("Wrong");
        scene.world.add<MeshRendererComponent>(wrong).mesh = scene.material("red");

        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const LogCapture log(LogLevel::Error);
        const Image image = scene.render(view);
        checkPixel(image, 32, 32, kRed);
        // The two cubes whose material failed are drawn with the default one: unlit here.
        checkPixel(image, 13, 32, Vec3(0.0f));
        checkPixel(image, 51, 32, Vec3(0.0f));
        CHECK(scene.renderer->statistics().meshRenderers == 3);

        CHECK(log.count(LogLevel::Error) == 4);
        CHECK(log.contains("cannot draw mesh 11111111-1111-4111-8111-111111111111"));
        CHECK(log.contains("red.material.json is a material, not a mesh"));
        CHECK(log.contains("cannot use material 11111111-1111-4111-8111-111111111111"));
        CHECK(log.contains("cannot use material broken.material.json"));

        // The next frames say nothing more.
        (void)scene.render(view);
        (void)scene.render(view);
        CHECK(log.count(LogLevel::Error) == 4);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a texture that cannot be loaded is reported once and left out of its material") {
        SceneFixture scene;
        REQUIRE(writeTextFile(scene.file("bad.png"), "this is not an image").has_value());
        scene.open();
        scene.writeFlatMaterial("red", kRed,
                                fmt::format(R"("emissiveTexture": "{}")", scene.asset("bad.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        const LogCapture log(LogLevel::Error);
        // Without its texture the material emits its plain color.
        checkPixel(scene.render(view), 32, 32, kRed);
        CHECK(log.count(LogLevel::Error) == 1);
        CHECK(log.contains("cannot use texture bad.png"));

        (void)scene.render(view);
        CHECK(log.count(LogLevel::Error) == 1);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("meshes, textures and materials are sent to the GPU once and counted") {
        SceneFixture scene;
        scene.writeImage("checker", makeChecker(8, 2, Vec4(1.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)));
        scene.open();
        scene.writeMaterial(
            "a", fmt::format(R"("baseColorTexture": "{0}", "emissiveTexture": "{0}", "normalTexture": "{0}")",
                             scene.asset("checker.png").toString()));
        scene.writeMaterial("b", fmt::format(R"("baseColorTexture": "{}")", scene.asset("checker.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());

        // Only what every renderer has from the start: two 1x1 textures and the default
        // material. The white one is four bytes; the flat normal map is eight, in half
        // floats, in which "no tilt" is exact.
        const RenderResourceStatistics before = scene.renderer->resources().statistics();
        CHECK(before.meshCount == 0);
        CHECK(before.textureCount == 2);
        CHECK(before.textureBytes == 12);
        CHECK(before.materialCount == 1);

        scene.add(BuiltinMesh::Cube, scene.material("a"), at(Vec3(-0.5f, 0.0f, 0.0f), Vec3(0.4f)));
        scene.add(BuiltinMesh::Cube, scene.material("b"), at(Vec3(0.5f, 0.0f, 0.0f), Vec3(0.4f)));
        scene.add(BuiltinMesh::Cube, scene.material("a"), at(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.4f)));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        (void)scene.render(view);

        const RenderResourceStatistics after = scene.renderer->resources().statistics();
        CHECK(after.meshCount == 1);
        // 24 vertices of 56 bytes and 36 sixteen-bit indices.
        CHECK(after.meshBytes == 24 * 56 + 36 * 2);
        CHECK(after.materialCount == 3);
        // One image, used as a color in two slots and as data in one: two textures.
        CHECK(after.textureCount == 4);
        // 8x8, 4x4, 2x2 and 1x1 pixels of four bytes, twice.
        CHECK(after.textureBytes == 12 + 2 * (64 + 16 + 4 + 1) * 4);

        // Further frames create nothing.
        (void)scene.render(view);
        CHECK(scene.renderer->resources().statistics().textureCount == 4);
        CHECK(scene.renderer->resources().statistics().meshCount == 1);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a texture larger than the limit is uploaded without its largest levels") {
        SceneFixture scene;
        scene.writeImage("large", makeChecker(16, 2, Vec4(1.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f)));
        scene.open({.maxTextureDimension = 4});
        scene.writeMaterial("m", fmt::format(R"("baseColorTexture": "{}")", scene.asset("large.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        scene.add(BuiltinMesh::Cube, scene.material("m"));
        (void)scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        // 4x4, 2x2 and 1x1 instead of starting at 16x16; plus the two default textures.
        CHECK(scene.renderer->resources().statistics().textureBytes == 12 + (16 + 4 + 1) * 4);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("refreshing the assets rebuilds what the GPU holds, with the files as they are now") {
        SceneFixture scene;
        scene.writeFlatMaterial("paint", kRed);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("paint"));
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        checkPixel(scene.render(view), 32, 32, kRed);

        scene.writeFlatMaterial("paint", kGreen);
        // Not picked up until the assets are refreshed.
        checkPixel(scene.render(view), 32, 32, kRed);

        REQUIRE(scene.assets->refresh().has_value());
        checkPixel(scene.render(view), 32, 32, kGreen);
        CHECK(scene.renderer->resources().statistics().meshCount == 1);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("each kind of surface goes through the passes made for it") {
        SceneFixture scene;
        scene.writeFlatMaterial("solid", kRed);
        scene.writeFlatMaterial("holes", kGreen, R"("alphaMode": "masked")");
        scene.writeFlatMaterial("glass", kBlue, R"("alphaMode": "blended")");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("solid"), at(Vec3(-0.6f, 0.0f, 0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("holes"), at(Vec3(0.0f), Vec3(0.3f)));
        scene.add(BuiltinMesh::Cube, scene.material("glass"), at(Vec3(0.6f, 0.0f, 0.0f), Vec3(0.3f)));
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f));

        checkPixel(image, 13, 32, kRed);
        checkPixel(image, 32, 32, kGreen);
        checkPixel(image, 51, 32, kBlue);

        // Opaque and masked surfaces: depth first, then the G-buffer. Blended ones: forward.
        const SceneRenderStatistics& statistics = scene.renderer->statistics();
        CHECK(statistics.depthPass == MeshPassStatistics{.drawCalls = 2, .instances = 2, .triangles = 24});
        CHECK(statistics.basePass == statistics.depthPass);
        CHECK(statistics.forwardPass == MeshPassStatistics{.drawCalls = 1, .instances = 1, .triangles = 12});
        CHECK(statistics.meshes == MeshPassStatistics{.drawCalls = 5, .instances = 5, .triangles = 60});
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the debug views show what the G-buffer holds of a surface") {
        SceneFixture scene;
        scene.writeMaterial("probe", R"("baseColor": [0.2, 0.4, 0.6, 1], "roughness": 0.25, "metallic": 1,
                                        "specular": 0.75, "emissiveColor": [0.5, 0.25, 0.125])");
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("probe"));

        const auto shown = [&](RenderDebugView debugView, const CameraView& view) {
            SceneRenderSettings settings = SceneFixture::flatSettings();
            settings.debugView = debugView;
            return scene.render(view, settings);
        };
        // Head-on, from four and a half metres in front of the face that looks along +Z.
        const CameraView front = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        // Colors are shown as colors ...
        checkPixel(shown(RenderDebugView::BaseColor, front), 32, 32, Vec3(0.2f, 0.4f, 0.6f));
        checkPixel(shown(RenderDebugView::Emissive, front), 32, 32, Vec3(0.5f, 0.25f, 0.125f));
        // ... and numbers as the grey that has their value.
        checkShown(shown(RenderDebugView::Roughness, front), 32, 32, Vec3(0.25f));
        checkShown(shown(RenderDebugView::Metallic, front), 32, 32, Vec3(1.0f));
        checkShown(shown(RenderDebugView::Specular, front), 32, 32, Vec3(0.75f));
        // The material has no occlusion texture: nothing is occluded.
        checkShown(shown(RenderDebugView::Occlusion, front), 32, 32, Vec3(1.0f));
        // A normal is shown with each axis from -1 to 1 as a color from 0 to 1.
        checkShown(shown(RenderDebugView::Normals, front), 32, 32, Vec3(0.5f, 0.5f, 1.0f));
        // Depth: white at the camera, half as bright every four metres. 1 / (1 + 4.5 / 4).
        checkShown(shown(RenderDebugView::Depth, front), 32, 32, Vec3(1.0f / 2.125f));

        // The other sides of the cube face the other ways.
        const CameraView above =
            scene.orthographicView(Vec3(0.0f, 5.0f, 0.0f), Vec3(0.0f), 2.0f, Vec3(0.0f, 0.0f, -1.0f));
        checkShown(shown(RenderDebugView::Normals, above), 32, 32, Vec3(0.5f, 1.0f, 0.5f));
        const CameraView left = scene.orthographicView(Vec3(-5.0f, 0.0f, 0.0f), Vec3(0.0f), 2.0f);
        checkShown(shown(RenderDebugView::Normals, left), 32, 32, Vec3(0.0f, 0.5f, 0.5f));

        // Where nothing is drawn, every view shows the background.
        for (const RenderDebugView debugView :
             {RenderDebugView::BaseColor, RenderDebugView::Normals, RenderDebugView::Depth}) {
            CAPTURE(nameOf(debugView));
            checkPixel(shown(debugView, front), 4, 4, kBackground);
        }
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the normals view shows normals as the lighting uses them, bent by the normal map") {
        SceneFixture scene;
        // A normal map that leans every normal towards the right of the image.
        const Vec3 encoded = glm::normalize(Vec3(1.0f, 0.0f, 1.0f)) * 0.5f + 0.5f;
        scene.writeImage("lean", makeChecker(8, 1, Vec4(encoded, 1.0f), Vec4(encoded, 1.0f)));
        scene.open();
        scene.writeMaterial("bent", fmt::format(R"("normalTexture": "{}")", scene.asset("lean.png").toString()));
        REQUIRE(scene.assets->refresh().has_value());
        scene.add(BuiltinMesh::Cube, scene.material("bent"), at(Vec3(0.0f), Vec3(2.0f)));

        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Normals;
        // On the +Z face the right of the image is +X: the normal is (1, 0, 1), normalized.
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f), settings);
        const float lean = 0.5f + 0.5f * std::sqrt(0.5f);
        checkShown(image, 32, 32, Vec3(lean, 0.5f, lean));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a surface looks the same whether it is lit from the G-buffer or drawn forward") {
        SceneFixture scene(UVec2(96, 96));
        scene.writeImage("colors", makeChecker(64, 4, Vec4(0.8f, 0.3f, 0.1f, 1.0f), Vec4(0.1f, 0.3f, 0.7f, 1.0f)));
        scene.writeImage("bumps", makeBumpNormalMap(64, 4));
        scene.open();
        // One material twice: solid, which is lit deferred, and blended with full
        // opacity, which is drawn forward and covers what is behind it just the same.
        const std::string surface = fmt::format(
            R"("baseColorTexture": "{}", "normalTexture": "{}", "roughness": 0.4, "metallic": 0.5,
               "specular": 0.8, "emissiveColor": [0.02, 0.05, 0.01])",
            scene.asset("colors.png").toString(), scene.asset("bumps.png").toString());
        scene.writeMaterial("deferred", surface);
        scene.writeMaterial("forward", surface + R"(, "alphaMode": "blended")");
        REQUIRE(scene.assets->refresh().has_value());

        const Entity ball = scene.add(BuiltinMesh::Sphere, scene.material("deferred"), at(Vec3(0.0f), Vec3(1.6f)));
        const CameraView view = scene.perspectiveView(Vec3(1.5f, 1.0f, 2.5f), Vec3(0.0f));
        // Lit the way a scene is: a key light and light from the sky and the ground.
        const SceneRenderSettings settings{.defaultLighting = true};

        const Image deferred = scene.render(view, settings);
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 0);
        scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material("forward")};
        const Image forward = scene.render(view, settings);
        CHECK(scene.renderer->statistics().basePass.drawCalls == 0);

        // The picture is not flat: there is something to compare.
        CHECK(glm::distance(Vec3(deferred.pixel(40, 40)), Vec3(deferred.pixel(60, 60))) > 0.05f);
        // The G-buffer stores what the forward pass has in full precision in eight bits
        // a channel; that is all the difference there may be.
        const ImageComparison difference = unwrap(compareImages(deferred, forward, {.channelTolerance = 3}));
        CHECK(difference.maxChannelDifference <= 3);
        CHECK(difference.differingPixels == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the lighting-only view lights every surface as plain matte grey") {
        SceneFixture scene;
        scene.writeMaterial("loud", R"("baseColor": [0.9, 0.1, 0.1, 1], "metallic": 1, "roughness": 0.3,
                                       "emissiveColor": [0, 0.5, 0])");
        scene.writeMaterial("grey", R"("baseColor": [0.5, 0.5, 0.5, 1], "metallic": 0, "roughness": 0.3)");
        scene.writeFlatMaterial("glass", kBlue, R"("alphaMode": "blended")");
        scene.open();
        const Entity ball = scene.add(BuiltinMesh::Sphere, scene.material("loud"), at(Vec3(0.0f), Vec3(1.6f)));
        const CameraView view = scene.perspectiveView(Vec3(1.0f, 1.5f, 2.5f), Vec3(0.0f));

        // (Debug views are never finished into a picture, so the picture they are held
        // against is not either.)
        SceneRenderSettings lightingOnly{.defaultLighting = true, .postProcessQuality = PostProcessQuality::Off};
        lightingOnly.debugView = RenderDebugView::LightingOnly;
        const Image shown = scene.render(view, lightingOnly);

        // The same ball made of grey, in the ordinary view: the same picture.
        scene.world.tryGet<MeshRendererComponent>(ball)->materials = {scene.material("grey")};
        const Image grey = scene.render(
            view, SceneRenderSettings{.defaultLighting = true, .postProcessQuality = PostProcessQuality::Off});
        const ImageComparison difference = unwrap(compareImages(shown, grey, {.channelTolerance = 2}));
        CHECK(difference.differingPixels == 0);
        // And grey it is: no trace of the red, of the metal or of the glow.
        const Vec4 pixel = shown.pixel(32, 28);
        CHECK(pixel.r == doctest::Approx(pixel.g).epsilon(0.1));
        CHECK(pixel.g > 0.2f);

        // Translucent surfaces are left out of this view.
        scene.add(BuiltinMesh::Cube, scene.material("glass"), at(Vec3(0.0f, 0.0f, 1.2f), Vec3(0.5f)));
        scene.render(view, lightingOnly);
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the wireframe view draws the edges of the triangles and nothing between them") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        if (!scene.context.graphics->adapter().supportsWireframe) {
            MESSAGE("this adapter cannot draw wireframes; nothing to test");
            return;
        }
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Wireframe;
        const Image image = scene.render(scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f), settings);

        // Lines are drawn: some pixels are neither background nor the material's red.
        const Vec3 background = displayed(kBackground);
        uint32_t linePixels = 0;
        for (uint32_t y = 0; y < image.height(); ++y) {
            for (uint32_t x = 0; x < image.width(); ++x) {
                const Vec3 pixel(image.pixel(x, y));
                CHECK(glm::distance(pixel, displayed(kRed)) > 0.2f);
                if (glm::distance(pixel, background) > 0.05f) {
                    ++linePixels;
                }
            }
        }
        // The face is 32 pixels square: four sides and a diagonal come to some 170 pixels,
        // and the edges of the faces behind it lie exactly behind those.
        CHECK(linePixels > 120);
        CHECK(linePixels < 260);
        // Inside a triangle, away from its edges, the background shows through.
        checkPixel(image, 40, 26, kBackground);
        checkPixel(image, 24, 38, kBackground);

        // Every triangle is drawn once, front and back alike, by the pass that stands in
        // for the forward pass.
        const SceneRenderStatistics& statistics = scene.renderer->statistics();
        CHECK(statistics.forwardPass == MeshPassStatistics{.drawCalls = 1, .instances = 1, .triangles = 12});
        CHECK(statistics.depthPass.drawCalls == 0);
        CHECK(statistics.basePass.drawCalls == 0);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("a renderer told that wireframes cannot be drawn shows the lit picture and says so once") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        scene.renderer = unwrap(SceneRenderer::create(scene.context.device(), *scene.context.shaders, *scene.assets,
                                                      {.supportsWireframe = false}));
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Wireframe;
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);

        const LogCapture log(LogLevel::Warn);
        checkPixel(scene.render(view, settings), 32, 32, kRed);
        checkPixel(scene.render(view, settings), 32, 32, kRed);
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("cannot draw wireframes"));
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the overdraw view is the brighter the more often a pixel is drawn, hidden surfaces included") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.writeFlatMaterial("glass", kBlue, R"("alphaMode": "blended")");
        scene.open();
        // A small cube in front of a large one: in the middle two surfaces are drawn,
        // around it one, though the eye would see one everywhere.
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f, 0.0f, -1.0f), Vec3(1.5f)));
        scene.add(BuiltinMesh::Cube, scene.material("red"), at(Vec3(0.0f, 0.0f, 1.0f), Vec3(0.5f)));
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Overdraw;
        const CameraView view = scene.orthographicView(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), 2.0f);
        const Image image = scene.render(view, settings);

        // Each layer adds (1/4, 1/8, 1/16) to black.
        const Vec3 layer(0.25f, 0.125f, 0.0625f);
        checkPixel(image, 32, 32, 2.0f * layer);
        checkPixel(image, 12, 32, layer);
        // Nothing drawn: black, whatever the background of the scene is.
        checkPixel(image, 2, 2, Vec3(0.0f));

        // Translucent surfaces count like any other.
        scene.add(BuiltinMesh::Cube, scene.material("glass"), at(Vec3(0.0f, 0.0f, 2.0f), Vec3(0.25f)));
        checkPixel(scene.render(view, settings), 32, 32, 3.0f * layer);
        CHECK(scene.renderer->statistics().forwardPass.drawCalls == 2);
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("one renderer serves several views of different sizes") {
        SceneFixture scene;
        scene.writeFlatMaterial("red", kRed);
        scene.open();
        scene.add(BuiltinMesh::Cube, scene.material("red"));
        SceneTargets wide = unwrap(SceneTargets::create(scene.context.device(), UVec2(128, 32), "Wide"));

        CameraComponent camera;
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = 2.0f;
        const Mat4 placement = glm::inverse(lookAt(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f)));

        scene.world.updateWorldTransforms();
        recordAndExecute(scene.context, [&](nvrhi::ICommandList* commandList) {
            scene.renderer->render(commandList, scene.world, makeCameraView(placement, camera, 1.0f), scene.targets,
                                   SceneFixture::flatSettings());
            scene.renderer->render(commandList, scene.world, makeCameraView(placement, camera, 4.0f), wide,
                                   SceneFixture::flatSettings());
        });

        const Image square = readToImage(*scene.context.graphics, scene.targets.display());
        const Image strip = readToImage(*scene.context.graphics, wide.display());
        checkPixel(square, 32, 32, kRed);
        checkPixel(square, 8, 32, kBackground);
        // The same cube, half the height of the view: 16 pixels of 128 wide.
        checkPixel(strip, 64, 16, kRed);
        checkPixel(strip, 52, 16, kBackground);
        checkPixel(strip, 76, 16, kBackground);

        // The last view on its own, and both together as what the frame cost.
        CHECK(scene.renderer->statistics().views == 1);
        CHECK(scene.renderer->statistics().basePass.triangles == 12);
        const SceneRenderStatistics frame = scene.renderer->takeFrameStatistics();
        CHECK(frame.views == 2);
        CHECK(frame.meshRenderers == 2);
        CHECK(frame.basePass == MeshPassStatistics{.drawCalls = 2, .instances = 2, .triangles = 24});
        CHECK(frame.depthPass == frame.basePass);
        CHECK(frame.meshes.triangles == 48);
        // Taking the numbers starts the count afresh.
        CHECK(scene.renderer->takeFrameStatistics().views == 0);
        CHECK(scene.renderer->statistics().views == 1);

        wide = SceneTargets();
        checkNoValidationIssues(*scene.context.graphics);
    }

    TEST_CASE("the built-in shapes under the default lighting match the reference image") {
        SceneFixture scene(UVec2(640, 360));
        scene.writeImage("checker",
                         makeChecker(64, 8, Vec4(0.55f, 0.55f, 0.52f, 1.0f), Vec4(0.25f, 0.26f, 0.28f, 1.0f)));
        scene.writeImage("bumps", makeBumpNormalMap(128, 4));
        scene.open();
        const std::string checker = scene.asset("checker.png").toString();
        const std::string bumps = scene.asset("bumps.png").toString();
        scene.writeMaterial("floor",
                            fmt::format(R"("baseColorTexture": "{}", "uvScale": [6, 6], "roughness": 0.8)", checker));
        scene.writeMaterial("paint", R"("baseColor": [0.55, 0.08, 0.06, 1], "roughness": 0.35)");
        scene.writeMaterial("steel", R"("baseColor": [0.56, 0.57, 0.58, 1], "metallic": 1, "roughness": 0.3)");
        scene.writeMaterial("studded", fmt::format(R"("baseColor": [0.30, 0.36, 0.28, 1], "roughness": 0.5,
                                                       "normalTexture": "{}", "uvScale": [2, 2])",
                                                   bumps));
        scene.writeMaterial("lamp", R"("baseColor": [0.02, 0.02, 0.02, 1], "emissiveColor": [1, 0.55, 0.2],
                                        "emissiveIntensity": 2)");
        scene.writeMaterial("glass", R"("baseColor": [0.6, 0.8, 0.9, 0.35], "alphaMode": "blended", "roughness": 0.1)");
        scene.writeMaterial("gold", R"("baseColor": [1.0, 0.77, 0.34, 1], "metallic": 1, "roughness": 0.2)");
        REQUIRE(scene.assets->refresh().has_value());

        scene.add(BuiltinMesh::Plane, scene.material("floor"), at(Vec3(0.0f), Vec3(12.0f)));
        scene.add(BuiltinMesh::Cube, scene.material("studded"), at(Vec3(-2.6f, 0.5f, 0.2f)));
        scene.add(BuiltinMesh::Sphere, scene.material("steel"), at(Vec3(-1.2f, 0.5f, 0.6f)));
        scene.add(BuiltinMesh::Cylinder, scene.material("paint"), at(Vec3(0.2f, 0.5f, 0.0f)));
        scene.add(BuiltinMesh::Cone, scene.material("lamp"), at(Vec3(1.5f, 0.5f, 0.5f)));
        scene.add(BuiltinMesh::Capsule, scene.material("paint"), at(Vec3(2.9f, 0.5f, -0.2f), Vec3(0.5f)));
        scene.add(BuiltinMesh::Torus, scene.material("gold"),
                  at(Vec3(0.2f, 1.45f, 0.0f), Vec3(1.0f), glm::angleAxis(0.5f, Vec3(1.0f, 0.0f, 0.0f))));
        scene.add(BuiltinMesh::Cube, scene.material("glass"), at(Vec3(-0.6f, 0.4f, 1.9f), Vec3(0.8f)));

        const Image image =
            scene.render(scene.perspectiveView(Vec3(0.6f, 2.6f, 6.2f), Vec3(0.1f, 0.5f, 0.0f), 40.0f),
                         SceneRenderSettings{.defaultLighting = true, .postProcessQuality = PostProcessQuality::Off});
        checkAgainstReference(image, "scene-primitives-640x360");
        CHECK(scene.renderer->statistics().meshRenderers == 8);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
