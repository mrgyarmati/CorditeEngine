#include "cordite/math/Projection.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/DebugDraw.h"
#include "cordite/renderer/DebugLinePass.h"
#include "cordite/renderer/RenderTarget.h"

#include "render/RenderTestSupport.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::readToImage;
using cordite::test::recordAndExecute;
using cordite::test::RenderContext;
using cordite::test::unwrap;

namespace {

constexpr UVec2 kTargetSize(480, 270);
constexpr Vec4 kBackground(0.10f, 0.11f, 0.13f, 1.0f);

/// A camera above and to the side of the origin, looking at it.
CameraView sceneView() {
    const Mat4 placement = glm::inverse(lookAt(Vec3(6.0f, 4.0f, 8.0f), Vec3(0.0f, 0.5f, 0.0f)));
    CameraComponent camera;
    camera.verticalFovDegrees = 50.0f;
    return makeCameraView(placement, camera, static_cast<float>(kTargetSize.x) / static_cast<float>(kTargetSize.y));
}

/// One of every shape, arranged so that none hides another.
DebugDraw sceneShapes() {
    DebugDraw lines;
    lines.axes(Mat4(1.0f), 2.0f);
    lines.box(Aabb::fromCenterExtents(Vec3(2.5f, 0.5f, -1.0f), Vec3(0.5f)), Mat4(1.0f), Vec4(1.0f, 0.8f, 0.2f, 1.0f));
    lines.sphere(Vec3(-2.5f, 1.0f, 1.0f), 1.0f, Vec4(0.3f, 0.9f, 0.9f, 1.0f));
    lines.cross(Vec3(0.0f, 2.5f, 0.0f), 0.6f, Vec4(1.0f));

    CameraComponent smallCamera;
    smallCamera.nearPlane = 0.3f;
    smallCamera.farPlane = 2.5f;
    const CameraView inner = makeCameraView(glm::translate(Mat4(1.0f), Vec3(0.0f, 1.0f, 4.0f)), smallCamera, 1.5f);
    lines.frustum(inner.inverseViewProjection, Vec4(0.9f, 0.4f, 0.9f, 1.0f));
    return lines;
}

RenderTarget makeTarget(RenderContext& context, bool withDepth) {
    return unwrap(RenderTarget::create(context.device(),
                                       {.size = kTargetSize, .withDepth = withDepth, .clearColor = kBackground}));
}

/// Draws the grid, fading towards the horizon, and then the shapes on top.
Image renderScene(RenderContext& context, DebugLinePass& pass, const RenderTarget& target, bool depthTest) {
    const CameraView view = sceneView();
    DebugDraw grid;
    grid.grid({.cellSize = 1.0f, .halfCellCount = 30});
    const DebugDraw shapes = sceneShapes();

    recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
        target.clear(commandList);
        pass.draw(commandList, target.framebuffer(), grid.vertices(),
                  {.viewProjection = view.viewProjection,
                   .depthTest = depthTest,
                   .fadeOrigin = view.position,
                   .fadeStart = 12.0f,
                   .fadeEnd = 24.0f});
        pass.draw(commandList, target.framebuffer(), shapes.vertices(),
                  {.viewProjection = view.viewProjection, .depthTest = depthTest});
    });
    return readToImage(*context.graphics, target.color());
}

} // namespace

TEST_SUITE("DebugLinePass") {
    TEST_CASE("debug shapes match the reference image") {
        RenderContext context;
        DebugLinePass pass = unwrap(DebugLinePass::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context, true);

        const Image image = renderScene(context, pass, target, true);
        checkAgainstReference(image, "debug-lines-480x270");

        // The grid fades out instead of ending at an edge: the horizon is background.
        const Vec4 horizon = image.pixel(240, 2);
        CHECK(horizon.r == doctest::Approx(kBackground.r).epsilon(0.05));
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("without anything in front of them, lines look the same with and without depth testing") {
        RenderContext context;
        DebugLinePass pass = unwrap(DebugLinePass::create(context.device(), *context.shaders));
        const RenderTarget withDepth = makeTarget(context, true);
        const RenderTarget withoutDepth = makeTarget(context, false);

        const Image tested = renderScene(context, pass, withDepth, true);
        const Image untested = renderScene(context, pass, withDepth, false);
        // A target without a depth buffer cannot be tested against, whatever is asked.
        const Image noDepthBuffer = renderScene(context, pass, withoutDepth, true);

        const ImageCompareOptions exact{.channelTolerance = 0, .maxDifferingRatio = 0.0};
        CHECK(unwrap(compareImages(tested, untested, exact)).matches);
        CHECK(unwrap(compareImages(tested, noDepthBuffer, exact)).matches);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("line colors and alpha reach the target unchanged") {
        RenderContext context;
        DebugLinePass pass = unwrap(DebugLinePass::create(context.device(), *context.shaders));
        const RenderTarget target =
            unwrap(RenderTarget::create(context.device(), {.size = {64, 64}, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}}));

        // With the default identity matrix, positions are clip-space coordinates. The two
        // horizontal lines run through the centers of pixel rows 16 and 48.
        const auto clipY = [](float pixelRow) { return 1.0f - 2.0f * (pixelRow + 0.5f) / 64.0f; };
        DebugDraw lines;
        lines.line(Vec3(-1.0f, clipY(16.0f), 0.5f), Vec3(1.0f, clipY(16.0f), 0.5f), Vec4(1.0f, 0.5f, 0.25f, 1.0f));
        lines.line(Vec3(-1.0f, clipY(48.0f), 0.5f), Vec3(1.0f, clipY(48.0f), 0.5f), Vec4(1.0f, 1.0f, 1.0f, 0.5f));

        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            target.clear(commandList);
            pass.draw(commandList, target.framebuffer(), lines.vertices(), {});
        });
        const Image image = readToImage(*context.graphics, target.color());

        // +Y is up: the opaque orange line is in the upper half.
        const Vec4 opaque = image.pixel(32, 16);
        CHECK(opaque.r == doctest::Approx(1.0f).epsilon(0.02));
        CHECK(opaque.g == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(opaque.b == doctest::Approx(0.25f).epsilon(0.03));
        // Half-transparent white over black.
        const Vec4 blended = image.pixel(32, 48);
        CHECK(blended.r == doctest::Approx(0.5f).epsilon(0.03));
        CHECK(blended.g == doctest::Approx(0.5f).epsilon(0.03));
        // Away from the lines nothing was drawn.
        CHECK(image.pixel(32, 32).r == doctest::Approx(0.0f));

        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("the vertex buffer grows for large amounts of lines and shrinking loads still work") {
        RenderContext context;
        DebugLinePass pass = unwrap(DebugLinePass::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context, true);
        const CameraView view = sceneView();

        // A deterministic cloud of far more lines than the buffer starts with.
        DebugDraw many;
        uint32_t state = 12345u;
        const auto next = [&state] {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 6.0f - 3.0f;
        };
        for (int index = 0; index < 20000; ++index) {
            many.line(Vec3(next(), next() + 3.0f, next()), Vec3(next(), next() + 3.0f, next()),
                      Vec4(1.0f, 1.0f, 1.0f, 0.2f));
        }
        const DebugDraw few = sceneShapes();

        for (const DebugDraw* lines : std::initializer_list<const DebugDraw*>{&few, &many, &few, &many}) {
            recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
                target.clear(commandList);
                pass.draw(commandList, target.framebuffer(), lines->vertices(),
                          {.viewProjection = view.viewProjection});
            });
        }

        // The cloud covers the middle of the image.
        const Vec4 center = readToImage(*context.graphics, target.color()).pixel(240, 100);
        CHECK(center.r > kBackground.r + 0.1f);
        checkNoValidationIssues(*context.graphics);
    }

    TEST_CASE("drawing no lines changes nothing") {
        RenderContext context;
        DebugLinePass pass = unwrap(DebugLinePass::create(context.device(), *context.shaders));
        const RenderTarget target = makeTarget(context, false);

        recordAndExecute(context, [&](nvrhi::ICommandList* commandList) {
            target.clear(commandList);
            pass.draw(commandList, target.framebuffer(), {}, {});
        });
        const Vec4 pixel = readToImage(*context.graphics, target.color()).pixel(10, 10);
        CHECK(pixel.r == doctest::Approx(kBackground.r).epsilon(0.02));
        checkNoValidationIssues(*context.graphics);
    }
}
