#include "cordite/renderer/SceneRenderer.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"
#include "cordite/math/Color.h"
#include "cordite/math/Projection.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/world/Components.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::LogCapture;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Linear colors that are easy to tell apart in a rendered image.
constexpr Vec3 kRed(1.0f, 0.0f, 0.0f);
constexpr Vec3 kGreen(0.0f, 1.0f, 0.0f);
constexpr Vec3 kBlue(0.0f, 0.0f, 1.0f);
constexpr Vec3 kBackground(0.25f, 0.25f, 0.25f);

/// What a linear color looks like once it has been encoded for display.
Vec3 displayed(const Vec3& linear) {
    return linearToSrgb(glm::clamp(linear, Vec3(0.0f), Vec3(1.0f)));
}

/// A device, a project directory to put assets in, and a world to draw.
///
/// Tests write the asset files they need, call open(), fill the world and render it.
class SceneFixture {
public:
    explicit SceneFixture(UVec2 size = UVec2(64, 64)) : m_size(size) {
        REQUIRE(ensureDirectory(m_directory.path()).has_value());
    }

    ~SceneFixture() {
        // GPU objects go before the device that made them.
        renderer.reset();
        targets = SceneTargets();
        context.graphics->waitIdle();
    }

    SceneFixture(const SceneFixture&) = delete;
    SceneFixture& operator=(const SceneFixture&) = delete;

    /// Where a file of the given name is, or would be, in the asset directory.
    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return m_directory.path() / pathFromUtf8(name);
    }

    /// Writes `<name>.material.json` with the given members.
    void writeMaterial(std::string_view name, std::string_view members) const {
        const std::string text =
            fmt::format(R"({{"format": "cordite.material", "version": 1{}{}}})", members.empty() ? "" : ", ", members);
        REQUIRE(writeTextFile(file(fmt::format("{}.material.json", name)), text).has_value());
    }

    /// Writes a material that shows `color` exactly, whatever the light: it reflects
    /// nothing and emits the color.
    void writeFlatMaterial(std::string_view name, const Vec3& color, std::string_view more = "") const {
        writeMaterial(name,
                      fmt::format(R"("baseColor": [0, 0, 0, 1], "specular": 0, "emissiveColor": [{}, {}, {}]{}{})",
                                  color.r, color.g, color.b, more.empty() ? "" : ", ", more));
    }

    void writeImage(std::string_view name, const Image& image) const {
        REQUIRE(image.savePng(file(fmt::format("{}.png", name))).has_value());
    }

    /// Indexes the files written so far and creates the renderer.
    void open(const RenderResourcesDesc& resources = {}) {
        assets = unwrap(AssetManager::create({.rootDirectory = m_directory.path()}));
        renderer = unwrap(SceneRenderer::create(context.device(), *context.shaders, *assets, resources));
        targets = unwrap(SceneTargets::create(context.device(), m_size, "Test"));
    }

    [[nodiscard]] Uuid asset(std::string_view reference) const {
        const AssetInfo* info = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(info != nullptr, "no asset " << std::string(reference));
        return info->id;
    }

    [[nodiscard]] Uuid material(std::string_view name) const { return asset(fmt::format("{}.material.json", name)); }

    Entity add(BuiltinMesh mesh, const Uuid& materialId, const Transform& transform = {}) {
        const Entity entity = world.createEntity(std::string(nameOf(mesh)));
        world.transform(entity) = transform;
        MeshRendererComponent& component = world.add<MeshRendererComponent>(entity);
        component.mesh = builtinMeshId(mesh);
        component.materials = {materialId};
        return entity;
    }

    /// Renders the world and returns the display image.
    Image render(const CameraView& view, const SceneRenderSettings& settings = flatSettings()) {
        world.updateWorldTransforms();
        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            renderer->render(commandList, world, view, targets, settings);
        });
        return readToImage(*context.graphics, targets.display());
    }

    /// No light at all, and a grey background: what is seen is what materials emit.
    static SceneRenderSettings flatSettings() {
        return {.backgroundColor = kBackground,
                .keyLightColor = Vec3(0.0f),
                .skyColor = Vec3(0.0f),
                .groundColor = Vec3(0.0f)};
    }

    /// A camera without perspective that looks from `eye` at `target` and sees
    /// `height` metres from top to bottom.
    [[nodiscard]] CameraView orthographicView(const Vec3& eye, const Vec3& target, float height,
                                              const Vec3& up = kWorldUp) const {
        CameraComponent camera;
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = height;
        camera.nearPlane = 0.1f;
        camera.farPlane = 100.0f;
        return makeCameraView(glm::inverse(lookAt(eye, target, up)), camera,
                              static_cast<float>(m_size.x) / static_cast<float>(m_size.y));
    }

    [[nodiscard]] CameraView perspectiveView(const Vec3& eye, const Vec3& target, float fovDegrees = 50.0f) const {
        CameraComponent camera;
        camera.verticalFovDegrees = fovDegrees;
        return makeCameraView(glm::inverse(lookAt(eye, target)), camera,
                              static_cast<float>(m_size.x) / static_cast<float>(m_size.y));
    }

    RenderContext context;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<SceneRenderer> renderer;
    SceneTargets targets;
    World world;

private:
    TempDirectory m_directory;
    UVec2 m_size;
};

/// Checks one pixel against a linear color, allowing for rounding.
void checkPixel(const Image& image, uint32_t x, uint32_t y, const Vec3& linear) {
    CAPTURE(x);
    CAPTURE(y);
    CHECK_NEAR(Vec3(image.pixel(x, y)), displayed(linear), 2.5f / 255.0f);
}

Transform at(const Vec3& position, const Vec3& scale = Vec3(1.0f), const Quat& rotation = kIdentityQuat) {
    return {.position = position, .rotation = rotation, .scale = scale};
}

/// A square image of `cells` x `cells` squares in two colors, top-left square first.
Image makeChecker(uint32_t size, uint32_t cells, const Vec4& first, const Vec4& second) {
    Image image = unwrap(Image::create(size, size, ImageFormat::Rgba8));
    const uint32_t cellSize = size / cells;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            image.setPixel(x, y, ((x / cellSize) + (y / cellSize)) % 2 == 0 ? first : second);
        }
    }
    return image;
}

/// A normal map of round bumps, one per cell: the surface normal of each bump, encoded
/// the way normal maps are, with +Y up in the image.
Image makeBumpNormalMap(uint32_t size, uint32_t cells) {
    Image image = unwrap(Image::create(size, size, ImageFormat::Rgba8));
    const float cellSize = static_cast<float>(size) / static_cast<float>(cells);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            // Position within the cell, from -1 to 1, with y pointing up.
            const float u = std::fmod(static_cast<float>(x) + 0.5f, cellSize) / cellSize * 2.0f - 1.0f;
            const float v = 1.0f - std::fmod(static_cast<float>(y) + 0.5f, cellSize) / cellSize * 2.0f;
            const float radius = std::sqrt(u * u + v * v);
            Vec3 normal(0.0f, 0.0f, 1.0f);
            if (radius < 0.8f) {
                // The slope of a dome that meets the flat surface at radius 0.8.
                const float slope = radius / 0.8f;
                normal = glm::normalize(Vec3(u * slope, v * slope, std::sqrt(std::max(1.0f - slope * slope, 0.05f))));
            }
            image.setPixel(x, y, Vec4(normal * 0.5f + 0.5f, 1.0f));
        }
    }
    return image;
}

} // namespace

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
        CHECK(statistics.meshes.drawCalls == 1);
        CHECK(statistics.meshes.instances == 1);
        CHECK(statistics.meshes.triangles == 12);
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
        CHECK(statistics.meshes.drawCalls == 3);
        CHECK(statistics.meshes.instances == 9);

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
        CHECK(statistics.meshes.instances == 1);
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
            settings.keyLightDirection = toLight;
            settings.keyLightColor = Vec3(kPi);
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
            settings.keyLightDirection = toLight;
            settings.keyLightColor = Vec3(kPi);
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
        settings.keyLightDirection = kWorldUp;
        settings.keyLightColor = Vec3(1.0f);
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
        settings.skyColor = Vec3(1.0f);
        settings.groundColor = Vec3(1.0f);
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

        // Only what every renderer has from the start: two 1x1 textures and the default material.
        const RenderResourceStatistics before = scene.renderer->resources().statistics();
        CHECK(before.meshCount == 0);
        CHECK(before.textureCount == 2);
        CHECK(before.textureBytes == 8);
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
        CHECK(after.textureBytes == 8 + 2 * (64 + 16 + 4 + 1) * 4);

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
        CHECK(scene.renderer->resources().statistics().textureBytes == 8 + (16 + 4 + 1) * 4);
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
        CHECK(scene.renderer->statistics().meshes.triangles == 12);
        const SceneRenderStatistics frame = scene.renderer->takeFrameStatistics();
        CHECK(frame.views == 2);
        CHECK(frame.meshRenderers == 2);
        CHECK(frame.meshes.drawCalls == 2);
        CHECK(frame.meshes.instances == 2);
        CHECK(frame.meshes.triangles == 24);
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

        const Image image = scene.render(scene.perspectiveView(Vec3(0.6f, 2.6f, 6.2f), Vec3(0.1f, 0.5f, 0.0f), 40.0f),
                                         SceneRenderSettings());
        checkAgainstReference(image, "scene-primitives-640x360");
        CHECK(scene.renderer->statistics().meshRenderers == 8);
        checkNoValidationIssues(*scene.context.graphics);
    }
}
