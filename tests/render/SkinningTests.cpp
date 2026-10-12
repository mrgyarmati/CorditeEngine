#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/MeshPass.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/SkinPalettes.h"
#include "cordite/renderer/ViewBindings.h"
#include "cordite/world/World.h"

#include "render/RenderTestSupport.h"
#include "render/SceneTestSupport.h"
#include "support/SkinnedModelSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace cordite;
using cordite::test::at;
using cordite::test::checkNoValidationIssues;
using cordite::test::checkPixel;
using cordite::test::kBackground;
using cordite::test::kBlue;
using cordite::test::kGreen;
using cordite::test::kRed;
using cordite::test::LogCapture;
using cordite::test::SceneFixture;
using cordite::test::unwrap;

namespace {

/// The pictures are this many pixels a side ...
constexpr uint32_t kSize = 128;
/// ... and show this many metres, looked at from the front: 32 pixels to the metre.
constexpr float kViewHeight = 4.0f;

/// The pixel that shows the point (x, y) of the plane z = 0 in frontView().
UVec2 pixelAt(float x, float y) {
    const float scale = static_cast<float>(kSize) / kViewHeight;
    return {static_cast<uint32_t>(64.0f + x * scale), static_cast<uint32_t>(64.0f - (y - 1.0f) * scale)};
}

/// A scene with the skinned bar of SkinnedModelSupport.h in it: two metres tall and a
/// fifth of a metre thick, standing on the origin, on three joints a metre apart. Its
/// clip "Bend" turns the middle joint a quarter turn about +Z in a second, which tips
/// the upper half of the bar over to the left of the picture.
struct BarScene {
    BarScene() : scene(UVec2(kSize, kSize)) {
        REQUIRE(exportGltf(test::makeSkinnedBar(), scene.file("bar.gltf")).has_value());
        scene.writeFlatMaterial("red", kRed);
        scene.writeFlatMaterial("green", kGreen);
        scene.writeFlatMaterial("masked", kBlue, R"("alphaMode": "masked", "alphaCutoff": 0.5)");
        scene.writeFlatMaterial("glass", kBlue, R"("alphaMode": "blended")");
        scene.writeMatteMaterial("white");
        scene.open();
        mesh = scene.asset("bar.gltf#mesh/0");
        skeleton = scene.asset("bar.gltf#skeleton/0");
        bend = scene.asset("bar.gltf#animation/0");
    }

    /// A bar without an animator: a skinned mesh, and nothing that poses it.
    Entity addBar(std::string_view material = "red", const Transform& transform = {}) {
        const Entity entity = scene.world.createEntity("Bar");
        scene.world.transform(entity) = transform;
        MeshRendererComponent& renderer = scene.world.add<MeshRendererComponent>(entity);
        renderer.mesh = mesh;
        renderer.materials = {scene.material(material)};
        return entity;
    }

    /// Gives an entity an animator that holds "Bend" at `time`: 0 upright, 1 bent.
    void pose(Entity entity, float time) {
        scene.world.add<AnimatorComponent>(entity) = {
            .skeleton = skeleton, .clip = bend, .time = time, .loop = false, .playing = false};
    }

    /// The whole bar and a metre to either side of it, from the front.
    [[nodiscard]] CameraView frontView() const {
        return scene.orthographicView(Vec3(0.0f, 1.0f, 5.0f), Vec3(0.0f, 1.0f, 0.0f), kViewHeight);
    }

    /// Poses the skeletons of the world and draws it.
    Image render(const CameraView& view, const SceneRenderSettings& settings = SceneFixture::flatSettings()) {
        animation.evaluate(scene.world, *scene.assets);
        return scene.render(view, settings);
    }

    [[nodiscard]] const SceneRenderStatistics& statistics() const { return scene.renderer->statistics(); }

    SceneFixture scene;
    AnimationSystem animation;
    Uuid mesh;
    Uuid skeleton;
    Uuid bend;
};

/// Checks the pixel that shows the point (x, y) of the plane z = 0.
void checkPoint(const Image& image, float x, float y, const Vec3& linear) {
    const UVec2 pixel = pixelAt(x, y);
    checkPixel(image, pixel.x, pixel.y, linear);
}

/// Checks that a picture shows the bar standing upright, in `color`.
void checkUpright(const Image& image, const Vec3& color, float offsetX = 0.0f) {
    checkPoint(image, offsetX, 0.25f, color);
    checkPoint(image, offsetX, 1.0f, color);
    checkPoint(image, offsetX, 1.75f, color);
    // Not where its upper half lies when it is bent, nor beside or above it.
    checkPoint(image, offsetX - 0.5f, 1.0f, kBackground);
    checkPoint(image, offsetX + 0.3f, 1.5f, kBackground);
    checkPoint(image, offsetX, 2.2f, kBackground);
}

/// Checks that a picture shows the bar bent: its foot where it was, its upper half
/// lying along -X (or along +X, for a bar that is mirrored) at the height of the middle
/// joint.
void checkBent(const Image& image, const Vec3& color, float offsetX = 0.0f, float side = -1.0f) {
    // The foot follows the root alone, which has not moved.
    checkPoint(image, offsetX, 0.1f, color);
    // The upper half, tipped over: from the middle joint a metre out, a tenth to
    // either side of the height the joint is at.
    checkPoint(image, offsetX + side * 0.25f, 1.0f, color);
    checkPoint(image, offsetX + side * 0.5f, 1.0f, color);
    checkPoint(image, offsetX + side * 0.9f, 1.0f, color);
    // It ends after a metre, and is a fifth of a metre thick.
    checkPoint(image, offsetX + side * 1.15f, 1.0f, kBackground);
    checkPoint(image, offsetX + side * 0.5f, 1.25f, kBackground);
    checkPoint(image, offsetX + side * 0.5f, 0.75f, kBackground);
    // Where it stood upright there is nothing now.
    checkPoint(image, offsetX, 1.5f, kBackground);
    checkPoint(image, offsetX, 1.9f, kBackground);
}

} // namespace

TEST_SUITE("Skinning") {
    TEST_CASE("a skinned mesh without a pose is drawn as it was modelled") {
        BarScene bar;
        bar.addBar();
        const Image image = bar.render(bar.frontView());
        checkUpright(image, kRed);
        CHECK(bar.statistics().meshRenderers == 1);
        CHECK(bar.statistics().skinnedMeshes == 0);
        CHECK(bar.statistics().jointMatrices == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a skinned mesh at rest is drawn, pixel for pixel, where the mesh was modelled") {
        BarScene bar;
        const Entity entity = bar.addBar();
        const Image modelled = bar.render(bar.frontView());

        // Posed, by a skeleton whose joints stand where they were bound: every matrix
        // leaves every vertex where it is, and the picture is the same to the last bit.
        bar.pose(entity, 0.0f);
        const Image posed = bar.render(bar.frontView());
        checkUpright(posed, kRed);
        CHECK(bar.statistics().skinnedMeshes == 1);
        CHECK(bar.statistics().jointMatrices == 3);
        CHECK(std::ranges::equal(posed.pixels(), modelled.pixels()));
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a pose moves the vertices: the upper half of a bent bar lies on its side") {
        BarScene bar;
        const Entity entity = bar.addBar();
        bar.pose(entity, 1.0f);
        const Image image = bar.render(bar.frontView());
        checkBent(image, kRed);
        // Drawn once by the depth pre-pass and once by the base pass, which keeps only
        // what has the very depth the first left: a bar without holes in it says that
        // both worked out the same positions.
        CHECK(bar.statistics().depthPass == MeshPassStatistics{.drawCalls = 1, .instances = 1, .triangles = 32});
        CHECK(bar.statistics().basePass == bar.statistics().depthPass);

        // Half way through the clip the joint is turned an eighth of a turn, and the
        // tip of the bar is where that puts it: up and to the left of the joint.
        bar.scene.world.tryGet<AnimatorComponent>(entity)->time = 0.5f;
        const Image halfway = bar.render(bar.frontView());
        const float along = std::sqrt(0.5f);
        checkPoint(halfway, -along * 0.9f, 1.0f + along * 0.9f, kRed);
        checkPoint(halfway, -along * 0.5f, 1.0f + along * 0.5f, kRed);
        checkPoint(halfway, 0.0f, 1.9f, kBackground);
        checkPoint(halfway, -0.9f, 1.0f, kBackground);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("the entity of a skinned mesh places what the pose made of it") {
        BarScene bar;
        // Moved to the right and half as large: the bend is the same, a metre is half.
        const Entity entity = bar.addBar("red", at(Vec3(1.0f, 0.5f, 0.0f), Vec3(0.5f)));
        bar.pose(entity, 1.0f);
        const Image image = bar.render(bar.frontView());
        // The foot, a twentieth above the ground the bar stands on ...
        checkPoint(image, 1.0f, 0.55f, kRed);
        // ... and the upper half, lying along -X from the middle joint at (1, 1).
        checkPoint(image, 0.8f, 1.0f, kRed);
        checkPoint(image, 0.6f, 1.0f, kRed);
        checkPoint(image, 0.4f, 1.0f, kBackground);
        checkPoint(image, 1.0f, 1.3f, kBackground);

        // Turned half way round about the upright axis, it bends to the other side.
        bar.scene.world.transform(entity) = at(Vec3(0.0f), Vec3(1.0f), glm::angleAxis(kPi, kWorldUp));
        checkBent(bar.render(bar.frontView()), kRed, 0.0f, 1.0f);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("figures in poses of their own are instances of one mesh") {
        BarScene bar;
        const Entity left = bar.addBar("red", at(Vec3(-0.7f, 0.0f, 0.0f)));
        const Entity right = bar.addBar("red", at(Vec3(1.3f, 0.0f, 0.0f)));
        bar.pose(left, 0.0f);
        bar.pose(right, 1.0f);
        const Image image = bar.render(bar.frontView());
        checkUpright(image, kRed, -0.7f);
        checkBent(image, kRed, 1.3f);
        CHECK(bar.statistics().skinnedMeshes == 2);
        CHECK(bar.statistics().jointMatrices == 6);
        // One draw call for both, in each pass.
        CHECK(bar.statistics().depthPass == MeshPassStatistics{.drawCalls = 1, .instances = 2, .triangles = 64});
        CHECK(bar.statistics().basePass == bar.statistics().depthPass);

        // A bar that is not posed is the same mesh, but not drawn the same way: a draw
        // call of its own.
        bar.addBar("red", at(Vec3(0.3f, 0.0f, 0.0f)));
        const Image three = bar.render(bar.frontView());
        checkUpright(three, kRed, -0.7f);
        checkUpright(three, kRed, 0.3f);
        checkBent(three, kRed, 1.3f);
        CHECK(bar.statistics().skinnedMeshes == 2);
        CHECK(bar.statistics().basePass == MeshPassStatistics{.drawCalls = 2, .instances = 3, .triangles = 96});
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("the meshes of a figure are drawn in the pose of what they hang under") {
        BarScene bar;
        // What is animated is the figure; its meshes are entities below it.
        const Entity figure = bar.scene.world.createEntity("Figure");
        bar.scene.world.transform(figure).position = Vec3(0.5f, 0.0f, 0.0f);
        bar.pose(figure, 1.0f);
        const Entity body = bar.addBar("red");
        const Entity armour = bar.addBar("green", at(Vec3(0.0f, 0.0f, -0.5f)));
        REQUIRE(bar.scene.world.setParent(body, figure, false).has_value());
        REQUIRE(bar.scene.world.setParent(armour, figure, false).has_value());

        const Image image = bar.render(bar.frontView());
        // Both bent, the one in front hiding the one behind it.
        checkBent(image, kRed, 0.5f);
        CHECK(bar.statistics().skinnedMeshes == 2);
        // One pose, sent to the GPU once.
        CHECK(bar.statistics().jointMatrices == 3);

        // A mesh below with an animator of its own is drawn in its own pose.
        bar.pose(armour, 0.0f);
        bar.scene.world.transform(armour).position = Vec3(1.0f, 0.0f, 0.0f);
        const Image own = bar.render(bar.frontView());
        checkBent(own, kRed, 0.5f);
        checkPoint(own, 1.5f, 1.75f, kGreen);
        checkPoint(own, 1.5f, 0.25f, kGreen);
        CHECK(bar.statistics().jointMatrices == 6);

        // The figure switched off, nothing of it is drawn, and nothing is posed.
        bar.scene.world.setEnabled(figure, false);
        const Image off = bar.render(bar.frontView());
        checkPoint(off, 0.0f, 1.0f, kBackground);
        checkPoint(off, 1.5f, 1.75f, kBackground);
        CHECK(bar.statistics().meshRenderers == 0);
        CHECK(bar.statistics().jointMatrices == 0);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("masked, translucent and mirrored skinned meshes are drawn in their pose as well") {
        BarScene bar;
        Entity entity;
        Vec3 color = kBlue;
        float side = -1.0f;
        SUBCASE("a material that cuts holes") {
            entity = bar.addBar("masked");
        }
        SUBCASE("a material that lets what is behind it show") {
            entity = bar.addBar("glass");
        }
        SUBCASE("an instance that is mirrored") {
            // Its triangles are turned the other way round, and it bends to the right.
            entity = bar.addBar("red", at(Vec3(0.0f), Vec3(-1.0f, 1.0f, 1.0f)));
            color = kRed;
            side = 1.0f;
        }
        bar.pose(entity, 1.0f);
        checkBent(bar.render(bar.frontView()), color, 0.0f, side);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("the debug views that draw the meshes themselves draw them in their pose") {
        BarScene bar;
        const Entity entity = bar.addBar();
        bar.pose(entity, 1.0f);
        SceneRenderSettings settings = SceneFixture::flatSettings();
        settings.debugView = RenderDebugView::Overdraw;
        const Image overdraw = bar.render(bar.frontView(), settings);
        const auto shows = [&](const Image& image, float x, float y) {
            const UVec2 pixel = pixelAt(x, y);
            const Vec4 shown = image.pixel(pixel.x, pixel.y);
            return shown.r + shown.g + shown.b > 0.05f;
        };
        // Something is counted where the bent bar is, and nothing where it stood.
        CHECK(shows(overdraw, -0.5f, 1.0f));
        CHECK(shows(overdraw, 0.0f, 0.1f));
        CHECK_FALSE(shows(overdraw, 0.0f, 1.6f));

        if (bar.scene.context.graphics->adapter().supportsWireframe) {
            settings.debugView = RenderDebugView::Wireframe;
            const Image wireframe = bar.render(bar.frontView(), settings);
            // The edges of its triangles, over the background of the view. Along the
            // middle of the tipped half there are some to cross - the ring half way
            // along it, the diagonals of its sides - and along the middle of where
            // the bar stood there are none.
            const Vec4 background = wireframe.pixel(2, 2);
            const auto hasEdge = [&](float x, float y) {
                const UVec2 pixel = pixelAt(x, y);
                return glm::distance(wireframe.pixel(pixel.x, pixel.y), background) > 0.05f;
            };
            bool foundTipped = false;
            for (float x = -0.95f; x < -0.05f; x += 0.01f) {
                foundTipped = foundTipped || hasEdge(x, 1.0f);
            }
            CHECK(foundTipped);
            bool foundUpright = false;
            for (float y = 1.3f; y < 1.95f; y += 0.01f) {
                foundUpright = foundUpright || hasEdge(0.0f, y);
            }
            CHECK_FALSE(foundUpright);
        }
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a pose that has fewer joints than the mesh follows is not used, and that is said once") {
        BarScene bar;
        const Entity entity = bar.addBar();
        // As a system might leave it that poses the wrong skeleton: two joints, turned
        // upside down, for a mesh that follows three.
        const Mat4 upsideDown = glm::rotate(Mat4(1.0f), kPi, Vec3(0.0f, 0.0f, 1.0f));
        bar.scene.world.add<SkeletonPoseComponent>(entity).skinning = {upsideDown, upsideDown};

        const LogCapture log;
        const Image image = bar.scene.render(bar.frontView());
        checkUpright(image, kRed);
        CHECK(bar.statistics().skinnedMeshes == 0);
        CHECK(bar.statistics().jointMatrices == 0);
        checkUpright(bar.scene.render(bar.frontView()), kRed);
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("is drawn as it was modelled: its vertices follow 3 joints, and the skeleton that 'Bar' is "
                           "posed with has 2"));

        // More joints than the mesh follows are no trouble.
        bar.scene.world.tryGet<SkeletonPoseComponent>(entity)->skinning = std::vector<Mat4>(5, Mat4(1.0f));
        checkUpright(bar.scene.render(bar.frontView()), kRed);
        CHECK(bar.statistics().skinnedMeshes == 1);
        CHECK(bar.statistics().jointMatrices == 5);
        CHECK(log.count(LogLevel::Warn) == 1);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a joint matrix that is not made of numbers moves nothing, and breaks nothing") {
        BarScene bar;
        const Entity entity = bar.addBar();
        const Mat4 nothing(std::numeric_limits<float>::quiet_NaN());
        Mat4 endless(1.0f);
        endless[3][0] = std::numeric_limits<float>::infinity();
        bar.scene.world.add<SkeletonPoseComponent>(entity).skinning = {Mat4(1.0f), nothing, endless};

        const Image image = bar.scene.render(bar.frontView());
        // Still in view, and every pixel a color: what follows the broken joints is
        // where it was modelled.
        CHECK(bar.statistics().culled == 0);
        CHECK(bar.statistics().skinnedMeshes == 1);
        checkUpright(image, kRed);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("what is in view is decided by where the pose puts the mesh") {
        BarScene bar;
        const Entity entity = bar.addBar();
        // A view of where the upper half of the bar lies when it is bent, and of
        // nothing the bar covers while it stands upright.
        const CameraView beside = bar.scene.orthographicView(Vec3(-0.7f, 1.0f, 5.0f), Vec3(-0.7f, 1.0f, 0.0f), 0.5f);
        // A view of where its top is while it stands, and of nothing it covers bent.
        const CameraView above = bar.scene.orthographicView(Vec3(0.0f, 1.7f, 5.0f), Vec3(0.0f, 1.7f, 0.0f), 0.5f);

        bar.pose(entity, 0.0f);
        Image image = bar.render(beside);
        CHECK(bar.statistics().culled == 1);
        checkPixel(image, 64, 64, kBackground);
        image = bar.render(above);
        CHECK(bar.statistics().culled == 0);
        checkPixel(image, 64, 64, kRed);

        bar.scene.world.tryGet<AnimatorComponent>(entity)->time = 1.0f;
        image = bar.render(beside);
        CHECK(bar.statistics().culled == 0);
        checkPixel(image, 64, 64, kRed);
        image = bar.render(above);
        CHECK(bar.statistics().culled == 1);
        checkPixel(image, 64, 64, kBackground);
        // Culled, it is not drawn, but it is posed all the same: its matrices are
        // there for a light that sees it.
        CHECK(bar.statistics().depthPass == MeshPassStatistics{});
        CHECK(bar.statistics().skinnedMeshes == 1);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }

    TEST_CASE("a skinned mesh throws the shadow of its pose") {
        BarScene bar;
        // A floor, and the bar on it - made a metre deep, so that its shadow is wide
        // enough to say where it is.
        const Entity floor =
            bar.scene.add(BuiltinMesh::Plane, bar.scene.material("white"), at(Vec3(0.0f), Vec3(12.0f)));
        (void)floor;
        const Entity entity = bar.addBar("white", at(Vec3(0.0f), Vec3(1.0f, 1.0f, 5.0f)));
        // A sun in front of the bar and to its right: the shadow of a point lies as far
        // to the left of it, and as far behind it, as the point is high. Bright enough
        // to light the floor white.
        const Vec3 toLight = glm::normalize(Vec3(1.0f, 1.0f, 1.0f));
        LightComponent sun;
        sun.type = LightType::Directional;
        sun.color = Vec3(1.0f / toLight.y);
        sun.intensity = 1.0f;
        sun.castsShadows = true;
        bar.scene.addLight(sun, Vec3(5.0f, 5.0f, 5.0f), -toLight);

        // Seen from above, with -Z up in the picture: 5 metres, centred on (-1, -1).
        const CameraView fromAbove = bar.scene.orthographicView(Vec3(-1.0f, 8.0f, -1.0f), Vec3(-1.0f, 0.0f, -1.0f),
                                                                5.0f, Vec3(0.0f, 0.0f, -1.0f));
        const auto floorAt = [](float x, float z) {
            const float scale = static_cast<float>(kSize) / 5.0f;
            return UVec2(static_cast<uint32_t>(64.0f + (x + 1.0f) * scale),
                         static_cast<uint32_t>(64.0f + (z + 1.0f) * scale));
        };
        const auto shownAt = [&](const Image& image, float x, float z) {
            const UVec2 pixel = floorAt(x, z);
            return image.pixel(pixel.x, pixel.y).r;
        };

        // The bar is a tube without ends, and a shadow map shows the faces turned to
        // the light: the points below are in the shadows of those, well inside them.
        //
        // Upright: the side of the bar that faces +X, 1.9 metres up, shades the floor
        // 1.8 metres to the left and 1.9 metres behind where it is.
        const Vec2 underTheTop(-1.8f, -2.0f);
        // Bent: the upper half lies 1.1 metres up, along -X, and what is now its upper
        // side shades the floor 1.1 metres to the left of it and as far behind it.
        const Vec2 underTheTipped(-2.0f, -0.8f);
        // On the side of the light nothing is shaded, whatever the pose.
        const Vec2 inFront(1.0f, 0.8f);

        bar.pose(entity, 0.0f);
        const Image upright = bar.render(fromAbove);
        CHECK(shownAt(upright, underTheTop.x, underTheTop.y) < 0.1f);
        CHECK(shownAt(upright, underTheTipped.x, underTheTipped.y) > 0.9f);
        CHECK(shownAt(upright, inFront.x, inFront.y) > 0.9f);
        CHECK(bar.statistics().shadowMaps > 0);
        CHECK(bar.statistics().shadowPass.drawCalls > 0);

        bar.scene.world.tryGet<AnimatorComponent>(entity)->time = 1.0f;
        const Image bent = bar.render(fromAbove);
        CHECK(shownAt(bent, underTheTipped.x, underTheTipped.y) < 0.1f);
        CHECK(shownAt(bent, underTheTop.x, underTheTop.y) > 0.9f);
        CHECK(shownAt(bent, inFront.x, inFront.y) > 0.9f);
        checkNoValidationIssues(*bar.scene.context.graphics);
    }
}

TEST_SUITE("SkinnedDraws") {
    TEST_CASE("a draw is skinned when its mesh is and it has a pose, and is not drawn without joint matrices") {
        const test::TempDirectory directory;
        REQUIRE(exportGltf(test::makeSkinnedBar(), directory.path() / "bar.gltf").has_value());
        test::RenderContext context;
        {
            const std::unique_ptr<AssetManager> assets =
                unwrap(AssetManager::create({.rootDirectory = directory.path()}));
            const std::unique_ptr<RenderResources> resources =
                unwrap(RenderResources::create(context.device(), *assets));
            const SkinPalettes palettes = unwrap(SkinPalettes::create(context.device()));
            const ViewBindings view = unwrap(ViewBindings::create(context.device()));

            // On the GPU a skinned mesh has, beside its vertices, how each follows its
            // joints, and a box for every joint.
            const AssetInfo* asset = assets->database().findByReference("bar.gltf#mesh/0");
            REQUIRE(asset != nullptr);
            const uint64_t bytesBefore = resources->statistics().meshBytes;
            const GpuMesh* bar = resources->mesh(asset->id);
            REQUIRE(bar != nullptr);
            CHECK(bar->isSkinned());
            CHECK(bar->jointBounds.size() == 3);
            const std::shared_ptr<const MeshData> data = unwrap(assets->loadMesh(asset->id));
            CHECK(resources->statistics().meshBytes - bytesBefore ==
                  data->vertices.size() * (sizeof(MeshVertex) + sizeof(MeshSkinVertex)) +
                      data->indices.size() * sizeof(uint16_t));
            const GpuMesh* cube = resources->mesh(builtinMeshId(BuiltinMesh::Cube));
            REQUIRE(cube != nullptr);
            CHECK_FALSE(cube->isSkinned());
            CHECK(cube->jointBounds.empty());

            const GpuMaterial& material = resources->material(Uuid());
            const std::vector<MeshDraw> draws = {
                {.mesh = bar, .material = &material, .palette = 6},
                // Without a pose it is drawn as the rigid mesh it then is ...
                {.mesh = bar, .material = &material},
                // ... and a pose is nothing to a mesh that follows no joints.
                {.mesh = cube, .material = &material, .palette = 6},
                {.mesh = bar, .material = &material, .palette = 9},
            };
            MeshDrawList list(context.device());
            MeshPassDesc desc;
            desc.debugName = "Test pass";
            desc.depth = MeshPassDepth::None;
            const MeshPassLayouts layouts{
                .view = view.layout(), .material = resources->materialLayout(), .joints = palettes.layout()};
            MeshPass pass = unwrap(MeshPass::create(context.device(), *context.shaders, layouts, desc));
            const nvrhi::TextureHandle target = test::createColorTarget(context.device(), 16, 16);
            const nvrhi::FramebufferHandle framebuffer =
                context.device()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

            test::recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                resources->flushUploads();
                list.build(commandList, draws, Vec3(0.0f), kWorldForward);
                // The rigid draws first, each mesh a batch; then the two that are
                // skinned, instances of one mesh in poses of their own.
                const std::span<const MeshBatch> batches = list.batches();
                REQUIRE(batches.size() == 3);
                CHECK(batches[0].mesh == bar);
                CHECK(batches[0].variant == 0);
                CHECK(batches[0].instanceCount == 1);
                CHECK(batches[1].mesh == cube);
                CHECK(batches[1].variant == 0);
                CHECK(batches[2].mesh == bar);
                CHECK(batches[2].variant == kMeshVariantSkinned);
                CHECK(batches[2].instanceCount == 2);

                // A skinned batch without the joint matrices is a mistake of the
                // caller, and is left out. (Nothing is bound before that is found,
                // so anything stands in for the view here.)
                const MeshPassBindings withoutJoints{.view = palettes.bindings(), .instances = list.instanceBuffer()};
#if defined(CORDITE_ENABLE_ASSERTS)
                CHECK_THROWS_AS(pass.draw(commandList, framebuffer, batches.subspan(2), withoutJoints),
                                test::AssertionFailure);
#else
                CHECK(pass.draw(commandList, framebuffer, batches.subspan(2), withoutJoints) == MeshPassStatistics{});
#endif
            });

            // A pass is made with the layouts of all three of its binding sets.
            for (const MeshPassLayouts& incomplete :
                 {MeshPassLayouts{.material = layouts.material, .joints = layouts.joints},
                  MeshPassLayouts{.view = layouts.view, .joints = layouts.joints},
                  MeshPassLayouts{.view = layouts.view, .material = layouts.material}}) {
#if defined(CORDITE_ENABLE_ASSERTS)
                CHECK_THROWS_AS((void)MeshPass::create(context.device(), *context.shaders, incomplete, desc),
                                test::AssertionFailure);
#else
                const Result<MeshPass> refused = MeshPass::create(context.device(), *context.shaders, incomplete, desc);
                REQUIRE_FALSE(refused.has_value());
                CHECK(refused.error().code == ErrorCode::InvalidArgument);
                CHECK(refused.error().message == "the pass 'Test pass' was given no device or not all of its layouts");
#endif
            }
        }
        context.graphics->waitIdle();
        checkNoValidationIssues(*context.graphics);
    }
}

TEST_SUITE("SkinPalettes") {
    TEST_CASE("every pose gets a place of its own among the joint matrices of a view") {
        test::RenderContext context;
        {
            SkinPalettes palettes = unwrap(SkinPalettes::create(context.device()));
            REQUIRE(palettes.layout() != nullptr);
            // Something to bind from the start.
            REQUIRE(palettes.bindings() != nullptr);
            CHECK(palettes.jointCount() == 0);
            CHECK(palettes.bufferBytes() > 0);

            const std::vector<Mat4> three(3, Mat4(1.0f));
            const std::vector<Mat4> five(5, Mat4(2.0f));
            CHECK(palettes.add(three) == 0);
            CHECK(palettes.add(five) == 3);
            CHECK(palettes.add(three) == 8);
            CHECK(palettes.jointCount() == 11);
            // No matrices are no pose.
            CHECK(palettes.add({}) == kNoPalette);
            CHECK(palettes.jointCount() == 11);

            test::recordAndExecute(context,
                                   [&](nvrhi::ICommandList* commandList) { CHECK(palettes.upload(commandList)); });

            palettes.clear();
            CHECK(palettes.jointCount() == 0);
            CHECK(palettes.add(five) == 0);
            // Nothing to send is nothing to do.
            palettes.clear();
            test::recordAndExecute(context,
                                   [&](nvrhi::ICommandList* commandList) { CHECK(palettes.upload(commandList)); });
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the buffer grows with what a view needs, and what was bound before stays alive") {
        test::RenderContext context;
        {
            SkinPalettes palettes = unwrap(SkinPalettes::create(context.device()));
            const nvrhi::BindingSetHandle before = palettes.bindings();
            const uint64_t bytesBefore = palettes.bufferBytes();

            // A crowd: more joints than the buffer was made for.
            const std::vector<Mat4> pose(64, Mat4(1.0f));
            for (uint32_t figure = 0; figure < 100; ++figure) {
                CHECK(palettes.add(pose) == figure * 64);
            }
            test::recordAndExecute(context,
                                   [&](nvrhi::ICommandList* commandList) { CHECK(palettes.upload(commandList)); });
            CHECK(palettes.bufferBytes() >= uint64_t{6400} * 48);
            CHECK(palettes.bufferBytes() > bytesBefore);
            CHECK(palettes.bindings() != before.Get());

            // A smaller view afterwards keeps the buffer.
            const uint64_t grown = palettes.bufferBytes();
            const nvrhi::IBindingSet* bindings = palettes.bindings();
            palettes.clear();
            CHECK(palettes.add(pose) == 0);
            test::recordAndExecute(context,
                                   [&](nvrhi::ICommandList* commandList) { CHECK(palettes.upload(commandList)); });
            CHECK(palettes.bufferBytes() == grown);
            CHECK(palettes.bindings() == bindings);
        }
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("a view is drawn with so many joint matrices and no more, and that is said once") {
        test::RenderContext context;
        {
            SkinPalettes palettes = unwrap(SkinPalettes::create(context.device()));
            const LogCapture log;
            const std::vector<Mat4> large(kMaxSkeletonJoints, Mat4(1.0f));
            uint32_t added = 0;
            while (palettes.add(large) != kNoPalette) {
                ++added;
            }
            CHECK(added == kMaxPaletteJoints / kMaxSkeletonJoints);
            CHECK(palettes.jointCount() == kMaxPaletteJoints);
            // Full for a pose of any size, and no fuller for having been asked.
            CHECK(palettes.add(std::vector<Mat4>(1, Mat4(1.0f))) == kNoPalette);
            CHECK(palettes.jointCount() == kMaxPaletteJoints);
            CHECK(log.count(LogLevel::Warn) == 1);
            CHECK(log.contains("a view is drawn with 1048576 joint matrices at the most"));

            // Room again for the next view.
            palettes.clear();
            CHECK(palettes.add(large) == 0);
        }
        checkNoValidationIssues(*context.graphics);
    }
}
