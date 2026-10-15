#include "cordite/editor/Gizmo.h"

#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace cordite;

namespace {

constexpr Vec2 kViewport(800.0f, 600.0f);

/// A camera at +Z looking down -Z at the origin, without perspective: 10 metres of the
/// world are the 600 pixels of the viewport's height, so a metre is 60 pixels everywhere.
CameraView frontView() {
    CameraComponent lens;
    lens.projection = CameraProjection::Orthographic;
    lens.orthographicHeight = 10.0f;
    return makeCameraView(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 10.0f)), lens, kViewport.x / kViewport.y);
}

/// The same camera with perspective, 60 degrees from top to bottom.
CameraView perspectiveView(const Vec3& position) {
    CameraComponent lens;
    lens.verticalFovDegrees = 60.0f;
    return makeCameraView(glm::translate(Mat4(1.0f), position), lens, kViewport.x / kViewport.y);
}

constexpr Vec2 kCenter(400.0f, 300.0f);
constexpr float kPixelsPerMetre = 60.0f;

GizmoFrame frameAtOrigin(GizmoMode mode = GizmoMode::Translate) {
    return gizmoFrame(Mat4(1.0f), GizmoSpace::World, mode, frontView(), kViewport);
}

} // namespace

TEST_SUITE("Gizmo") {
    TEST_CASE("modes and spaces have names that lead back to them") {
        for (const GizmoMode mode : kGizmoModes) {
            CHECK(gizmoModeFromName(nameOf(mode)) == mode);
        }
        CHECK(gizmoModeFromName("Rotate") == GizmoMode::Rotate);
        CHECK_FALSE(gizmoModeFromName("move").has_value());
        CHECK(gizmoSpaceFromName("LOCAL") == GizmoSpace::Local);
        CHECK(gizmoSpaceFromName(nameOf(GizmoSpace::World)) == GizmoSpace::World);
        CHECK_FALSE(gizmoSpaceFromName("parent").has_value());
    }

    TEST_CASE("a gizmo is as large on the screen wherever it is") {
        // Without perspective, a metre is as many pixels near and far.
        const GizmoFrame here = frameAtOrigin();
        CHECK(here.size == doctest::Approx(kGizmoScreenSize / kPixelsPerMetre));
        const GizmoFrame there = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(3.0f, 1.0f, -40.0f)), GizmoSpace::World,
                                            GizmoMode::Translate, frontView(), kViewport);
        CHECK(there.size == doctest::Approx(here.size));

        // With perspective, twice as far is twice as large in metres.
        const CameraView view = perspectiveView(Vec3(0.0f));
        const GizmoFrame near = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -5.0f)), GizmoSpace::World,
                                           GizmoMode::Translate, view, kViewport);
        const GizmoFrame far = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -10.0f)), GizmoSpace::World,
                                          GizmoMode::Translate, view, kViewport);
        CHECK(far.size == doctest::Approx(near.size * 2.0f));
        // Where its axis ends is kGizmoScreenSize pixels from its centre.
        const Vec2 center = *worldToScreen(view, near.origin, kViewport);
        const Vec2 tip = *worldToScreen(view, near.origin + near.y * near.size, kViewport);
        CHECK(glm::length(tip - center) == doctest::Approx(kGizmoScreenSize).epsilon(0.001));
        // Off to the side it is as large: the depth it is at counts, not how far away it is.
        const GizmoFrame aside = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(6.0f, 0.0f, -5.0f)), GizmoSpace::World,
                                            GizmoMode::Translate, view, kViewport);
        CHECK(aside.size == doctest::Approx(near.size));
        const Vec2 asideCenter = *worldToScreen(view, aside.origin, kViewport);
        const Vec2 asideTip = *worldToScreen(view, aside.origin + aside.y * aside.size, kViewport);
        CHECK(glm::length(asideTip - asideCenter) == doctest::Approx(kGizmoScreenSize).epsilon(0.001));
        // Larger interfaces have larger gizmos.
        CHECK(gizmoFrame(Mat4(1.0f), GizmoSpace::World, GizmoMode::Translate, frontView(), kViewport, 2.0f).size ==
              doctest::Approx(here.size * 2.0f));
    }

    TEST_CASE("the axes are the world's, or the entity's own without its scale") {
        const Mat4 placed = glm::translate(Mat4(1.0f), Vec3(1.0f, 2.0f, 3.0f)) *
                            glm::mat4_cast(glm::angleAxis(kPi / 2.0f, Vec3(0.0f, 1.0f, 0.0f))) *
                            glm::scale(Mat4(1.0f), Vec3(3.0f, 0.5f, 2.0f));
        const GizmoFrame world = gizmoFrame(placed, GizmoSpace::World, GizmoMode::Translate, frontView(), kViewport);
        CHECK_NEAR(world.origin, Vec3(1.0f, 2.0f, 3.0f), 1.0e-4f);
        CHECK_NEAR(world.x, Vec3(1.0f, 0.0f, 0.0f), 1.0e-4f);
        CHECK_NEAR(world.z, Vec3(0.0f, 0.0f, 1.0f), 1.0e-4f);

        // Turned a quarter about Y: its X points along the world's -Z, its Z along +X.
        const GizmoFrame local = gizmoFrame(placed, GizmoSpace::Local, GizmoMode::Translate, frontView(), kViewport);
        CHECK_NEAR(local.x, Vec3(0.0f, 0.0f, -1.0f), 1.0e-4f);
        CHECK_NEAR(local.y, Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);
        CHECK_NEAR(local.z, Vec3(1.0f, 0.0f, 0.0f), 1.0e-4f);
        // Scale is along the entity's own axes, whatever space is asked for.
        const GizmoFrame scale = gizmoFrame(placed, GizmoSpace::World, GizmoMode::Scale, frontView(), kViewport);
        CHECK_NEAR(scale.x, local.x, 1.0e-4f);

        // An entity scaled to nothing along an axis has no axes of its own to show.
        const GizmoFrame flat = gizmoFrame(glm::scale(Mat4(1.0f), Vec3(1.0f, 0.0f, 1.0f)), GizmoSpace::Local,
                                           GizmoMode::Translate, frontView(), kViewport);
        const GizmoFrame thin = gizmoFrame(glm::scale(Mat4(1.0f), Vec3(1.0f, 1.0f, 0.0f)), GizmoSpace::Local,
                                           GizmoMode::Translate, frontView(), kViewport);
        CHECK_NEAR(thin.z, Vec3(0.0f, 0.0f, 1.0f), 1.0e-6f);
        CHECK_NEAR(flat.y, Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);
    }

    TEST_CASE("the handle under the pointer is the one it is nearest to") {
        const GizmoFrame frame = frameAtOrigin();
        const CameraView view = frontView();
        const float axis = frame.size * kPixelsPerMetre;
        // Along X, to the right; along Y, up the screen; Z points at the camera, and is seen end on.
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, kCenter + Vec2(axis * 0.8f, 3.0f)) ==
              GizmoHandle::X);
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, kCenter + Vec2(-2.0f, -axis * 0.7f)) ==
              GizmoHandle::Y);
        // Inside the square between X and Y.
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, kCenter + Vec2(axis * 0.35f, -axis * 0.35f)) ==
              GizmoHandle::XY);
        // Too far from anything.
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, kCenter + Vec2(axis * 0.6f, axis * 0.6f)) ==
              GizmoHandle::None);
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport,
                         kCenter + Vec2(axis * 1.2f + kGizmoPickPixels * 2.0f, 0.0f)) == GizmoHandle::None);
        // Within the distance, which grows with the interface.
        const Vec2 beside = kCenter + Vec2(axis * 0.5f, kGizmoPickPixels * 1.5f);
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, beside) == GizmoHandle::None);
        CHECK(pickHandle(GizmoMode::Translate, frame, view, kViewport, beside, 2.0f) == GizmoHandle::X);
    }

    TEST_CASE("the rings of rotation and the boxes of scale are handles") {
        const CameraView view = frontView();
        const GizmoFrame rotate = frameAtOrigin(GizmoMode::Rotate);
        const float radius = rotate.size * kPixelsPerMetre;
        // The ring about Z faces the camera: a circle on the screen.
        CHECK(pickHandle(GizmoMode::Rotate, rotate, view, kViewport,
                         kCenter + Vec2(radius * 0.7071f, radius * 0.7071f) + Vec2(2.0f, 2.0f)) == GizmoHandle::Z);
        CHECK(pickHandle(GizmoMode::Rotate, rotate, view, kViewport, kCenter + Vec2(radius * 0.4f, radius * 0.3f)) ==
              GizmoHandle::None);

        const GizmoFrame scale = frameAtOrigin(GizmoMode::Scale);
        CHECK(pickHandle(GizmoMode::Scale, scale, view, kViewport, kCenter) == GizmoHandle::All);
        CHECK(pickHandle(GizmoMode::Scale, scale, view, kViewport,
                         kCenter + Vec2(scale.size * kPixelsPerMetre, 0.0f)) == GizmoHandle::X);
    }

    TEST_CASE("an axis that is held moves along itself as far as the pointer does") {
        const CameraView view = frontView();
        const GizmoFrame frame = frameAtOrigin();
        const Vec2 grab = kCenter + Vec2(40.0f, 0.0f);
        const GizmoDrag drag = beginDrag(GizmoMode::Translate, GizmoHandle::X, frame, view, kViewport, grab);
        // 120 pixels right and some up: two metres along X, nothing else.
        const Vec3 moved = dragTranslation(drag, view, kViewport, grab + Vec2(120.0f, -50.0f));
        CHECK_NEAR(moved, Vec3(2.0f, 0.0f, 0.0f), 1.0e-4f);
        CHECK_NEAR(dragTranslation(drag, view, kViewport, grab), Vec3(0.0f), 1.0e-4f);

        // Across a plane, both ways - taken hold of at its square, which moves with the pointer.
        const float axis = frame.size * kPixelsPerMetre;
        const Vec2 square = kCenter + Vec2(axis * 0.35f, -axis * 0.35f);
        const GizmoDrag atSquare = beginDrag(GizmoMode::Translate, GizmoHandle::XY, frame, view, kViewport, square);
        CHECK_NEAR(dragTranslation(atSquare, view, kViewport, square + Vec2(60.0f, 30.0f)), Vec3(1.0f, -0.5f, 0.0f),
                   1.0e-4f);
        const GizmoDrag plane = beginDrag(GizmoMode::Translate, GizmoHandle::XY, frame, view, kViewport, kCenter);
        CHECK_NEAR(dragTranslation(plane, view, kViewport, kCenter + Vec2(60.0f, 30.0f)), Vec3(1.0f, -0.5f, 0.0f),
                   1.0e-4f);

        // With perspective, the handle stays under the pointer: moved along the axis as it is
        // seen, and sideways as far as one likes, it is where the pointer's ray meets the axis.
        const CameraView seen = perspectiveView(Vec3(0.0f, 0.0f, 10.0f));
        const GizmoFrame raised = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(0.0f, 1.0f, 0.0f)), GizmoSpace::World,
                                             GizmoMode::Translate, seen, kViewport);
        const Vec2 onAxis = *worldToScreen(seen, raised.origin + raised.x * raised.size * 0.5f, kViewport);
        const GizmoDrag held = beginDrag(GizmoMode::Translate, GizmoHandle::X, raised, seen, kViewport, onAxis);
        const Vec3 slid = dragTranslation(held, seen, kViewport, onAxis + Vec2(90.0f, 70.0f));
        const Vec2 handle = *worldToScreen(seen, raised.origin + raised.x * raised.size * 0.5f + slid, kViewport);
        CHECK(handle.x == doctest::Approx(onAxis.x + 90.0f).epsilon(0.0005));
        CHECK(slid.y == doctest::Approx(0.0f));

        // Where the ray runs along the axis - at the point the axis vanishes into - where along
        // it the pointer is cannot be told either: the handle stays where it is.
        const CameraView ahead = perspectiveView(Vec3(0.0f));
        const GizmoFrame below = gizmoFrame(glm::translate(Mat4(1.0f), Vec3(0.0f, -2.0f, -10.0f)), GizmoSpace::World,
                                            GizmoMode::Translate, ahead, kViewport);
        const Vec2 alongZ = *worldToScreen(ahead, below.origin + below.z * below.size * 0.5f, kViewport);
        const GizmoDrag depth = beginDrag(GizmoMode::Translate, GizmoHandle::Z, below, ahead, kViewport, alongZ);
        const Vec3 vanished = dragTranslation(depth, ahead, kViewport, kCenter);
        CHECK(std::isfinite(vanished.z));
        CHECK_NEAR(vanished, Vec3(0.0f), 1.0e-4f);

        // Z points at the camera: where along it the pointer is cannot be told.
        const GizmoDrag endOn = beginDrag(GizmoMode::Translate, GizmoHandle::Z, frame, view, kViewport, kCenter);
        CHECK_NEAR(dragTranslation(endOn, view, kViewport, kCenter + Vec2(80.0f, 0.0f)), Vec3(0.0f), 1.0e-4f);
    }

    TEST_CASE("a ring that is held turns as the pointer goes round the centre") {
        const CameraView view = frontView();
        const GizmoFrame frame = frameAtOrigin(GizmoMode::Rotate);
        // From the right of the centre to above it: a quarter turn the other way to the clock,
        // as seen - about +Z, which points at the viewer.
        const GizmoDrag drag =
            beginDrag(GizmoMode::Rotate, GizmoHandle::Z, frame, view, kViewport, kCenter + Vec2(100.0f, 0.0f));
        const Quat turned = dragRotation(drag, view, kViewport, kCenter + Vec2(0.0f, -100.0f));
        CHECK_NEAR(turned * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 1.0e-4f);
        CHECK_NEAR(dragRotation(drag, view, kViewport, kCenter + Vec2(50.0f, 0.0f)) * Vec3(1.0f, 0.0f, 0.0f),
                   Vec3(1.0f, 0.0f, 0.0f), 1.0e-4f);

        // Seen from behind its axis, the same sweep turns the other way about it.
        CameraComponent lens;
        lens.projection = CameraProjection::Orthographic;
        lens.orthographicHeight = 10.0f;
        const CameraView behind = makeCameraView(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -10.0f)) *
                                                     glm::mat4_cast(glm::angleAxis(kPi, kWorldUp)),
                                                 lens, kViewport.x / kViewport.y);
        const GizmoDrag back =
            beginDrag(GizmoMode::Rotate, GizmoHandle::Z, frame, behind, kViewport, kCenter + Vec2(100.0f, 0.0f));
        CHECK_NEAR(dragRotation(back, behind, kViewport, kCenter + Vec2(0.0f, -100.0f)) * Vec3(1.0f, 0.0f, 0.0f),
                   Vec3(0.0f, -1.0f, 0.0f), 1.0e-4f);
        // At the centre there is no angle.
        CHECK_NEAR(dragRotation(drag, view, kViewport, kCenter) * Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f),
                   1.0e-4f);
    }

    TEST_CASE("a scale handle that is held scales by how far it is pulled, never to nothing") {
        const CameraView view = frontView();
        const GizmoFrame frame = frameAtOrigin(GizmoMode::Scale);
        const float axis = frame.size * kPixelsPerMetre;
        const Vec2 grab = kCenter + Vec2(axis, 0.0f);
        const GizmoDrag drag = beginDrag(GizmoMode::Scale, GizmoHandle::X, frame, view, kViewport, grab);
        // Pulled out by one length of the axis: twice as large along X alone.
        CHECK_NEAR(dragScale(drag, view, kViewport, grab + Vec2(axis, 0.0f)), Vec3(2.0f, 1.0f, 1.0f), 1.0e-4f);
        CHECK_NEAR(dragScale(drag, view, kViewport, grab + Vec2(-axis * 0.5f, 40.0f)), Vec3(0.5f, 1.0f, 1.0f), 1.0e-4f);
        CHECK(dragScale(drag, view, kViewport, grab + Vec2(-axis * 3.0f, 0.0f)).x == kGizmoMinimumScale);

        // Along Y, up the screen.
        const Vec2 top = kCenter + Vec2(0.0f, -axis);
        const GizmoDrag upward = beginDrag(GizmoMode::Scale, GizmoHandle::Y, frame, view, kViewport, top);
        CHECK_NEAR(dragScale(upward, view, kViewport, top + Vec2(15.0f, -axis)), Vec3(1.0f, 2.0f, 1.0f), 1.0e-4f);

        // The centre scales along all three, larger up and to the right.
        const GizmoDrag all = beginDrag(GizmoMode::Scale, GizmoHandle::All, frame, view, kViewport, kCenter);
        const Vec3 larger = dragScale(all, view, kViewport, kCenter + Vec2(axis * 0.7071f, -axis * 0.7071f));
        CHECK_NEAR(larger, Vec3(2.0f), 1.0e-4f);
        CHECK(dragScale(all, view, kViewport, kCenter + Vec2(-axis, axis)).y == kGizmoMinimumScale);
    }

    TEST_CASE("a gizmo is drawn with the handle that is held in the color of the selection") {
        DebugDraw plain;
        drawGizmo(plain, GizmoMode::Translate, frameAtOrigin(), GizmoHandle::None);
        DebugDraw held;
        drawGizmo(held, GizmoMode::Translate, frameAtOrigin(), GizmoHandle::X);
        REQUIRE_FALSE(plain.vertices().empty());
        const auto colorsOf = [](const DebugDraw& lines) {
            std::vector<uint32_t> colors;
            for (const DebugVertex& vertex : lines.vertices()) {
                colors.push_back(vertex.color);
            }
            return colors;
        };
        CHECK(plain.vertices().size() == held.vertices().size());
        CHECK(colorsOf(plain) != colorsOf(held));
        for (const GizmoMode mode : kGizmoModes) {
            DebugDraw lines;
            drawGizmo(lines, mode, frameAtOrigin(mode), GizmoHandle::None);
            CHECK_FALSE(lines.vertices().empty());
        }
    }
}
