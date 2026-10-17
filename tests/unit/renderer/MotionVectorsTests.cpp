#include "cordite/renderer/MotionVectors.h"

#include "cordite/math/Transform.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// What a camera with perspective, `fovDegrees` from top to bottom, sees from `position`
/// turned by `rotation`, on a square target.
CameraView viewFrom(const Vec3& position, const Quat& rotation = kIdentityQuat, float fovDegrees = 60.0f) {
    CameraComponent camera;
    camera.verticalFovDegrees = fovDegrees;
    return makeCameraView(Transform{.position = position, .rotation = rotation}.toMatrix(), camera, 1.0f);
}

/// A turn by `degrees` about `axis`.
Quat turnedBy(float degrees, const Vec3& axis) {
    return glm::angleAxis(glm::radians(degrees), axis);
}

} // namespace

TEST_SUITE("MotionVectors") {
    TEST_CASE("a camera that moves and turns a little goes on with the same shot") {
        const CameraView before = viewFrom(Vec3(1.0f, 2.0f, 3.0f));
        CHECK_FALSE(isCameraCut(before, before));
        CHECK_FALSE(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f + 9.9f))));
        CHECK_FALSE(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), turnedBy(44.0f, kWorldUp))));
        // From sixty degrees to seventy-five: what was in the picture is three quarters as
        // large, which is less than half again smaller.
        CHECK_FALSE(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), kIdentityQuat, 75.0f)));
    }

    TEST_CASE("a camera that jumps, turns or zooms too far between two pictures cuts to another shot") {
        const CameraView before = viewFrom(Vec3(1.0f, 2.0f, 3.0f));
        // Further than ten metres, in any direction.
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f + 10.1f))));
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f - 7.2f, 2.0f + 7.2f, 3.0f))));
        // More than 45 degrees about any axis: rolling leaves the way it looks as it was.
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), turnedBy(46.0f, kWorldUp))));
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), turnedBy(-46.0f, kWorldRight))));
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), turnedBy(46.0f, kWorldForward))));
        // Zoomed in or out by more than half again: the projection scales the picture.
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), kIdentityQuat, 20.0f)));
        CHECK(isCameraCut(before, viewFrom(Vec3(1.0f, 2.0f, 3.0f), kIdentityQuat, 100.0f)));
        // From perspective to none.
        CameraComponent flat;
        flat.projection = CameraProjection::Orthographic;
        CHECK(
            isCameraCut(before, makeCameraView(Transform{.position = Vec3(1.0f, 2.0f, 3.0f)}.toMatrix(), flat, 1.0f)));
        // And a camera that is no numbers is no shot to go on with.
        CameraView broken = before;
        broken.position = Vec3(kNotANumber);
        CHECK(isCameraCut(before, broken));
        CHECK(isCameraCut(broken, before));
        broken = before;
        broken.view[0][0] = kNotANumber;
        CHECK(isCameraCut(before, broken));
    }

    TEST_CASE("a view's history goes on only through the same camera, without a cut") {
        MotionHistory history;
        const CameraView view = viewFrom(Vec3(0.0f));
        const Uuid camera = Uuid::generate();
        // The first picture has nothing before it.
        CHECK_FALSE(history.continuesWith(view, camera));

        history.remember(view, camera);
        CHECK(history.valid);
        CHECK(history.continuesWith(view, camera));
        CHECK(history.continuesWith(viewFrom(Vec3(0.0f, 0.0f, -0.5f)), camera));
        // Through another entity, or none, it is another shot.
        CHECK_FALSE(history.continuesWith(view, Uuid::generate()));
        CHECK_FALSE(history.continuesWith(view, Uuid()));
        CHECK_FALSE(history.continuesWith(viewFrom(Vec3(0.0f, 0.0f, -11.0f)), camera));

        // A view of no entity goes on with itself.
        history.remember(view, Uuid());
        CHECK(history.continuesWith(view, Uuid()));

        history.placements.emplace_back(camera, Mat4(2.0f));
        history.poses.emplace_back(camera, std::vector<Mat4>{Mat4(1.0f)});
        history.forget();
        CHECK_FALSE(history.valid);
        CHECK(history.placements.empty());
        CHECK(history.poses.empty());
        CHECK_FALSE(history.continuesWith(view, Uuid()));
    }

    TEST_CASE("what a view remembers is found by the ids of the entities, in whatever order it was put") {
        MotionHistory history;
        std::vector<Uuid> ids;
        for (int index = 0; index < 64; ++index) {
            ids.push_back(Uuid::generate());
            history.placements.emplace_back(ids.back(), Mat4(static_cast<float>(index)));
        }
        history.poses.emplace_back(ids[7], std::vector<Mat4>(3, Mat4(7.0f)));
        history.poses.emplace_back(ids[2], std::vector<Mat4>(2, Mat4(2.0f)));
        history.remember(viewFrom(Vec3(0.0f)), Uuid());
        for (int index = 0; index < 64; ++index) {
            const Mat4* placement = history.placementOf(ids[static_cast<size_t>(index)]);
            REQUIRE(placement != nullptr);
            CHECK(*placement == Mat4(static_cast<float>(index)));
        }
        REQUIRE(history.poseOf(ids[7]) != nullptr);
        CHECK(*history.poseOf(ids[7]) == std::vector<Mat4>(3, Mat4(7.0f)));
        REQUIRE(history.poseOf(ids[2]) != nullptr);
        CHECK(history.poseOf(ids[2])->size() == 2);
        // What is not remembered is none.
        CHECK(history.placementOf(Uuid::generate()) == nullptr);
        CHECK(history.poseOf(ids[3]) == nullptr);
        CHECK(MotionHistory().placementOf(ids[0]) == nullptr);
    }

    TEST_CASE("the motion debug view colors a motion by its direction and its length") {
        const UVec2 size(200, 100);
        // Nothing moved: black, as for what is no number.
        CHECK(motionDebugColor(Vec2(0.0f), size) == Vec3(0.0f));
        CHECK(motionDebugColor(Vec2(kNotANumber, 0.0f), size) == Vec3(0.0f));
        // Sixteen pixels to the right, and more, are red at its brightest; eight half as
        // bright.
        CHECK_NEAR(motionDebugColor(Vec2(16.0f / 200.0f, 0.0f), size), Vec3(1.0f, 0.0f, 0.0f), 1.0e-5f);
        CHECK_NEAR(motionDebugColor(Vec2(0.5f, 0.0f), size), Vec3(1.0f, 0.0f, 0.0f), 1.0e-5f);
        CHECK_NEAR(motionDebugColor(Vec2(8.0f / 200.0f, 0.0f), size), Vec3(0.5f, 0.0f, 0.0f), 1.0e-5f);
        // Up - y of the picture goes down - is a quarter of the way round the hues.
        CHECK_NEAR(motionDebugColor(Vec2(0.0f, -16.0f / 100.0f), size), Vec3(0.5f, 1.0f, 0.0f), 1.0e-5f);
        // To the left, cyan; down, blue-purple.
        CHECK_NEAR(motionDebugColor(Vec2(-16.0f / 200.0f, 0.0f), size), Vec3(0.0f, 1.0f, 1.0f), 1.0e-5f);
        CHECK_NEAR(motionDebugColor(Vec2(0.0f, 16.0f / 100.0f), size), Vec3(0.5f, 0.0f, 1.0f), 1.0e-5f);
        // Pixels are counted on each axis by the size of the picture along it: a tenth of
        // the width and a fifth of the height are twenty pixels each, up and to the right -
        // an eighth of the way round the hues.
        CHECK_NEAR(motionDebugColor(Vec2(0.1f, -0.2f), size), Vec3(1.0f, 0.75f, 0.0f), 1.0e-5f);
    }
}
