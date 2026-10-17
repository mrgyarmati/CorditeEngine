#include "cordite/renderer/LevelOfDetail.h"

#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <cmath>
#include <limits>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// The errors of three levels: a millimetre, a centimetre, ten centimetres.
constexpr std::array<float, 3> kErrors = {0.001f, 0.01f, 0.1f};

/// A view with perspective from the origin, in which a metre one metre away is 500 pixels.
LodView perspectiveView(float errorPixels = 1.0f) {
    return {.cameraPosition = Vec3(0.0f),
            .orthographic = false,
            .pixelsPerMetre = 500.0f,
            .nearPlane = 0.1f,
            .errorPixels = errorPixels};
}

/// A box of a millimetre `distance` metres in front of the origin.
Aabb pointAt(float distance) {
    return Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -distance), Vec3(0.0005f));
}

} // namespace

TEST_SUITE("LevelOfDetail") {
    TEST_CASE("a view says how many pixels high a metre is: one metre in front of a perspective camera, and anywhere "
              "without") {
        CameraComponent camera;
        camera.verticalFovDegrees = 90.0f;
        const CameraView perspective = makeCameraView(Mat4(1.0f), camera, 1.0f);
        const LodView lodView = lodViewOf(perspective, 1000, 1.5f);
        // Ninety degrees from top to bottom: one metre away, a metre is half the height.
        CHECK(lodView.pixelsPerMetre == doctest::Approx(500.0f));
        CHECK_FALSE(lodView.orthographic);
        CHECK(lodView.errorPixels == 1.5f);
        CHECK(lodView.nearPlane == camera.nearPlane);

        camera.projection = CameraProjection::Orthographic;
        camera.orthographicHeight = 10.0f;
        const LodView flat = lodViewOf(makeCameraView(Mat4(1.0f), camera, 1.0f), 1000, 1.0f);
        CHECK(flat.orthographic);
        CHECK(flat.pixelsPerMetre == doctest::Approx(100.0f));
    }

    TEST_CASE("the coarsest level whose error looks small enough is drawn") {
        // One metre away a millimetre is half a pixel, a centimetre five: the first level.
        CHECK(selectLod(kErrors, pointAt(1.0f), 1.0f, perspectiveView()) == 1);
        // Ten metres away a centimetre is half a pixel.
        CHECK(selectLod(kErrors, pointAt(10.0f), 1.0f, perspectiveView()) == 2);
        CHECK(selectLod(kErrors, pointAt(100.0f), 1.0f, perspectiveView()) == 3);
        // Forty centimetres away even a millimetre looks larger than a pixel.
        CHECK(selectLod(kErrors, pointAt(0.4f), 1.0f, perspectiveView()) == 0);
        // Twice as large, the errors are: the mesh needs finer levels at the same distance.
        CHECK(selectLod(kErrors, pointAt(10.0f), 2.5f, perspectiveView()) == 1);
        // A view that allows more error takes coarser levels.
        CHECK(selectLod(kErrors, pointAt(1.0f), 1.0f, perspectiveView(6.0f)) == 2);
        // No levels: the mesh itself.
        CHECK(selectLod({}, pointAt(100.0f), 1.0f, perspectiveView()) == 0);
    }

    TEST_CASE("the nearest point of the bounds counts, and a camera inside them sees the finest") {
        // A sphere of five metres about a point twenty metres away is fifteen metres away
        // at its nearest: a centimetre there is a third of a pixel.
        const Aabb large = Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -20.0f), Vec3(5.0f / std::sqrt(3.0f)));
        CHECK(selectLod(kErrors, large, 1.0f, perspectiveView()) == 2);
        // Eleven metres larger, the sphere is four metres away: a centimetre is more than a
        // pixel.
        const Aabb larger = Aabb::fromCenterExtents(Vec3(0.0f, 0.0f, -20.0f), Vec3(16.0f / std::sqrt(3.0f)));
        CHECK(selectLod(kErrors, larger, 1.0f, perspectiveView()) == 1);
        // The camera inside.
        const Aabb around = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(1.0f));
        CHECK(selectLod(kErrors, around, 1.0f, perspectiveView()) == 0);
    }

    TEST_CASE("without perspective, how far a mesh is does not matter") {
        LodView flat{.orthographic = true, .pixelsPerMetre = 50.0f, .nearPlane = 0.1f, .errorPixels = 1.0f};
        // A centimetre is half a pixel anywhere.
        CHECK(selectLod(kErrors, pointAt(1.0f), 1.0f, flat) == 2);
        CHECK(selectLod(kErrors, pointAt(1000.0f), 1.0f, flat) == 2);
        // Behind the camera too: a view without perspective sees on both sides of its
        // plane, as far as its depth goes.
        CHECK(selectLod(kErrors, pointAt(-5.0f), 1.0f, flat) == 2);
    }

    TEST_CASE("a view that allows no error, and numbers that are none, draw the finest") {
        for (const float pixels : {0.0f, -1.0f, kNotANumber, std::numeric_limits<float>::infinity()}) {
            CAPTURE(pixels);
            CHECK(selectLod(kErrors, pointAt(100.0f), 1.0f, perspectiveView(pixels)) == 0);
        }
        for (const float scale : {0.0f, -1.0f, kNotANumber}) {
            CAPTURE(scale);
            CHECK(selectLod(kErrors, pointAt(100.0f), scale, perspectiveView()) == 0);
        }
        CHECK(selectLod(kErrors, Aabb(), 1.0f, perspectiveView()) == 0);
        CHECK(selectLod(kErrors, Aabb::fromCenterExtents(Vec3(kNotANumber), Vec3(1.0f)), 1.0f, perspectiveView()) == 0);
        LodView broken = perspectiveView();
        broken.cameraPosition = Vec3(kNotANumber);
        CHECK(selectLod(kErrors, pointAt(100.0f), 1.0f, broken) == 0);
        // A level whose error is no number ends the levels.
        const std::array<float, 3> errors = {0.001f, kNotANumber, 0.1f};
        CHECK(selectLod(errors, pointAt(100.0f), 1.0f, perspectiveView()) == 1);
    }

    TEST_CASE("how much a matrix enlarges is its largest scale along an axis") {
        CHECK(scaleOf(Mat4(1.0f)) == 1.0f);
        const Mat4 scaled = Transform{.rotation = glm::angleAxis(0.7f, glm::normalize(Vec3(1.0f, 2.0f, 3.0f))),
                                      .scale = Vec3(2.0f, -3.0f, 0.5f)}
                                .toMatrix();
        CHECK(scaleOf(scaled) == doctest::Approx(3.0f));
    }
}
