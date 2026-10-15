#include "cordite/editor/EditorCamera.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

using namespace cordite;

namespace {

constexpr float kFrameSeconds = 0.1f;

/// A camera at the origin looking along -Z, with round numbers for speeds.
EditorCamera levelCamera() {
    EditorCamera camera;
    camera.setPosition(Vec3(0.0f));
    camera.setOrientation(0.0f, 0.0f);
    camera.moveSpeed = 10.0f;
    camera.fastMultiplier = 3.0f;
    camera.lookSensitivity = 0.5f;
    return camera;
}

/// Starts an input frame with the pointer resting at a known place.
Input restingInput() {
    Input input;
    input.injectMouseMove(Vec2(100.0f, 100.0f));
    input.beginFrame();
    return input;
}

/// Presses a button during one frame and leaves it held for the frames after.
void press(Input& input, EditorCamera& camera, MouseButton button, bool viewportHovered = true) {
    input.injectMouseButton(button, true);
    camera.update(input, kFrameSeconds, viewportHovered);
    input.beginFrame();
}

} // namespace

TEST_SUITE("EditorCamera") {
    TEST_CASE("a new camera looks down at the origin from a corner") {
        const EditorCamera camera;
        const Vec3 toOrigin = glm::normalize(Vec3(0.0f, 0.5f, 0.0f) - camera.position());
        CHECK(glm::dot(camera.forward(), toOrigin) > 0.99f);
        CHECK_FALSE(camera.isNavigating());
    }

    TEST_CASE("yaw turns left and pitch looks up") {
        EditorCamera camera = levelCamera();
        CHECK_NEAR(camera.forward(), Vec3(0.0f, 0.0f, -1.0f), 1e-6f);

        camera.setOrientation(90.0f, 0.0f);
        CHECK_NEAR(camera.forward(), Vec3(-1.0f, 0.0f, 0.0f), 1e-6f);

        camera.setOrientation(0.0f, 45.0f);
        CHECK(camera.forward().y == doctest::Approx(std::sqrt(0.5f)));
        CHECK(camera.forward().z < 0.0f);
    }

    TEST_CASE("the orientation is kept within sensible bounds") {
        EditorCamera camera = levelCamera();
        camera.setOrientation(190.0f, 120.0f);
        CHECK(camera.yawDegrees() == doctest::Approx(-170.0f));
        CHECK(camera.pitchDegrees() == doctest::Approx(89.0f));

        camera.setOrientation(-540.0f, -500.0f);
        CHECK(camera.yawDegrees() == doctest::Approx(-180.0f));
        CHECK(camera.pitchDegrees() == doctest::Approx(-89.0f));
    }

    TEST_CASE("lookAt points the camera at a target") {
        EditorCamera camera;
        for (const Vec3 target :
             {Vec3(0.0f), Vec3(10.0f, 2.0f, 3.0f), Vec3(-4.0f, 9.0f, 20.0f), Vec3(5.0f, -30.0f, 25.0f)}) {
            const Vec3 position(5.0f, 3.0f, 5.0f);
            camera.lookAt(position, target);
            CHECK_NEAR(camera.position(), position, 0.0f);
            CHECK_NEAR(camera.forward(), glm::normalize(target - position), 1e-4f);
        }

        // Straight down is out of reach: the camera stops just short of vertical.
        camera.lookAt(Vec3(0.0f, 10.0f, 0.0f), Vec3(0.0f));
        CHECK(camera.pitchDegrees() == doctest::Approx(-89.0f));

        // Looking at where it stands keeps a usable orientation.
        camera.lookAt(Vec3(1.0f), Vec3(1.0f));
        CHECK(glm::length(camera.forward()) == doctest::Approx(1.0f));
    }

    TEST_CASE("the camera matrix and the view agree with position and direction") {
        EditorCamera camera;
        camera.lookAt(Vec3(3.0f, 2.0f, 1.0f), Vec3(0.0f));

        const Mat4 matrix = camera.cameraToWorld();
        CHECK_NEAR(Vec3(matrix[3]), camera.position(), 1e-6f);
        CHECK_NEAR(-Vec3(matrix[2]), camera.forward(), 1e-5f);

        const CameraView view = camera.view(16.0f / 9.0f);
        CHECK_NEAR(view.position, camera.position(), 1e-5f);
        CHECK_NEAR(view.forward, camera.forward(), 1e-5f);
        CHECK(view.nearPlane == camera.lens().nearPlane);
        // What it looks at is in the middle of the picture.
        const std::optional<Vec2> center = worldToScreen(view, Vec3(0.0f), Vec2(160.0f, 90.0f));
        REQUIRE(center.has_value());
        CHECK_NEAR(*center, Vec2(80.0f, 45.0f), 0.01f);
    }

    TEST_CASE("framing moves the camera until a sphere fills the view") {
        EditorCamera camera = levelCamera();
        camera.lens().verticalFovDegrees = 60.0f;
        camera.frame(Vec3(0.0f, 0.0f, -50.0f), 2.0f);

        // The direction is kept; the sphere is straight ahead, a little more than
        // radius / sin(30 degrees) away.
        CHECK_NEAR(camera.forward(), Vec3(0.0f, 0.0f, -1.0f), 1e-6f);
        const float distance = glm::distance(camera.position(), Vec3(0.0f, 0.0f, -50.0f));
        CHECK(distance == doctest::Approx(4.8f));
        CHECK(camera.position().x == doctest::Approx(0.0f));

        // A point still gets a sensible distance.
        camera.frame(Vec3(0.0f), 0.0f);
        CHECK(glm::length(camera.position()) > 0.0f);
    }

    TEST_CASE("holding the right button turns the camera with the pointer") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();
        press(input, camera, MouseButton::Right);
        CHECK(camera.isNavigating());

        // 20 pixels to the right and 10 down, at half a degree per pixel.
        input.injectMouseMove(Vec2(120.0f, 110.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.yawDegrees() == doctest::Approx(-10.0f));
        CHECK(camera.pitchDegrees() == doctest::Approx(-5.0f));
        CHECK(camera.forward().x > 0.0f);
        CHECK(camera.forward().y < 0.0f);

        input.beginFrame();
        input.injectMouseButton(MouseButton::Right, false);
        camera.update(input, kFrameSeconds, true);
        CHECK_FALSE(camera.isNavigating());

        // Without the button the pointer does nothing.
        input.beginFrame();
        input.injectMouseMove(Vec2(300.0f, 300.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.yawDegrees() == doctest::Approx(-10.0f));
    }

    TEST_CASE("looking cannot tip over the vertical") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();
        press(input, camera, MouseButton::Right);

        input.injectMouseMove(Vec2(100.0f, -5000.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.pitchDegrees() == doctest::Approx(89.0f));
    }

    TEST_CASE("the keys fly the camera while the right button is held") {
        struct Case {
            Key key;
            Vec3 expected;
        };

        for (const Case& entry : {Case{Key::W, Vec3(0.0f, 0.0f, -1.0f)}, Case{Key::S, Vec3(0.0f, 0.0f, 1.0f)},
                                  Case{Key::D, Vec3(1.0f, 0.0f, 0.0f)}, Case{Key::A, Vec3(-1.0f, 0.0f, 0.0f)},
                                  Case{Key::E, Vec3(0.0f, 1.0f, 0.0f)}, Case{Key::Q, Vec3(0.0f, -1.0f, 0.0f)}}) {
            CAPTURE(static_cast<int>(entry.key));
            EditorCamera camera = levelCamera();
            Input input = restingInput();
            press(input, camera, MouseButton::Right);

            input.injectKey(entry.key, true);
            camera.update(input, kFrameSeconds, true);
            // 10 m/s for a tenth of a second.
            CHECK_NEAR(camera.position(), entry.expected, 1e-5f);
        }
    }

    TEST_CASE("flying follows the direction the camera faces, at a steady speed") {
        EditorCamera camera = levelCamera();
        camera.setOrientation(90.0f, 0.0f);
        Input input = restingInput();
        press(input, camera, MouseButton::Right);

        // Forwards and sideways together are no faster than either alone.
        input.injectKey(Key::W, true);
        input.injectKey(Key::D, true);
        camera.update(input, kFrameSeconds, true);
        CHECK(glm::length(camera.position()) == doctest::Approx(1.0f));
        // Facing -X: forwards is -X and right is -Z.
        CHECK(camera.position().x == doctest::Approx(-std::sqrt(0.5f)));
        CHECK(camera.position().z == doctest::Approx(-std::sqrt(0.5f)));

        // Opposite keys cancel.
        EditorCamera still = levelCamera();
        Input opposite = restingInput();
        press(opposite, still, MouseButton::Right);
        opposite.injectKey(Key::W, true);
        opposite.injectKey(Key::S, true);
        still.update(opposite, kFrameSeconds, true);
        CHECK_NEAR(still.position(), Vec3(0.0f), 0.0f);
    }

    TEST_CASE("shift flies faster") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();
        press(input, camera, MouseButton::Right);

        input.injectKey(Key::W, true);
        input.injectKey(Key::LeftShift, true);
        camera.update(input, kFrameSeconds, true);
        CHECK_NEAR(camera.position(), Vec3(0.0f, 0.0f, -3.0f), 1e-5f);
    }

    TEST_CASE("the keys alone do not move the camera") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();
        input.injectKey(Key::W, true);
        camera.update(input, kFrameSeconds, true);
        CHECK_NEAR(camera.position(), Vec3(0.0f), 0.0f);
    }

    TEST_CASE("a movement starts only over the viewport but continues outside it") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();

        // Pressed while the pointer is elsewhere: the interface gets the click.
        const bool notHovered = false;
        press(input, camera, MouseButton::Right, notHovered);
        CHECK_FALSE(camera.isNavigating());
        input.injectMouseMove(Vec2(150.0f, 100.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.yawDegrees() == doctest::Approx(0.0f));

        input.beginFrame();
        input.injectMouseButton(MouseButton::Right, false);
        camera.update(input, kFrameSeconds, true);
        input.beginFrame();

        // Pressed over the viewport, then dragged off it.
        press(input, camera, MouseButton::Right);
        input.injectMouseMove(Vec2(170.0f, 100.0f));
        camera.update(input, kFrameSeconds, notHovered);
        CHECK(camera.isNavigating());
        CHECK(camera.yawDegrees() == doctest::Approx(-10.0f));
    }

    TEST_CASE("holding the middle button slides the camera against the pointer") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();
        press(input, camera, MouseButton::Middle);
        CHECK(camera.isNavigating());

        // 50 pixels right and 25 down at 10 m/s * 0.002 = 2 cm per pixel.
        input.injectMouseMove(Vec2(150.0f, 125.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK_NEAR(camera.position(), Vec3(-1.0f, 0.5f, 0.0f), 1e-5f);
        // Sliding does not turn.
        CHECK(camera.yawDegrees() == doctest::Approx(0.0f));
    }

    TEST_CASE("the wheel moves the camera along its direction over the viewport") {
        EditorCamera camera = levelCamera();
        Input input = restingInput();

        input.injectScroll(Vec2(0.0f, 2.0f));
        camera.update(input, kFrameSeconds, true);
        // Two notches at 10 m/s * 0.2 = 2 m each, independent of the frame time.
        CHECK_NEAR(camera.position(), Vec3(0.0f, 0.0f, -4.0f), 1e-5f);

        input.beginFrame();
        input.injectScroll(Vec2(0.0f, -1.0f));
        const bool notHovered = false;
        camera.update(input, kFrameSeconds, notHovered);
        CHECK_NEAR(camera.position(), Vec3(0.0f, 0.0f, -4.0f), 1e-5f);
    }
    TEST_CASE("Alt and the left button turn the camera around what it looks at") {
        EditorCamera camera;
        camera.lookSensitivity = 0.5f;
        camera.lookAt(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        CHECK(camera.focusDistance() == doctest::Approx(10.0f));
        Input input = restingInput();

        // Without Alt the left button is not the camera's.
        press(input, camera, MouseButton::Left);
        CHECK_FALSE(camera.isNavigating());
        input.injectMouseButton(MouseButton::Left, false);
        input.beginFrame();

        input.injectKey(Key::LeftAlt, true);
        press(input, camera, MouseButton::Left);
        CHECK(camera.isNavigating());
        CHECK(camera.isOrbiting());
        // 40 pixels to the right: twenty degrees round, at half a degree a pixel.
        input.injectMouseMove(Vec2(140.0f, 100.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.yawDegrees() == doctest::Approx(-20.0f));
        // Still ten metres from the origin, and looking at it.
        CHECK(glm::length(camera.position()) == doctest::Approx(10.0f));
        CHECK_NEAR(camera.position() + camera.forward() * camera.focusDistance(), Vec3(0.0f), 1e-4f);
        // Dragged right, it goes round to the left: the scene is seen turned to the right.
        CHECK(camera.position().x < -3.0f);

        // Let go, it stops; held where it began, nothing begins.
        input.beginFrame();
        input.injectMouseButton(MouseButton::Left, false);
        camera.update(input, kFrameSeconds, true);
        CHECK_FALSE(camera.isNavigating());
        CHECK_FALSE(camera.isOrbiting());
        input.beginFrame();
        press(input, camera, MouseButton::Left, false);
        CHECK_FALSE(camera.isNavigating());

        // Looking about with the right button, Alt or not, is no orbit.
        input.injectMouseButton(MouseButton::Left, false);
        input.beginFrame();
        press(input, camera, MouseButton::Right);
        CHECK(camera.isNavigating());
        CHECK_FALSE(camera.isOrbiting());
    }

    TEST_CASE("framing and looking at choose what the camera turns around, and the wheel nears it") {
        EditorCamera camera;
        camera.frame(Vec3(5.0f, 1.0f, -3.0f), 2.0f);
        CHECK(camera.focusDistance() == doctest::Approx(glm::distance(camera.position(), Vec3(5.0f, 1.0f, -3.0f))));
        camera.lookAt(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        CHECK(camera.focusDistance() == doctest::Approx(10.0f));
        // Looking at where it is, it turns around a point just ahead.
        camera.lookAt(Vec3(1.0f), Vec3(1.0f));
        CHECK(camera.focusDistance() > 0.0f);

        // Two notches: four metres nearer. Past it, the point is a little further on.
        camera.moveSpeed = 10.0f;
        camera.lookAt(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f));
        Input input = restingInput();
        input.injectScroll(Vec2(0.0f, 2.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.focusDistance() == doctest::Approx(6.0f));
        input.beginFrame();
        input.injectScroll(Vec2(0.0f, 5.0f));
        camera.update(input, kFrameSeconds, true);
        CHECK(camera.position().z == doctest::Approx(-4.0f));
        CHECK(camera.focusDistance() > 0.0f);
        CHECK(camera.focusDistance() < 0.5f);
    }
}
