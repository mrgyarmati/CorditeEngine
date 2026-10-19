#include "cordite/renderer/ShadowPlan.h"

#include "cordite/math/Projection.h"
#include "cordite/math/Random.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

using namespace cordite;

namespace {

/// A camera at `eye` looking at `target`, with a 16:9 picture.
CameraView perspectiveView(const Vec3& eye, const Vec3& target, float nearPlane = 0.1f, float farPlane = 200.0f) {
    CameraComponent camera;
    camera.verticalFovDegrees = 60.0f;
    camera.nearPlane = nearPlane;
    camera.farPlane = farPlane;
    return makeCameraView(glm::inverse(lookAt(eye, target)), camera, 16.0f / 9.0f);
}

SceneLight sun(const Vec3& direction, bool castsShadows = true) {
    SceneLight light{.type = GpuLightType::Directional, .direction = glm::normalize(direction)};
    light.shadow.enabled = castsShadows;
    return light;
}

SceneLight pointLight(const Vec3& position, float range, bool castsShadows = true) {
    SceneLight light{.type = GpuLightType::Point, .position = position, .range = range};
    light.shadow.enabled = castsShadows;
    return light;
}

SceneLight spotLight(const Vec3& position, const Vec3& direction, float range, float outerAngle) {
    SceneLight light{.type = GpuLightType::Spot,
                     .position = position,
                     .direction = glm::normalize(direction),
                     .range = range,
                     .innerConeAngle = outerAngle * 0.5f,
                     .outerConeAngle = outerAngle};
    light.shadow.enabled = true;
    return light;
}

/// Clusters and a shadow plan for some lights, as the renderer makes them.
struct Planned {
    Planned(const CameraView& view, const std::vector<SceneLight>& lights,
            const ShadowSettings& settings = shadowSettingsFor(ShadowQuality::Medium)) {
        buildLightClusters(view, lights, clusters);
        planShadows(view, lights, clusters, settings, plan);
    }

    LightClusters clusters;
    ShadowPlan plan;
};

/// Where a point of the world is in a shadow map: x and y from -1 to 1, depth from 0 to 1.
Vec3 inShadowMap(const ShadowView& shadow, const Vec3& point) {
    const Vec4 clip = shadow.viewProjection * Vec4(point, 1.0f);
    return Vec3(clip) / clip.w;
}

bool isInside(const ShadowView& shadow, const Vec3& point) {
    const Vec3 place = inShadowMap(shadow, point);
    return std::abs(place.x) <= 1.0f && std::abs(place.y) <= 1.0f && place.z >= 0.0f && place.z <= 1.0f;
}

/// Checks that the axes of a map are what its matrix makes of them: a step of one
/// texel along `right` or `up` from `point` moves it by one texel in the map, and not
/// in depth.
void checkAxes(const ShadowView& shadow, const Vec3& point) {
    CHECK(glm::length(shadow.right) == doctest::Approx(1.0f));
    CHECK(glm::length(shadow.up) == doctest::Approx(1.0f));
    CHECK(glm::dot(shadow.right, shadow.up) == doctest::Approx(0.0f));

    // A texel there: the size at one metre times the distance for a map with
    // perspective, measured along the way the map looks, which is at right angles to both.
    const Vec4 clip = shadow.viewProjection * Vec4(point, 1.0f);
    const float texel = shadow.perspective ? shadow.texelSize * clip.w : shadow.texelSize;
    const float oneTexel = 2.0f / static_cast<float>(shadow.tileSize);
    const Vec3 here = inShadowMap(shadow, point);
    const Vec3 toTheRight = inShadowMap(shadow, point + shadow.right * texel) - here;
    const Vec3 above = inShadowMap(shadow, point + shadow.up * texel) - here;
    CHECK(toTheRight.x == doctest::Approx(oneTexel).epsilon(0.01));
    CHECK(std::abs(toTheRight.y) < oneTexel * 0.01f);
    CHECK(std::abs(toTheRight.z) < 1.0e-5f);
    CHECK(above.y == doctest::Approx(oneTexel).epsilon(0.01));
    CHECK(std::abs(above.x) < oneTexel * 0.01f);
    CHECK(std::abs(above.z) < 1.0e-5f);
}

/// True when two tiles of the atlas share a texel.
bool overlap(const ShadowView& a, const ShadowView& b) {
    return a.tileOrigin.x < b.tileOrigin.x + b.tileSize && b.tileOrigin.x < a.tileOrigin.x + a.tileSize &&
           a.tileOrigin.y < b.tileOrigin.y + b.tileSize && b.tileOrigin.y < a.tileOrigin.y + a.tileSize;
}

bool isPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

/// Checks that every tile lies in the atlas, is a power of two, and shares no texel
/// with another.
void checkTiles(const ShadowPlan& plan, uint32_t atlasSize) {
    for (size_t first = 0; first < plan.views.size(); ++first) {
        const ShadowView& shadow = plan.views[first];
        CAPTURE(first);
        CHECK(isPowerOfTwo(shadow.tileSize));
        CHECK(shadow.tileSize >= kMinShadowTileSize);
        CHECK(shadow.tileOrigin.x + shadow.tileSize <= atlasSize);
        CHECK(shadow.tileOrigin.y + shadow.tileSize <= atlasSize);
        // A tile starts at a multiple of its size, as squares cut in four do.
        CHECK(shadow.tileOrigin.x % shadow.tileSize == 0);
        CHECK(shadow.tileOrigin.y % shadow.tileSize == 0);
        for (size_t second = first + 1; second < plan.views.size(); ++second) {
            CAPTURE(second);
            CHECK_FALSE(overlap(shadow, plan.views[second]));
        }
    }
}

} // namespace

TEST_SUITE("ShadowPlan") {
    TEST_CASE("shadow qualities have names, and come to these numbers") {
        CHECK(nameOf(ShadowQuality::Off) == "off");
        CHECK(nameOf(ShadowQuality::Low) == "low");
        CHECK(nameOf(ShadowQuality::Medium) == "medium");
        CHECK(nameOf(ShadowQuality::High) == "high");
        CHECK(displayNameOf(ShadowQuality::Off) == "Off");
        CHECK(displayNameOf(ShadowQuality::Low) == "Low");
        CHECK(displayNameOf(ShadowQuality::Medium) == "Medium");
        CHECK(displayNameOf(ShadowQuality::High) == "High");

        CHECK(shadowSettingsFor(ShadowQuality::Off).atlasSize == 0);
        CHECK(shadowSettingsFor(ShadowQuality::Low) ==
              ShadowSettings{.atlasSize = 2048, .cascadeSize = 512, .localSize = 256, .filterRadius = 1});
        CHECK(shadowSettingsFor(ShadowQuality::Medium) ==
              ShadowSettings{.atlasSize = 4096, .cascadeSize = 1024, .localSize = 512, .filterRadius = 1});
        CHECK(shadowSettingsFor(ShadowQuality::High) ==
              ShadowSettings{.atlasSize = 8192, .cascadeSize = 2048, .localSize = 1024, .filterRadius = 2});
        // What the defaults of the struct are, a medium quality is.
        CHECK(shadowSettingsFor(ShadowQuality::Medium) == ShadowSettings());

        // Every quality is found by its name, whatever the case; nothing else is.
        CHECK(kShadowQualities.size() == 4);
        for (const ShadowQuality quality : kShadowQualities) {
            CHECK(shadowQualityFromName(nameOf(quality)) == quality);
        }
        CHECK(shadowQualityFromName("HIGH") == ShadowQuality::High);
        CHECK(shadowQualityFromName("Off") == ShadowQuality::Off);
        CHECK_FALSE(shadowQualityFromName("ultra").has_value());
        CHECK_FALSE(shadowQualityFromName("").has_value());
    }

    TEST_CASE("cascades divide the shadow distance evenly, by a constant factor, or in between") {
        // Evenly: each as deep as the others.
        const auto even = cascadeEnds(1.0f, 81.0f, 4, 0.0f);
        CHECK(even[0] == doctest::Approx(21.0f));
        CHECK(even[1] == doctest::Approx(41.0f));
        CHECK(even[2] == doctest::Approx(61.0f));
        CHECK(even[3] == 81.0f);

        // By a constant factor: each three times as far as the one before.
        const auto byFactor = cascadeEnds(1.0f, 81.0f, 4, 1.0f);
        CHECK(byFactor[0] == doctest::Approx(3.0f));
        CHECK(byFactor[1] == doctest::Approx(9.0f));
        CHECK(byFactor[2] == doctest::Approx(27.0f));
        CHECK(byFactor[3] == 81.0f);

        // Half way between the two.
        const auto mixed = cascadeEnds(1.0f, 81.0f, 4, 0.5f);
        CHECK(mixed[0] == doctest::Approx(12.0f));
        CHECK(mixed[1] == doctest::Approx(25.0f));
        CHECK(mixed[2] == doctest::Approx(44.0f));
        CHECK(mixed[3] == 81.0f);

        // Fewer cascades leave the rest at zero; the last one always ends at the distance.
        const auto two = cascadeEnds(0.5f, 50.0f, 2, 0.0f);
        CHECK(two[0] == doctest::Approx(25.25f));
        CHECK(two[1] == 50.0f);
        CHECK(two[2] == 0.0f);
        CHECK(two[3] == 0.0f);
        CHECK(cascadeEnds(0.1f, 30.0f, 1, 0.7f)[0] == 30.0f);
    }

    TEST_CASE("cascade settings outside what makes sense are brought back into it") {
        // No cascades, or too many.
        CHECK(cascadeEnds(1.0f, 10.0f, 0, 0.0f)[0] == 10.0f);
        const auto many = cascadeEnds(1.0f, 10.0f, 9, 0.0f);
        CHECK(many[3] == 10.0f);
        CHECK(many[0] == doctest::Approx(3.25f));

        // A distribution beyond its ends counts as the end.
        CHECK(cascadeEnds(1.0f, 81.0f, 4, 7.0f)[0] == doctest::Approx(3.0f));
        CHECK(cascadeEnds(1.0f, 81.0f, 4, -2.0f)[0] == doctest::Approx(21.0f));

        // A distance nearer than the near plane, and a near plane of nothing.
        const auto inverted = cascadeEnds(5.0f, 2.0f, 2, 0.5f);
        CHECK(inverted[0] == doctest::Approx(5.0f));
        CHECK(inverted[1] == 5.0f);
        const auto fromZero = cascadeEnds(0.0f, 100.0f, 2, 1.0f);
        CHECK(std::isfinite(fromZero[0]));
        CHECK(fromZero[0] > 0.0f);
        CHECK(fromZero[1] == 100.0f);
    }

    TEST_CASE("a point is looked up in the map along the largest part of its offset from the light") {
        CHECK(pointShadowFace(Vec3(3.0f, 1.0f, -2.0f)) == 0);
        CHECK(pointShadowFace(Vec3(-3.0f, 1.0f, -2.0f)) == 1);
        CHECK(pointShadowFace(Vec3(0.5f, 2.0f, 1.0f)) == 2);
        CHECK(pointShadowFace(Vec3(0.5f, -2.0f, 1.0f)) == 3);
        CHECK(pointShadowFace(Vec3(0.5f, 0.2f, 1.0f)) == 4);
        CHECK(pointShadowFace(Vec3(0.5f, 0.2f, -1.0f)) == 5);
        // Where two parts are as large, X goes before Y and Y before Z; the shaders agree.
        CHECK(pointShadowFace(Vec3(1.0f, 1.0f, 1.0f)) == 0);
        CHECK(pointShadowFace(Vec3(0.0f, -1.0f, 1.0f)) == 3);
        CHECK(pointShadowFace(Vec3(0.0f)) == 0);
    }

    TEST_CASE("lights that cast no shadows get no shadow maps") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const Planned planned(view,
                              {sun(Vec3(0.0f, -1.0f, 0.0f), false), pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f, false)});
        CHECK(planned.plan.views.empty());
        CHECK(planned.plan.cascades.count == 0);
        CHECK(planned.plan.lightsWithoutRoom == 0);
        REQUIRE(planned.clusters.lights.size() == 2);
        CHECK(planned.clusters.lights[0].shadowView == -1);
        CHECK(planned.clusters.lights[1].shadowView == -1);
    }

    TEST_CASE("with shadows switched off, no light gets a map and none counts as going without") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const Planned planned(view, {sun(Vec3(0.0f, -1.0f, 0.0f)), pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f)},
                              shadowSettingsFor(ShadowQuality::Off));
        CHECK(planned.plan.views.empty());
        CHECK(planned.plan.cascades.count == 0);
        CHECK(planned.plan.lightsWithoutRoom == 0);
        CHECK(planned.clusters.lights[0].shadowView == -1);
        CHECK(planned.clusters.lights[1].shadowView == -1);
    }

    TEST_CASE("a spot light gets one map that looks along its cone") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const Vec3 position(1.0f, 3.0f, -1.0f);
        const Vec3 direction = glm::normalize(Vec3(0.2f, -1.0f, 0.1f));
        SceneLight light = spotLight(position, direction, 6.0f, toRadians(30.0f));
        light.shadow.bias = 1.25f;
        light.shadow.normalOffset = 3.5f;
        const Planned planned(view, {light});

        REQUIRE(planned.plan.views.size() == 1);
        const ShadowView& shadow = planned.plan.views[0];
        CHECK(shadow.perspective);
        CHECK(shadow.light == 0);
        CHECK(shadow.tileSize == 512);
        CHECK(shadow.tileOrigin == UVec2(0, 0));
        // The light knows its map and how to look things up in it.
        CHECK(planned.clusters.lights[0].shadowView == 0);
        CHECK(planned.clusters.lights[0].shadowBias == 1.25f);
        CHECK(planned.clusters.lights[0].shadowNormalOffset == 3.5f);

        // Along the axis: the middle of the map, deeper the farther.
        const Vec3 nearby = inShadowMap(shadow, position + direction * 0.5f);
        const Vec3 far = inShadowMap(shadow, position + direction * 5.9f);
        CHECK(nearby.x == doctest::Approx(0.0f).epsilon(1.0e-4));
        CHECK(nearby.y == doctest::Approx(0.0f).epsilon(1.0e-4));
        CHECK(nearby.z > 0.0f);
        CHECK(far.z > nearby.z);
        CHECK(far.z < 1.0f);
        // The end of its range is the far end of the map.
        CHECK(inShadowMap(shadow, position + direction * 6.0f).z == doctest::Approx(1.0f).epsilon(1.0e-4));

        // The edge of the cone is the edge of the map: 30 degrees off the axis.
        const Vec3 side = glm::normalize(glm::cross(direction, Vec3(0.0f, 0.0f, 1.0f)));
        const Vec3 onEdge =
            position + (direction * std::cos(toRadians(30.0f)) + side * std::sin(toRadians(30.0f))) * 3.0f;
        const Vec3 edge = inShadowMap(shadow, onEdge);
        CHECK(glm::length(Vec2(edge)) == doctest::Approx(1.0f).epsilon(1.0e-3));

        // A texel one metre from the light: the width of the cone there over the texels.
        CHECK(shadow.texelSize == doctest::Approx(2.0f * std::tan(toRadians(30.0f)) / 512.0f));
        checkAxes(shadow, position + direction * 3.0f + side * 0.4f);
        CHECK_FALSE(isInside(shadow, position - direction));
    }

    TEST_CASE("a point or a rect light gets six maps, and each point around it is in the one it is looked up in") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const Vec3 position(0.5f, 1.5f, -1.0f);
        const float range = 5.0f;
        // A rect light's shadows are those of a point light in its middle.
        for (const GpuLightType type : {GpuLightType::Point, GpuLightType::Rect}) {
            CAPTURE(static_cast<int>(type));
            SceneLight light = pointLight(position, range);
            light.type = type;
            const Planned planned(view, {light});

            REQUIRE(planned.plan.views.size() == 6);
            CHECK(planned.clusters.lights[0].shadowView == 0);
            checkTiles(planned.plan, 4096);
            for (size_t face = 0; face < 6; ++face) {
                const ShadowView& shadow = planned.plan.views[face];
                CAPTURE(face);
                CHECK(shadow.perspective);
                CHECK(shadow.tileSize == 512);
                CHECK(shadow.light == 0);
            }

            Random random(3);
            for (int sample = 0; sample < 3000; ++sample) {
                const Vec3 offset = random.onUnitSphere() * random.range(0.1f, range * 0.999f);
                CAPTURE(offset);
                const ShadowView& shadow = planned.plan.views[pointShadowFace(offset)];
                const Vec3 place = inShadowMap(shadow, position + offset);
                // Within the map, and not at its very edge: the filter has texels to read
                // around it. That margin is what a map sees beyond its side of the cube.
                CHECK(std::max(std::abs(place.x), std::abs(place.y)) < 1.0f - 2.0f * 2.4f / 512.0f);
                CHECK(place.z > 0.0f);
                CHECK(place.z < 1.0f);
            }

            // The six look along the axes, in the order of pointShadowFace().
            const std::array<Vec3, 6> directions = {Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f),
                                                    Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                                    Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f)};
            for (size_t face = 0; face < 6; ++face) {
                CAPTURE(face);
                const Vec3 place = inShadowMap(planned.plan.views[face], position + directions[face] * 2.0f);
                CHECK(place.x == doctest::Approx(0.0f).epsilon(1.0e-4));
                CHECK(place.y == doctest::Approx(0.0f).epsilon(1.0e-4));
                checkAxes(planned.plan.views[face], position + directions[face] * 2.0f);
                CHECK_FALSE(isInside(planned.plan.views[face], position - directions[face] * 2.0f));
            }
        }
    }

    TEST_CASE("the sun gets cascades that cover the view slice by slice") {
        const CameraView view = perspectiveView(Vec3(3.0f, 2.0f, 9.0f), Vec3(0.0f, 1.0f, 0.0f));
        SceneLight light = sun(Vec3(-0.4f, -0.8f, -0.3f));
        light.shadow.distance = 50.0f;
        light.shadow.cascades = 4;
        light.shadow.cascadeDistribution = 0.6f;
        const Planned planned(view, {light});

        const ShadowCascades& cascades = planned.plan.cascades;
        REQUIRE(planned.plan.views.size() == 4);
        CHECK(cascades.count == 4);
        CHECK(cascades.firstView == 0);
        CHECK(planned.clusters.lights[0].shadowView == 0);
        const auto expectedEnds = cascadeEnds(0.1f, 50.0f, 4, 0.6f);
        CHECK(cascades.ends == expectedEnds);
        CHECK(cascades.fadeEnd == 50.0f);
        CHECK(cascades.fadeStart == doctest::Approx(45.0f));
        checkTiles(planned.plan, 4096);

        // Every point the camera sees up to the shadow distance is in the map of the
        // cascade its distance puts it in.
        Random random(5);
        const Mat4 cameraToWorld = glm::inverse(view.view);
        for (int sample = 0; sample < 4000; ++sample) {
            const float distance = random.range(0.1f, 50.0f);
            // Within the 60 degree lens of a 16:9 picture.
            const float halfHeight = distance * std::tan(toRadians(30.0f));
            const Vec3 inView(random.range(-1.0f, 1.0f) * halfHeight * 16.0f / 9.0f,
                              random.range(-1.0f, 1.0f) * halfHeight, -distance);
            const Vec3 point(cameraToWorld * Vec4(inView, 1.0f));
            uint32_t cascade = 0;
            while (cascade + 1 < cascades.count && distance > cascades.ends[cascade]) {
                ++cascade;
            }
            CAPTURE(distance);
            CAPTURE(cascade);
            CHECK(isInside(planned.plan.views[cascade], point));
        }

        for (uint32_t cascade = 0; cascade < 4; ++cascade) {
            const ShadowView& shadow = planned.plan.views[cascade];
            CAPTURE(cascade);
            CHECK_FALSE(shadow.perspective);
            CHECK(shadow.tileSize == 1024);
            CHECK(shadow.light == 0);
            // A texel is some centimetres of the world, and more with every cascade.
            CHECK(shadow.texelSize > 0.0f);
            if (cascade > 0) {
                CHECK(shadow.texelSize > planned.plan.views[cascade - 1].texelSize);
            }
            // The light looks along its direction: farther along it is deeper in the map.
            const Vec3 base(0.0f, 1.0f, 0.0f);
            CHECK(inShadowMap(shadow, base + light.direction).z > inShadowMap(shadow, base).z);
            // What stands between the light and the view, far off, is still in the map.
            CHECK(inShadowMap(shadow, base - light.direction * 150.0f).z > 0.0f);
            checkAxes(shadow, base);
        }
    }

    TEST_CASE("the cascades reach no further than the camera sees") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f), 0.5f, 20.0f);
        SceneLight light = sun(Vec3(0.0f, -1.0f, 0.0f));
        light.shadow.distance = 500.0f;
        light.shadow.cascades = 2;
        const Planned planned(view, {light});
        CHECK(planned.plan.cascades.count == 2);
        CHECK(planned.plan.cascades.ends[1] == 20.0f);
        CHECK(planned.plan.cascades.fadeEnd == 20.0f);
        CHECK(planned.plan.views.size() == 2);
    }

    TEST_CASE("the maps of the sun keep their texels as the camera moves and turns") {
        SceneLight light = sun(Vec3(-0.3f, -0.9f, 0.25f));
        light.shadow.distance = 40.0f;
        light.shadow.cascades = 3;
        const Vec3 eye(2.0f, 3.0f, 7.0f);
        const Planned reference(perspectiveView(eye, Vec3(0.0f)), {light});
        REQUIRE(reference.plan.views.size() == 3);

        // Where a point of the world falls in a map, in texels.
        const auto texelOf = [](const ShadowView& shadow, const Vec3& point) {
            return Vec2(inShadowMap(shadow, point)) * 0.5f * static_cast<float>(shadow.tileSize);
        };
        const Vec3 landmark(0.37f, 0.11f, -0.52f);

        Random random(9);
        for (int step = 0; step < 50; ++step) {
            // Somewhere else, looking somewhere else.
            const Vec3 moved =
                eye + Vec3(random.range(-3.0f, 3.0f), random.range(-1.0f, 1.0f), random.range(-3.0f, 3.0f));
            const Vec3 target(random.range(-2.0f, 2.0f), random.range(0.0f, 1.0f), random.range(-2.0f, 2.0f));
            const Planned planned(perspectiveView(moved, target), {light});
            REQUIRE(planned.plan.views.size() == 3);
            for (size_t cascade = 0; cascade < 3; ++cascade) {
                CAPTURE(cascade);
                const ShadowView& before = reference.plan.views[cascade];
                const ShadowView& after = planned.plan.views[cascade];
                // A texel is as large as before: the cascade is a sphere, which is as
                // wide whichever way the camera looks.
                CHECK(after.texelSize == before.texelSize);
                // And the map has moved by whole texels: a point of the world is at the
                // same place within its texel.
                const Vec2 shift = texelOf(after, landmark) - texelOf(before, landmark);
                CHECK(std::abs(shift.x - std::round(shift.x)) < 0.02f);
                CHECK(std::abs(shift.y - std::round(shift.y)) < 0.02f);
            }
        }
    }

    TEST_CASE("a light's shadow resolution doubles or halves its map") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        SceneLight low = spotLight(Vec3(-2.0f, 3.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 6.0f, 0.6f);
        low.shadow.resolutionScale = 0.5f;
        SceneLight high = spotLight(Vec3(2.0f, 3.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 6.0f, 0.6f);
        high.shadow.resolutionScale = 2.0f;
        SceneLight daylight = sun(Vec3(0.0f, -1.0f, 0.1f));
        daylight.shadow.resolutionScale = 2.0f;
        daylight.shadow.cascades = 2;

        const Planned planned(view, {low, high, daylight});
        REQUIRE(planned.plan.views.size() == 4);
        // The sun first, whatever the order of the lights.
        CHECK(planned.plan.views[0].tileSize == 2048);
        CHECK(planned.plan.views[1].tileSize == 2048);
        REQUIRE(planned.clusters.lights.size() == 3);
        CHECK(planned.clusters.lights[0].shadowView == 0);
        for (size_t index = 1; index < 3; ++index) {
            const GpuLight& light = planned.clusters.lights[index];
            REQUIRE(light.shadowView >= 2);
            const ShadowView& shadow = planned.plan.views[static_cast<size_t>(light.shadowView)];
            CHECK(shadow.light == index);
            CHECK(shadow.tileSize == (light.position.x < 0.0f ? 256u : 1024u));
        }
        checkTiles(planned.plan, 4096);

        // The same lights at low quality: everything half the size.
        const Planned cheaper(view, {low, high, daylight}, shadowSettingsFor(ShadowQuality::Low));
        REQUIRE(cheaper.plan.views.size() == 4);
        CHECK(cheaper.plan.views[0].tileSize == 1024);
        checkTiles(cheaper.plan, 2048);
    }

    TEST_CASE("the sun leaves a quarter of the atlas to the other lights: its far cascades give way first") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        // Four cascades at twice the usual size would be the whole atlas of 4096 texels.
        SceneLight daylight = sun(Vec3(0.2f, -1.0f, 0.1f));
        daylight.shadow.resolutionScale = 2.0f;
        daylight.shadow.cascades = 4;
        const SceneLight lamp = pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f);

        const Planned planned(view, {lamp, daylight});
        REQUIRE(planned.plan.views.size() == 10);
        const std::vector<uint32_t> expected = {2048, 2048, 1024, 1024};
        float previousTexel = 0.0f;
        for (size_t cascade = 0; cascade < 4; ++cascade) {
            CAPTURE(cascade);
            const ShadowView& shadow = planned.plan.views[cascade];
            CHECK(shadow.tileSize == expected[cascade]);
            // Each map says what its own texels cover; the far ones cover more.
            checkAxes(shadow, Vec3(0.0f, 0.0f, 0.0f));
            CHECK(shadow.texelSize > previousTexel);
            previousTexel = shadow.texelSize;
        }
        // The lamp finds the room it asks for.
        CHECK(planned.plan.lightsWithoutRoom == 0);
        for (size_t face = 4; face < 10; ++face) {
            CHECK(planned.plan.views[face].tileSize == 512);
        }
        checkTiles(planned.plan, 4096);

        // Three such cascades are three quarters exactly, and stay as they are.
        daylight.shadow.cascades = 3;
        const Planned three(view, {lamp, daylight});
        REQUIRE(three.plan.views.size() == 9);
        for (size_t cascade = 0; cascade < 3; ++cascade) {
            CHECK(three.plan.views[cascade].tileSize == 2048);
        }
        CHECK(three.plan.lightsWithoutRoom == 0);
        checkTiles(three.plan, 4096);

        // In an atlas too small for anything else, the sun still gets the smallest maps
        // there are, and the lamp none.
        daylight.shadow.cascades = 4;
        const Planned cramped(view, {lamp, daylight},
                              {.atlasSize = 128, .cascadeSize = 1024, .localSize = 512, .filterRadius = 1});
        REQUIRE(cramped.plan.views.size() == 4);
        for (const ShadowView& shadow : cramped.plan.views) {
            CHECK(shadow.tileSize == 64);
        }
        CHECK(cramped.plan.lightsWithoutRoom == 1);
        checkTiles(cramped.plan, 128);
    }

    TEST_CASE("of several directional lights, the brightest that casts shadows is the sun") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        // The brightest of all casts no shadows; of the other three, the third is brightest.
        std::vector<SceneLight> lights = {sun(Vec3(0.0f, -1.0f, 0.0f), false), sun(Vec3(1.0f, -1.0f, 0.0f)),
                                          sun(Vec3(-1.0f, -1.0f, 0.0f)), sun(Vec3(0.0f, -1.0f, 1.0f))};
        lights[0].color = Vec3(5.0f);
        lights[1].color = Vec3(0.5f, 0.6f, 0.4f);
        lights[2].color = Vec3(0.2f, 0.1f, 2.0f);
        lights[3].color = Vec3(1.0f);
        const Planned planned(view, lights);
        REQUIRE(planned.clusters.lights.size() == 4);
        CHECK(planned.clusters.lights[0].shadowView == -1);
        CHECK(planned.clusters.lights[1].shadowView == -1);
        CHECK(planned.clusters.lights[2].shadowView == 0);
        CHECK(planned.clusters.lights[3].shadowView == -1);
        CHECK(planned.plan.cascades.count == 4);
        CHECK(planned.plan.views.size() == 4);
        // The two that are to cast shadows and are not the sun.
        CHECK(planned.plan.lightsWithoutRoom == 2);
        for (const ShadowView& shadow : planned.plan.views) {
            CHECK(shadow.light == 2);
        }

        // As bright as each other: the one that comes first.
        lights[2].color = Vec3(1.0f);
        const Planned tied(view, lights);
        CHECK(tied.clusters.lights[2].shadowView == 0);
        CHECK(tied.clusters.lights[3].shadowView == -1);
    }

    TEST_CASE("when the brightest directional light finds no room, no other is the sun in its place") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        // An atlas of one tile: four cascades do not fit, one would.
        const ShadowSettings settings{.atlasSize = 64, .cascadeSize = 64, .localSize = 64, .filterRadius = 1};
        std::vector<SceneLight> lights = {sun(Vec3(0.0f, -1.0f, 0.0f)), sun(Vec3(1.0f, -1.0f, 0.0f))};
        lights[0].color = Vec3(2.0f);
        lights[0].shadow.cascades = 4;
        lights[1].color = Vec3(1.0f);
        lights[1].shadow.cascades = 1;
        const Planned planned(view, lights, settings);
        CHECK(planned.plan.views.empty());
        CHECK(planned.plan.cascades.count == 0);
        CHECK(planned.plan.lightsWithoutRoom == 2);
        CHECK(planned.clusters.lights[0].shadowView == -1);
        CHECK(planned.clusters.lights[1].shadowView == -1);

        // With one cascade the brightest fits, and is the sun.
        lights[0].shadow.cascades = 1;
        const Planned fitting(view, lights, settings);
        CHECK(fitting.plan.views.size() == 1);
        CHECK(fitting.clusters.lights[0].shadowView == 0);
        CHECK(fitting.plan.lightsWithoutRoom == 1);
    }

    TEST_CASE("of an atlas whose side is not a power of two, the largest one in it is used") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 10.0f), Vec3(0.0f, 2.0f, 0.0f));
        // 3000 texels a side: 2048 of them are used. Four cascades of 1024 would be all
        // of that, so the far two are halved, and the six maps of the lamp fill the rest
        // to the last texel.
        const ShadowSettings settings{.atlasSize = 3000, .cascadeSize = 1024, .localSize = 1024, .filterRadius = 1};
        const std::vector<SceneLight> lights = {sun(Vec3(0.2f, -1.0f, 0.1f)), pointLight(Vec3(0.0f, 2.0f, 6.0f), 2.0f)};
        const Planned planned(view, lights, settings);
        REQUIRE(planned.plan.views.size() == 10);
        CHECK(planned.plan.lightsWithoutRoom == 0);
        checkTiles(planned.plan, 2048);
        uint64_t texels = 0;
        for (size_t index = 0; index < planned.plan.views.size(); ++index) {
            CAPTURE(index);
            const uint32_t size = planned.plan.views[index].tileSize;
            CHECK(size == (index < 2 ? 1024u : 512u));
            texels += static_cast<uint64_t>(size) * size;
        }
        CHECK(texels == 2048u * 2048u);

        // Smaller than the smallest map: no shadows, as with none at all.
        const Planned tiny(view, lights, {.atlasSize = 63, .cascadeSize = 64, .localSize = 64, .filterRadius = 1});
        CHECK(tiny.plan.views.empty());
        CHECK(tiny.plan.lightsWithoutRoom == 0);
    }

    TEST_CASE("what a light says about its shadows is made usable before it is used") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const float notANumber = std::numeric_limits<float>::quiet_NaN();
        const float endless = std::numeric_limits<float>::infinity();

        for (const float broken : {notANumber, endless, -endless}) {
            CAPTURE(broken);
            std::vector<SceneLight> lights = {sun(Vec3(0.3f, -1.0f, 0.2f)), pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f),
                                              spotLight(Vec3(1.0f, 3.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 6.0f, 0.5f)};
            for (SceneLight& light : lights) {
                light.shadow.bias = broken;
                light.shadow.normalOffset = broken;
                light.shadow.resolutionScale = broken;
                light.shadow.distance = broken;
                light.shadow.cascadeDistribution = broken;
            }
            const Planned planned(view, lights);
            // Every light still gets its maps, and nothing in them is "not a number".
            REQUIRE(planned.plan.views.size() == 4 + 6 + 1);
            checkTiles(planned.plan, 4096);
            for (const ShadowView& shadow : planned.plan.views) {
                for (glm::length_t column = 0; column < 4; ++column) {
                    for (glm::length_t row = 0; row < 4; ++row) {
                        CHECK(std::isfinite(shadow.viewProjection[column][row]));
                    }
                }
                CHECK(std::isfinite(shadow.texelSize));
                CHECK(shadow.texelSize > 0.0f);
            }
            for (const float end : planned.plan.cascades.ends) {
                CHECK(std::isfinite(end));
            }
            CHECK(std::isfinite(planned.plan.cascades.fadeStart));
            // No number at all: what a light has when it says nothing. Without end: as
            // far as the camera sees. Less than nothing: the least there can be.
            const float expected = std::isnan(broken) ? 60.0f : (broken > 0.0f ? view.farPlane : 2.0f * view.nearPlane);
            CHECK(planned.plan.cascades.fadeEnd == doctest::Approx(expected));
            for (const GpuLight& light : planned.clusters.lights) {
                CHECK(std::isfinite(light.shadowBias));
                CHECK(std::isfinite(light.shadowNormalOffset));
                CHECK(light.shadowBias >= 0.0f);
                CHECK(light.shadowBias <= 16.0f);
                CHECK(light.shadowNormalOffset >= 0.0f);
                CHECK(light.shadowNormalOffset <= 16.0f);
            }
        }

        // Offsets beyond what a light may ask for are brought back to it.
        std::vector<SceneLight> lights = {pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f),
                                          pointLight(Vec3(1.0f, 1.0f, 0.0f), 4.0f)};
        lights[0].shadow.bias = -3.0f;
        lights[0].shadow.normalOffset = 100.0f;
        lights[1].shadow.bias = 2.5f;
        lights[1].shadow.normalOffset = 0.0f;
        const Planned planned(view, lights);
        REQUIRE(planned.clusters.lights.size() == 2);
        CHECK(planned.clusters.lights[0].shadowBias == 0.0f);
        CHECK(planned.clusters.lights[0].shadowNormalOffset == 16.0f);
        CHECK(planned.clusters.lights[1].shadowBias == 2.5f);
        CHECK(planned.clusters.lights[1].shadowNormalOffset == 0.0f);
    }

    TEST_CASE("when the atlas fills up, lights get smaller maps, and the farthest lights none") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 10.0f), Vec3(0.0f, 2.0f, 0.0f));
        // A small atlas: sixteen tiles of the size a light asks for.
        const ShadowSettings settings{.atlasSize = 1024, .cascadeSize = 256, .localSize = 256, .filterRadius = 1};

        // Point lights in a row away from the camera, each asking for six maps.
        std::vector<SceneLight> lights;
        for (int index = 0; index < 60; ++index) {
            lights.push_back(pointLight(Vec3(0.0f, 2.0f, 6.0f - 0.25f * static_cast<float>(index)), 1.0f));
        }
        // Given in the opposite order: it is the distance that decides, not the order.
        std::reverse(lights.begin(), lights.end());
        const Planned planned(view, lights, settings);
        checkTiles(planned.plan, 1024);
        CHECK(planned.plan.views.size() <= kMaxShadowViews);
        CHECK(planned.plan.views.size() % 6 == 0);

        // Two lights get the size they ask for (twelve of the sixteen tiles); the rest
        // of the atlas goes to lights with smaller maps, until nothing is left.
        size_t fullSize = 0;
        size_t smaller = 0;
        size_t without = 0;
        float farthestWithShadow = 0.0f;
        float nearestWithout = 1.0e9f;
        for (const GpuLight& light : planned.clusters.lights) {
            const float distance = glm::distance(light.position, view.position);
            if (light.shadowView < 0) {
                ++without;
                nearestWithout = std::min(nearestWithout, distance);
                continue;
            }
            farthestWithShadow = std::max(farthestWithShadow, distance);
            const uint32_t tileSize = planned.plan.views[static_cast<size_t>(light.shadowView)].tileSize;
            fullSize += tileSize == 256 ? 1 : 0;
            smaller += tileSize < 256 ? 1 : 0;
        }
        CHECK(fullSize == 2);
        CHECK(smaller > 0);
        CHECK(without > 0);
        CHECK(planned.plan.lightsWithoutRoom == without);
        CHECK(fullSize + smaller == planned.plan.views.size() / 6);
        // Every light with a shadow is nearer than every light without.
        CHECK(farthestWithShadow < nearestWithout);
    }

    TEST_CASE("a view has no more shadow maps than kMaxShadowViews") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 10.0f), Vec3(0.0f, 2.0f, 0.0f));
        // Room in the atlas for far more maps than a view may have.
        const ShadowSettings settings{.atlasSize = 8192, .cascadeSize = 256, .localSize = 64, .filterRadius = 1};
        std::vector<SceneLight> lights = {sun(Vec3(0.2f, -1.0f, 0.1f))};
        for (int index = 0; index < 100; ++index) {
            lights.push_back(spotLight(Vec3(-5.0f + 0.1f * static_cast<float>(index), 3.0f, 0.0f),
                                       Vec3(0.0f, -1.0f, 0.0f), 4.0f, 0.5f));
        }
        const Planned planned(view, lights, settings);
        CHECK(planned.plan.views.size() == kMaxShadowViews);
        CHECK(planned.plan.cascades.count == 4);
        CHECK(planned.plan.lightsWithoutRoom == 100 - (kMaxShadowViews - 4));
        checkTiles(planned.plan, 8192);
    }

    TEST_CASE("planning again replaces the plan, and tells the lights anew") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const std::vector<SceneLight> lights = {sun(Vec3(0.0f, -1.0f, 0.0f)), pointLight(Vec3(0.0f, 1.0f, 0.0f), 4.0f)};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        ShadowPlan plan;
        planShadows(view, lights, clusters, shadowSettingsFor(ShadowQuality::Medium), plan);
        CHECK(plan.views.size() == 10);
        CHECK(clusters.lights[1].shadowView == 4);

        planShadows(view, lights, clusters, shadowSettingsFor(ShadowQuality::Off), plan);
        CHECK(plan.views.empty());
        CHECK(plan.cascades.count == 0);
        CHECK(clusters.lights[0].shadowView == -1);
        CHECK(clusters.lights[1].shadowView == -1);

        // Clusters that were not built from these lights plan nothing rather than
        // reading past the end of them.
        const std::vector<SceneLight> fewer = {sun(Vec3(0.0f, -1.0f, 0.0f))};
        planShadows(view, std::span<const SceneLight>(), clusters, shadowSettingsFor(ShadowQuality::Medium), plan);
        CHECK(plan.views.empty());
        planShadows(view, fewer, clusters, shadowSettingsFor(ShadowQuality::Medium), plan);
        CHECK(plan.views.size() == 4);
        CHECK(clusters.lights[1].shadowView == -1);
    }

    TEST_CASE("clusters that do not say where their lights come from are a mistake of the caller") {
        const CameraView view = perspectiveView(Vec3(0.0f, 2.0f, 8.0f), Vec3(0.0f));
        const std::vector<SceneLight> lights = {sun(Vec3(0.0f, -1.0f, 0.0f))};
        LightClusters clusters;
        buildLightClusters(view, lights, clusters);
        // As code that fills in clusters by hand might leave them.
        clusters.sources.clear();
        ShadowPlan plan;
        plan.views.resize(3);
        plan.lightsWithoutRoom = 7;
#if defined(CORDITE_ENABLE_ASSERTS)
        CHECK_THROWS_AS(planShadows(view, lights, clusters, shadowSettingsFor(ShadowQuality::Medium), plan),
                        test::AssertionFailure);
#else
        planShadows(view, lights, clusters, shadowSettingsFor(ShadowQuality::Medium), plan);
#endif
        // Either way the plan is empty and no light refers to a map.
        CHECK(plan.views.empty());
        CHECK(plan.lightsWithoutRoom == 0);
        CHECK(clusters.lights[0].shadowView == -1);
    }
}
