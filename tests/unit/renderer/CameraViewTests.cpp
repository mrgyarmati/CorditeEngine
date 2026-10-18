#include "cordite/renderer/CameraView.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/animation/CameraShake.h"
#include "cordite/math/Transform.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>

using namespace cordite;

namespace {

/// Where a world-space point lands in normalized device coordinates (x, y in [-1, 1],
/// depth in [0, 1]).
Vec3 project(const CameraView& view, const Vec3& point) {
    const Vec4 clip = view.viewProjection * Vec4(point, 1.0f);
    return Vec3(clip) / clip.w;
}

bool isFinite(const Mat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

Mat4 placedAt(const Vec3& position, const Quat& rotation = kIdentityQuat, const Vec3& scale = Vec3(1.0f)) {
    return Transform{position, rotation, scale}.toMatrix();
}

} // namespace

TEST_SUITE("CameraView") {
    TEST_CASE("a camera looks along its -Z axis with +Y up") {
        const CameraView view = makeCameraView(Mat4(1.0f), CameraComponent(), 16.0f / 9.0f);

        CHECK_NEAR(view.position, Vec3(0.0f), 0.0f);
        CHECK_NEAR(view.forward, Vec3(0.0f, 0.0f, -1.0f), 1e-6f);

        // Straight ahead is the center of the image; up is up and right is right.
        CHECK_NEAR(Vec2(project(view, Vec3(0.0f, 0.0f, -10.0f))), Vec2(0.0f), 1e-6f);
        CHECK(project(view, Vec3(0.0f, 1.0f, -10.0f)).y > 0.0f);
        CHECK(project(view, Vec3(1.0f, 0.0f, -10.0f)).x > 0.0f);
    }

    TEST_CASE("depth is reversed: one at the near plane and zero at the far plane") {
        CameraComponent camera;
        camera.nearPlane = 0.5f;
        camera.farPlane = 200.0f;
        const CameraView view = makeCameraView(Mat4(1.0f), camera, 1.0f);

        CHECK(view.nearPlane == 0.5f);
        CHECK(view.farPlane == 200.0f);
        CHECK(project(view, Vec3(0.0f, 0.0f, -0.5f)).z == doctest::Approx(1.0f));
        CHECK(project(view, Vec3(0.0f, 0.0f, -200.0f)).z == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(project(view, Vec3(0.0f, 0.0f, -10.0f)).z > project(view, Vec3(0.0f, 0.0f, -20.0f)).z);
    }

    TEST_CASE("the field of view is vertical and the aspect ratio widens the image") {
        CameraComponent camera;
        camera.verticalFovDegrees = 90.0f;
        const CameraView view = makeCameraView(Mat4(1.0f), camera, 2.0f);

        // With a 90 degree field of view, a point as high as it is far is on the top edge.
        CHECK(project(view, Vec3(0.0f, 10.0f, -10.0f)).y == doctest::Approx(1.0f));
        // The image is twice as wide as it is high.
        CHECK(project(view, Vec3(20.0f, 0.0f, -10.0f)).x == doctest::Approx(1.0f));
    }

    TEST_CASE("the view follows the placement of the camera") {
        const Quat lookDownX = glm::angleAxis(toRadians(-90.0f), kWorldUp);
        const CameraView view = makeCameraView(placedAt(Vec3(5.0f, 2.0f, 0.0f), lookDownX), CameraComponent(), 1.0f);

        CHECK_NEAR(view.position, Vec3(5.0f, 2.0f, 0.0f), 1e-6f);
        CHECK_NEAR(view.forward, Vec3(1.0f, 0.0f, 0.0f), 1e-6f);
        CHECK_NEAR(Vec2(project(view, Vec3(25.0f, 2.0f, 0.0f))), Vec2(0.0f), 1e-5f);
        // Behind the camera.
        CHECK((view.viewProjection * Vec4(-5.0f, 2.0f, 0.0f, 1.0f)).w < 0.0f);
    }

    TEST_CASE("scale in the camera's placement is ignored") {
        const Quat rotation = glm::angleAxis(toRadians(35.0f), glm::normalize(Vec3(0.2f, 1.0f, 0.1f)));
        const CameraView plain = makeCameraView(placedAt(Vec3(1.0f, 2.0f, 3.0f), rotation), CameraComponent(), 1.5f);
        const CameraView scaled =
            makeCameraView(placedAt(Vec3(1.0f, 2.0f, 3.0f), rotation, Vec3(3.0f, 0.5f, 7.0f)), CameraComponent(), 1.5f);

        CHECK(test::nearlyEqual(plain.viewProjection, scaled.viewProjection, 1e-4f));
        CHECK_NEAR(plain.forward, scaled.forward, 1e-6f);
    }

    TEST_CASE("the view of a camera's entity is shaken by its shake, in the camera's own space and in metres") {
        World world;
        const Entity camera = world.createEntity("Camera");
        const Quat turned = glm::angleAxis(toRadians(30.0f), kWorldUp);
        // Scaled: what the shake moves is metres, not the entity's units.
        world.transform(camera) = Transform{Vec3(1.0f, 2.0f, 3.0f), turned, Vec3(3.0f)};
        world.add<CameraComponent>(camera).verticalFovDegrees = 50.0f;
        world.add<CameraShakeComponent>(camera) = {.trauma = 1.0f, .phase = 1.7f};
        world.updateWorldTransforms();
        const CameraComponent lens = *world.tryGet<CameraComponent>(camera);

        const Transform offset = cameraShakeOffset(*world.tryGet<CameraShakeComponent>(camera));
        REQUIRE(glm::length(offset.position) > 1e-3f);
        const CameraView view = makeCameraView(world, camera, 1.5f);
        CHECK_NEAR(view.position, Vec3(1.0f, 2.0f, 3.0f) + turned * offset.position, 1e-5f);
        CHECK_NEAR(view.forward, turned * offset.rotation * kWorldForward, 1e-5f);
        const CameraView expected = makeCameraView(
            placedAt(Vec3(1.0f, 2.0f, 3.0f) + turned * offset.position, turned * offset.rotation), lens, 1.5f);
        CHECK(test::nearlyEqual(view.viewProjection, expected.viewProjection, 1e-4f));
        CHECK(test::nearlyEqual(cameraPlacement(world, camera), placedAt(view.position, turned * offset.rotation),
                                1e-5f));
    }

    TEST_CASE("a camera that does not shake is seen from where its entity is, to the bit") {
        World world;
        const Entity camera = world.createEntity("Camera");
        world.transform(camera) =
            Transform{Vec3(-4.0f, 1.5f, 2.25f), glm::angleAxis(toRadians(-20.0f), kWorldRight), Vec3(2.0f)};
        world.add<CameraComponent>(camera);
        world.updateWorldTransforms();
        const CameraView placed = makeCameraView(world.worldMatrix(camera), CameraComponent{}, 1.25f);
        const auto sameView = [&](const CameraView& view) {
            return view.view == placed.view && view.viewProjection == placed.viewProjection &&
                   view.position == placed.position && view.forward == placed.forward;
        };

        CHECK(cameraPlacement(world, camera) == world.worldMatrix(camera));
        CHECK(sameView(makeCameraView(world, camera, 1.25f)));
        // Nor one whose shake is at rest, wherever in its shaking it is.
        world.add<CameraShakeComponent>(camera).phase = 3.5f;
        CHECK(cameraPlacement(world, camera) == world.worldMatrix(camera));
        CHECK(sameView(makeCameraView(world, camera, 1.25f)));
        // Shaken, it is somewhere else.
        world.tryGet<CameraShakeComponent>(camera)->trauma = 0.5f;
        CHECK_FALSE(sameView(makeCameraView(world, camera, 1.25f)));
    }

    TEST_CASE("the game is seen through the camera a sequence that plays has cut to, or else the primary one") {
        World world;
        const Entity primary = world.createEntity("Primary");
        world.add<CameraComponent>(primary);
        const Entity cinema = world.createEntity("Cinema");
        world.add<CameraComponent>(cinema).primary = false;
        const Entity director = world.createEntity("Director");
        world.add<SequencePlayerComponent>(director);
        CHECK(findGameCamera(world) == primary);

        // Cut to while it plays.
        world.tryGet<SequencePlayerComponent>(director)->camera = world.id(cinema);
        CHECK(findGameCamera(world) == cinema);
        // Not while it does not, nor when it is switched off - or the camera is.
        world.tryGet<SequencePlayerComponent>(director)->playing = false;
        CHECK(findGameCamera(world) == primary);
        world.tryGet<SequencePlayerComponent>(director)->playing = true;
        world.setEnabled(director, false);
        CHECK(findGameCamera(world) == primary);
        world.setEnabled(director, true);
        world.setEnabled(cinema, false);
        CHECK(findGameCamera(world) == primary);
        world.setEnabled(cinema, true);
        // A cut to what is no camera, or to nothing that is there, is no cut.
        world.tryGet<SequencePlayerComponent>(director)->camera = world.id(director);
        CHECK(findGameCamera(world) == primary);
        world.tryGet<SequencePlayerComponent>(director)->camera = *Uuid::parse("01234567-89ab-4def-8edc-ba9876543210");
        CHECK(findGameCamera(world) == primary);

        // Of two that cut, the first in the order of the hierarchy.
        world.tryGet<SequencePlayerComponent>(director)->camera = world.id(cinema);
        const Entity close = world.createEntity("Close");
        world.add<CameraComponent>(close).primary = false;
        const Entity second = world.createEntity("Second director");
        world.add<SequencePlayerComponent>(second).camera = world.id(close);
        CHECK(findGameCamera(world) == cinema);
        world.setSiblingIndex(second, 0);
        CHECK(findGameCamera(world) == close);
        // Without a primary camera, a cut is still seen; without either, nothing is.
        world.destroyEntity(primary);
        CHECK(findGameCamera(world) == close);
        world.destroyEntity(second);
        world.destroyEntity(director);
        CHECK(findGameCamera(world).isNull());
    }

    TEST_CASE("an entity without a camera has no view of its own") {
        World world;
        const Entity plain = world.createEntity("No camera");
        world.updateWorldTransforms();
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS((void)makeCameraView(world, plain, 1.0f), test::AssertionFailure);
#else
        // Seen through a camera as a new one is.
        const CameraView view = makeCameraView(world, plain, 1.0f);
        CHECK(view.nearPlane == CameraComponent{}.nearPlane);
        CHECK_NEAR(view.forward, kWorldForward, 1e-6f);
#endif
    }

    TEST_CASE("an orthographic camera sees a box of the configured height") {
        CameraComponent camera;
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = 8.0f;
        camera.nearPlane = 1.0f;
        camera.farPlane = 101.0f;
        const CameraView view = makeCameraView(Mat4(1.0f), camera, 2.0f);

        // Half the height up is the top edge, at any distance.
        CHECK(project(view, Vec3(0.0f, 4.0f, -5.0f)).y == doctest::Approx(1.0f));
        CHECK(project(view, Vec3(0.0f, 4.0f, -90.0f)).y == doctest::Approx(1.0f));
        CHECK(project(view, Vec3(8.0f, 0.0f, -5.0f)).x == doctest::Approx(1.0f));
        CHECK(project(view, Vec3(0.0f, 0.0f, -1.0f)).z == doctest::Approx(1.0f));
        CHECK(project(view, Vec3(0.0f, 0.0f, -101.0f)).z == doctest::Approx(0.0f));
    }

    TEST_CASE("settings that would break the projection are clamped") {
        CameraComponent camera;
        camera.nearPlane = 10.0f;
        camera.farPlane = 1.0f;
        camera.verticalFovDegrees = 0.0f;

        for (const float aspect :
             {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.0f}) {
            CAPTURE(aspect);
            const CameraView view = makeCameraView(Mat4(1.0f), camera, aspect);
            CHECK(view.farPlane > view.nearPlane);
            CHECK(isFinite(view.viewProjection));
            CHECK(isFinite(view.inverseViewProjection));
        }

        camera = CameraComponent();
        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = 0.0f;
        camera.nearPlane = 0.0f;
        const CameraView view = makeCameraView(Mat4(1.0f), camera, 1.0f);
        CHECK(view.nearPlane > 0.0f);
        CHECK(isFinite(view.inverseViewProjection));
    }

    TEST_CASE("the ray through the center of the image is the viewing direction") {
        const Quat rotation = glm::angleAxis(toRadians(40.0f), kWorldUp);
        const CameraView view = makeCameraView(placedAt(Vec3(3.0f, 1.0f, -2.0f), rotation), CameraComponent(), 2.0f);

        const Ray ray = screenRay(view, Vec2(400.0f, 200.0f), Vec2(800.0f, 400.0f));
        CHECK_NEAR(ray.direction, view.forward, 1e-5f);
        // It starts on the near plane, straight ahead of the camera.
        CHECK_NEAR(ray.origin, view.position + view.forward * view.nearPlane, 1e-4f);
    }

    TEST_CASE("a ray passes through the pixel it was made for") {
        const Vec2 targetSize(640.0f, 360.0f);
        for (const CameraProjection projection : {CameraProjection::Perspective, CameraProjection::Orthographic}) {
            CAPTURE(static_cast<int>(projection));
            CameraComponent camera;
            camera.projection = projection;
            const CameraView view = makeCameraView(placedAt(Vec3(0.0f, 2.0f, 6.0f)), camera, 640.0f / 360.0f);

            for (const Vec2 pixel :
                 {Vec2(0.0f, 0.0f), Vec2(639.0f, 359.0f), Vec2(100.5f, 300.25f), Vec2(320.0f, 180.0f)}) {
                const Ray ray = screenRay(view, pixel, targetSize);
                CHECK(glm::length(ray.direction) == doctest::Approx(1.0f));
                const std::optional<Vec2> back = worldToScreen(view, ray.at(12.0f), targetSize);
                REQUIRE(back.has_value());
                CHECK_NEAR(*back, pixel, 0.01f);
            }
        }
    }

    TEST_CASE("pixels have their origin at the top-left corner") {
        const CameraView view = makeCameraView(Mat4(1.0f), CameraComponent(), 1.0f);
        const Vec2 targetSize(200.0f, 200.0f);

        CHECK(screenRay(view, Vec2(100.0f, 0.0f), targetSize).direction.y > 0.0f);
        CHECK(screenRay(view, Vec2(100.0f, 200.0f), targetSize).direction.y < 0.0f);
        CHECK(screenRay(view, Vec2(0.0f, 100.0f), targetSize).direction.x < 0.0f);

        const std::optional<Vec2> above = worldToScreen(view, Vec3(0.0f, 1.0f, -5.0f), targetSize);
        REQUIRE(above.has_value());
        CHECK(above->y < 100.0f);
        CHECK(above->x == doctest::Approx(100.0f));
    }

    TEST_CASE("points behind the camera have no place on screen") {
        const CameraView view = makeCameraView(Mat4(1.0f), CameraComponent(), 1.0f);
        CHECK_FALSE(worldToScreen(view, Vec3(0.0f, 0.0f, 5.0f), Vec2(100.0f)).has_value());
        CHECK_FALSE(worldToScreen(view, Vec3(0.0f, 0.0f, 0.0f), Vec2(100.0f)).has_value());
        CHECK(worldToScreen(view, Vec3(0.0f, 0.0f, -5.0f), Vec2(100.0f)).has_value());
    }

    TEST_CASE("the primary camera is the first usable one in hierarchy order") {
        World world;
        CHECK(findPrimaryCamera(world).isNull());

        const Entity plain = world.createEntity("No camera");
        const Entity secondary = world.createEntity("Secondary");
        world.add<CameraComponent>(secondary).primary = false;
        CHECK(findPrimaryCamera(world).isNull());

        const Entity disabled = world.createEntity("Disabled");
        world.add<CameraComponent>(disabled);
        world.setEnabled(disabled, false);

        const Entity rig = world.createEntity("Disabled rig");
        const Entity onRig = world.createEntity("On disabled rig");
        world.add<CameraComponent>(onRig);
        REQUIRE(world.setParent(onRig, rig).has_value());
        world.setEnabled(rig, false);
        CHECK(findPrimaryCamera(world).isNull());

        const Entity first = world.createEntity("First");
        const Entity second = world.createEntity("Second");
        world.add<CameraComponent>(first);
        world.add<CameraComponent>(second);
        CHECK(findPrimaryCamera(world) == first);

        // Order in the hierarchy decides, not order of creation.
        world.setSiblingIndex(second, 0);
        CHECK(findPrimaryCamera(world) == second);

        // Enabling the rig makes its camera eligible again, and it comes earlier.
        world.setEnabled(rig, true);
        world.setSiblingIndex(rig, 0);
        CHECK(findPrimaryCamera(world) == onRig);
        CHECK(world.isValid(plain));
    }
}
