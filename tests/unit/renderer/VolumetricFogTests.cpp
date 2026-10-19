#include "cordite/renderer/VolumetricFog.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace cordite;

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A fog lit by lights, as dense everywhere, that throws on all it takes away and
/// alike in every direction; its light is worked out 50 metres deep.
FogSettings evenFog(float density) {
    FogSettings fog;
    fog.color = Vec3(0.1f);
    fog.density = density;
    fog.height = 1000.0f;
    fog.heightFalloff = 0.0f;
    fog.volumetric = true;
    fog.scattering = Vec3(1.0f);
    fog.anisotropy = 0.0f;
    fog.volumetricDistance = 50.0f;
    return fog;
}

GpuLight sunOf(const Vec3& direction, const Vec3& fogColor) {
    GpuLight sun;
    sun.typeAndChannels = static_cast<uint32_t>(GpuLightType::Directional);
    sun.direction = direction;
    sun.color = fogColor;
    sun.fogColor = fogColor;
    return sun;
}

GpuLight pointOf(const Vec3& position, float range, const Vec3& fogColor) {
    GpuLight light;
    light.typeAndChannels = static_cast<uint32_t>(GpuLightType::Point);
    light.position = position;
    light.range = range;
    light.color = fogColor;
    light.fogColor = fogColor;
    return light;
}

/// A spot light at the origin shining along -Z, with its cones in radians, as LightClusters
/// lays one out.
GpuLight spotOf(float innerAngle, float outerAngle, const Vec3& fogColor) {
    GpuLight light = pointOf(Vec3(0.0f), 10.0f, fogColor);
    light.typeAndChannels = static_cast<uint32_t>(GpuLightType::Spot);
    light.direction = Vec3(0.0f, 0.0f, -1.0f);
    light.spotScale = 1.0f / (std::cos(innerAngle) - std::cos(outerAngle));
    light.spotOffset = -std::cos(outerAngle) * light.spotScale;
    return light;
}

/// A rect light at the origin shining along -Z, its right +X, with the halves of its sides
/// `halfWidth` and `halfHeight`, as LightClusters lays one out.
GpuLight rectOf(float halfWidth, float halfHeight, const Vec3& fogColor) {
    GpuLight light = pointOf(Vec3(0.0f), 10.0f, fogColor);
    light.typeAndChannels = static_cast<uint32_t>(GpuLightType::Rect);
    light.direction = Vec3(0.0f, 0.0f, -1.0f);
    light.rectRight = Vec3(halfWidth, 0.0f, 0.0f);
    light.rectHalfHeight = halfHeight;
    return light;
}

/// A line of sight from the origin along -Z, the way the camera looks.
FogSight sightOf(float distance, uint32_t slices = 64) {
    return {.viewer = Vec3(0.0f),
            .direction = Vec3(0.0f, 0.0f, -1.0f),
            .forward = Vec3(0.0f, 0.0f, -1.0f),
            .distance = distance,
            .slices = slices};
}

/// How much of what is `distance` along -Z from the origin is fog.
float amountAt(const FogSettings& fog, float distance) {
    return std::isinf(distance) ? fogAmountTowards(fog, Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f))
                                : fogAmount(fog, Vec3(0.0f), Vec3(0.0f, 0.0f, -distance));
}

/// The integral of what `light` throws towards the origin, times how much the fog's amount
/// grows, along -Z up to `distance`: in a million steps, in doubles.
Vec3 integratedAlong(const FogSettings& fog, const GpuLight& light, float distance) {
    constexpr int kSteps = 1000000;
    const double step = static_cast<double>(distance) / kSteps;
    glm::dvec3 sum(0.0);
    double before = 0.0;
    for (int index = 0; index < kSteps; ++index) {
        const double end = step * (index + 1);
        const double middle = end - step * 0.5;
        const double amount = amountAt(fog, static_cast<float>(end));
        const Vec3 thrown =
            lightInFog(light, Vec3(0.0f, 0.0f, static_cast<float>(-middle)), Vec3(0.0f, 0.0f, 1.0f), fog.anisotropy);
        sum += glm::dvec3(thrown) * (amount - before);
        before = amount;
    }
    return Vec3(sum) * fog.scattering;
}

float relativeError(const Vec3& actual, const Vec3& expected) {
    return glm::length(actual - expected) / glm::length(expected);
}

} // namespace

TEST_SUITE("VolumetricFog") {
    TEST_CASE("the grid of a view has boxes about the size of a tile, and no more of them than its quality allows") {
        CHECK(fogVolumeGridFor(UVec2(1920, 1080), ScreenEffectsQuality::Medium) == FogVolumeGrid{160, 90, 64});
        CHECK(fogVolumeGridFor(UVec2(1280, 720), ScreenEffectsQuality::Medium) == FogVolumeGrid{107, 60, 64});
        CHECK(fogVolumeGridFor(UVec2(1280, 720), ScreenEffectsQuality::Low) == FogVolumeGrid{80, 45, 48});
        CHECK(fogVolumeGridFor(UVec2(1920, 1080), ScreenEffectsQuality::High) == FogVolumeGrid{240, 135, 80});
        // A picture four times as large has boxes twice as large, not four times as many.
        CHECK(fogVolumeGridFor(UVec2(3840, 2160), ScreenEffectsQuality::Medium) == FogVolumeGrid{160, 90, 64});
        // Tall and narrow: the longer side decides how large the boxes are.
        CHECK(fogVolumeGridFor(UVec2(100, 4000), ScreenEffectsQuality::Medium) == FogVolumeGrid{4, 160, 64});
        CHECK(fogVolumeGridFor(UVec2(1, 1), ScreenEffectsQuality::Low) == FogVolumeGrid{1, 1, 48});

        CHECK(fogVolumeGridFor(UVec2(1920, 1080), ScreenEffectsQuality::Off).isEmpty());
        CHECK(fogVolumeGridFor(UVec2(0, 1080), ScreenEffectsQuality::High).isEmpty());
        CHECK(fogVolumeGridFor(UVec2(1920, 0), ScreenEffectsQuality::High).isEmpty());
        CHECK_FALSE(fogVolumeGridFor(UVec2(64, 64), ScreenEffectsQuality::Low).isEmpty());
    }

    TEST_CASE("slices are thin near the camera and deep far from it, and a depth is found at its share") {
        CHECK(fogSliceDepth(0.5f, 40.0f) == 10.0f);
        CHECK(fogSliceDepth(1.0f, 40.0f) == 40.0f);
        CHECK(fogSliceDepth(0.0f, 40.0f) == 0.0f);
        CHECK(fogSliceDepth(2.0f, 40.0f) == 40.0f);
        CHECK(fogSliceDepth(-1.0f, 40.0f) == 0.0f);
        CHECK(fogSliceDepth(kNotANumber, 40.0f) == 0.0f);

        CHECK(fogSliceShare(10.0f, 40.0f) == 0.5f);
        CHECK(fogSliceShare(40.0f, 40.0f) == 1.0f);
        CHECK(fogSliceShare(100.0f, 40.0f) == 1.0f);
        CHECK(fogSliceShare(kInfinity, 40.0f) == 1.0f);
        CHECK(fogSliceShare(0.0f, 40.0f) == 0.0f);
        CHECK(fogSliceShare(-3.0f, 40.0f) == 0.0f);
        CHECK(fogSliceShare(kNotANumber, 40.0f) == 0.0f);
        // A grid that reaches nowhere ends at once.
        CHECK(fogSliceShare(5.0f, 0.0f) == 1.0f);

        for (const float share : {0.1f, 0.25f, 0.6f, 0.9f}) {
            CHECK(fogSliceShare(fogSliceDepth(share, 50.0f), 50.0f) == doctest::Approx(share).epsilon(1.0e-6));
        }
        float thickness = 0.0f;
        for (uint32_t slice = 0; slice < 64; ++slice) {
            const float next = fogSliceDepth(static_cast<float>(slice + 1) / 64.0f, 50.0f) -
                               fogSliceDepth(static_cast<float>(slice) / 64.0f, 50.0f);
            CHECK(next > thickness);
            thickness = next;
        }
    }

    TEST_CASE("the phase function sends all the light somewhere, and more of it onwards the larger the anisotropy") {
        for (const float cosine : {-1.0f, -0.3f, 0.0f, 0.5f, 1.0f}) {
            CHECK(henyeyGreenstein(cosine, 0.0f) == doctest::Approx(1.0 / (4.0 * kPi)).epsilon(1.0e-6));
        }
        // Its integral over every direction: 2 pi times that over the cosine, from -1 to 1.
        for (const float anisotropy : {-0.9f, -0.5f, 0.0f, 0.2f, 0.6f, 0.9f}) {
            constexpr int kSteps = 400000;
            double sum = 0.0;
            for (int index = 0; index < kSteps; ++index) {
                const double cosine = -1.0 + (index + 0.5) * 2.0 / kSteps;
                sum += henyeyGreenstein(static_cast<float>(cosine), anisotropy) * (2.0 / kSteps);
            }
            CAPTURE(anisotropy);
            CHECK(2.0 * kPi * sum == doctest::Approx(1.0).epsilon(1.0e-3));
        }

        CHECK(henyeyGreenstein(1.0f, 0.5f) > henyeyGreenstein(0.0f, 0.5f));
        CHECK(henyeyGreenstein(0.0f, 0.5f) > henyeyGreenstein(-1.0f, 0.5f));
        CHECK(henyeyGreenstein(-1.0f, -0.5f) > henyeyGreenstein(1.0f, -0.5f));
        // (1 - g^2) / (4 pi (1 + g^2 - 2g)^1.5) straight onwards: 0.75 / (4 pi 0.125).
        CHECK(henyeyGreenstein(1.0f, 0.5f) == doctest::Approx(6.0 / (4.0 * kPi)).epsilon(1.0e-6));

        // What is not a phase function is held to the nearest that is.
        CHECK(henyeyGreenstein(0.3f, 5.0f) == henyeyGreenstein(0.3f, kMostFogAnisotropy));
        CHECK(henyeyGreenstein(0.3f, -5.0f) == henyeyGreenstein(0.3f, -kMostFogAnisotropy));
        CHECK(henyeyGreenstein(0.3f, kNotANumber) == henyeyGreenstein(0.3f, 0.0f));
        CHECK(henyeyGreenstein(2.0f, 0.5f) == henyeyGreenstein(1.0f, 0.5f));
        CHECK(henyeyGreenstein(kNotANumber, 0.5f) == henyeyGreenstein(0.0f, 0.5f));
        CHECK(std::isfinite(henyeyGreenstein(1.0f, kMostFogAnisotropy)));
    }

    TEST_CASE("a light throws its fog color into the fog by its distance and its cone, and more of it onwards") {
        // Two metres from a point light of a range of ten: (1 - (4 / 100)^2)^2 / 4.
        const GpuLight point = pointOf(Vec3(0.0f), 10.0f, Vec3(2.0f, 1.0f, 0.5f));
        const float falloff = (1.0f - 0.0016f) * (1.0f - 0.0016f) / 4.0f;
        CHECK_NEAR(lightInFog(point, Vec3(0.0f, 0.0f, -2.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f),
                   Vec3(2.0f, 1.0f, 0.5f) * falloff / (4.0f * kPi), 1.0e-7f);
        CHECK(lightInFog(point, Vec3(0.0f, 0.0f, -10.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f) == Vec3(0.0f));
        CHECK(lightInFog(point, Vec3(0.0f, 0.0f, -11.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f) == Vec3(0.0f));

        // A spot light as bright as a point light inside its inner cone, dark outside its
        // outer one, and in between as its cone says.
        const GpuLight spot = spotOf(0.3f, 0.5f, Vec3(2.0f, 1.0f, 0.5f));
        CHECK_NEAR(lightInFog(spot, Vec3(0.0f, 0.0f, -2.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f),
                   lightInFog(point, Vec3(0.0f, 0.0f, -2.0f), Vec3(0.0f, 0.0f, 1.0f), 0.0f), 1.0e-7f);
        const auto along = [](float angle) { return 2.0f * Vec3(std::sin(angle), 0.0f, -std::cos(angle)); };
        CHECK(lightInFog(spot, along(0.6f), Vec3(0.0f, 0.0f, 1.0f), 0.0f) == Vec3(0.0f));
        const float cone = (std::cos(0.4f) - std::cos(0.5f)) / (std::cos(0.3f) - std::cos(0.5f));
        CHECK_NEAR(lightInFog(spot, along(0.4f), Vec3(0.0f, 0.0f, 1.0f), 0.0f),
                   lightInFog(point, along(0.4f), Vec3(0.0f, 0.0f, 1.0f), 0.0f) * cone * cone, 1.0e-6f);

        // The sun is the same everywhere, and brightest seen against the way it shines when
        // the fog throws its light onwards.
        const GpuLight sun = sunOf(Vec3(0.0f, -1.0f, 0.0f), Vec3(1.0f));
        const Vec3 against = lightInFog(sun, Vec3(5.0f, 1.0f, 2.0f), Vec3(0.0f, -1.0f, 0.0f), 0.5f);
        CHECK_NEAR(against, Vec3(henyeyGreenstein(1.0f, 0.5f)), 1.0e-6f);
        CHECK(lightInFog(sun, Vec3(-300.0f, 7.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 0.5f) == against);
        CHECK(lightInFog(sun, Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f), 0.5f).x < against.x / 10.0f);

        // A light that gives the fog nothing.
        CHECK(lightInFog(pointOf(Vec3(0.0f), 10.0f, Vec3(0.0f)), Vec3(0.0f, 0.0f, -2.0f), Vec3(0.0f, 0.0f, 1.0f),
                         0.0f) == Vec3(0.0f));
    }

    TEST_CASE("a rect light throws the light of its face into the fog, as much of it as the face fills of all around") {
        // Half a metre wide and a quarter high: an eighth of a square metre.
        const Vec3 fogColor(2.0f, 1.0f, 0.5f);
        const GpuLight panel = rectOf(0.25f, 0.125f, fogColor);
        const GpuLight point = pointOf(Vec3(0.0f), 10.0f, fogColor);
        const Vec3 toViewer(0.0f, 0.0f, 1.0f);
        // Far along its axis it is the point light of the same fog color, ...
        const Vec3 ahead(0.0f, 0.0f, -6.0f);
        CHECK(relativeError(lightInFog(panel, ahead, toViewer, 0.0f), lightInFog(point, ahead, toViewer, 0.0f)) <
              2.0e-3f);
        // ... at a slant less by its cosine, the face being seen narrower; ...
        const Vec3 slanted = 6.0f * Vec3(std::sin(1.0f), 0.0f, -std::cos(1.0f));
        CHECK(relativeError(lightInFog(panel, slanted, toViewer, 0.0f),
                            lightInFog(point, slanted, toViewer, 0.0f) * std::cos(1.0f)) < 3.0e-3f);
        // ... and in front of its face no brighter than the face, which fills half of all
        // around there: its fog color over its area, times 2 pi, by the phase of 1 / (4 pi).
        CHECK(relativeError(lightInFog(panel, Vec3(0.0f, 0.0f, -1.0e-4f), toViewer, 0.0f), fogColor * 4.0f) < 2.0e-3f);
        // Behind it, in its plane and at its range, nothing.
        CHECK(lightInFog(panel, Vec3(0.0f, 0.0f, 1.0f), toViewer, 0.0f) == Vec3(0.0f));
        CHECK(lightInFog(panel, Vec3(1.0f, 0.0f, 0.0f), toViewer, 0.0f) == Vec3(0.0f));
        CHECK(lightInFog(panel, Vec3(0.0f, 0.0f, -10.0f), toViewer, 0.0f) == Vec3(0.0f));
        CHECK(lightInFog(panel, Vec3(0.0f, 0.0f, -9.0f), toViewer, 0.0f).x > 0.0f);
    }

    TEST_CASE("a fog lit by a sun that nothing shadows gets its light in proportion to how much fog there is") {
        FogSettings fog = evenFog(0.05f);
        const std::array<GpuLight, 1> lights = {sunOf(Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f))};
        // Across the line of sight: the phase function of a right angle.
        const Vec3 thrown = Vec3(2.0f) * henyeyGreenstein(0.0f, 0.0f);

        // At the end of a slice - the 32nd of 64 ends 50 * (1/2)^2 metres in - the sum is
        // what the fog has taken away up to there.
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(12.5f)), thrown * amountAt(fog, 12.5f), 1.0e-6f);
        // Beyond the grid, and the sky, in the closed form.
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(80.0f)), thrown * amountAt(fog, 80.0f), 1.0e-6f);
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(kInfinity)), thrown, 1.0e-6f);
        // As much at the end of the grid from either side.
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(50.0f)), thrown * amountAt(fog, 50.0f), 1.0e-6f);

        // Between the ends of two slices, a pixel gets their sums as far as it is between
        // them: here half way, by its share.
        const float halfWay = fogSliceDepth(32.5f / 64.0f, 50.0f);
        const float end = fogSliceDepth(33.0f / 64.0f, 50.0f);
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(halfWay)),
                   thrown * (amountAt(fog, 12.5f) + amountAt(fog, end)) * 0.5f, 1.0e-6f);
        // And in the first slice, between nothing and its end.
        const float first = fogSliceDepth(0.25f / 64.0f, 50.0f);
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(first)),
                   thrown * amountAt(fog, fogSliceDepth(1.0f / 64.0f, 50.0f)) * 0.25f, 1.0e-7f);

        // No more than the fog's largest opacity, and nothing where it has not begun.
        fog.maxOpacity = 0.3f;
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(kInfinity)), thrown * 0.3f, 1.0e-6f);
        fog.maxOpacity = 1.0f;
        fog.startDistance = 20.0f;
        CHECK(fogLightAlong(fog, lights, sightOf(12.5f)) == Vec3(0.0f));
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(kInfinity)), thrown, 1.0e-6f);
        // In the colors the fog throws on.
        fog.scattering = Vec3(0.5f, 1.0f, 0.0f);
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(kInfinity)), thrown * Vec3(0.5f, 1.0f, 0.0f), 1.0e-6f);

        // A fog that thins out above, seen from below: all there is of it.
        FogSettings layered = evenFog(0.2f);
        layered.height = 0.0f;
        layered.heightFalloff = 0.5f;
        const FogSight upwards{.viewer = Vec3(0.0f),
                               .direction = Vec3(0.0f, 1.0f, 0.0f),
                               .forward = Vec3(0.0f, 1.0f, 0.0f),
                               .distance = kInfinity,
                               .slices = 64};
        CHECK_NEAR(fogLightAlong(layered, lights, upwards),
                   thrown * fogAmountTowards(layered, Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f)), 1.0e-6f);
    }

    TEST_CASE("a point light's light in the fog comes nearer the integral the finer the slices are") {
        const FogSettings fog = evenFog(0.1f);
        const std::array<GpuLight, 1> lights = {pointOf(Vec3(2.0f, 0.0f, -10.0f), 8.0f, Vec3(5.0f))};
        const Vec3 integral = integratedAlong(fog, lights[0], 30.0f);
        REQUIRE(integral.x > 0.0f);

        const float coarse = relativeError(fogLightAlong(fog, lights, sightOf(30.0f, 16)), integral);
        const float medium = relativeError(fogLightAlong(fog, lights, sightOf(30.0f, 64)), integral);
        const float fine = relativeError(fogLightAlong(fog, lights, sightOf(30.0f, 256)), integral);
        CAPTURE(coarse);
        CAPTURE(medium);
        CAPTURE(fine);
        CHECK(medium < 1.0e-3f);
        CHECK(fine < medium);
        CHECK(medium < coarse);
        // Behind the light there is nothing more to add: past its range, the sum stands.
        CHECK_NEAR(fogLightAlong(fog, lights, sightOf(25.0f, 256)), fogLightAlong(fog, lights, sightOf(45.0f, 256)),
                   1.0e-6f);
    }

    TEST_CASE("nothing for a fog that lights do not light, a grid of no slices, or a line of sight that does not go "
              "forward") {
        FogSettings fog = evenFog(0.05f);
        const std::array<GpuLight, 1> lights = {sunOf(Vec3(1.0f, 0.0f, 0.0f), Vec3(2.0f))};
        CHECK(fogLightAlong(fog, lights, sightOf(0.0f)) == Vec3(0.0f));
        CHECK(fogLightAlong(fog, lights, sightOf(30.0f, 0)) == Vec3(0.0f));
        FogSight backwards = sightOf(30.0f);
        backwards.direction = Vec3(0.0f, 0.0f, 1.0f);
        CHECK(fogLightAlong(fog, lights, backwards) == Vec3(0.0f));
        CHECK(fogLightAlong(fog, {}, sightOf(30.0f)) == Vec3(0.0f));
        fog.volumetric = false;
        CHECK(fogLightAlong(fog, lights, sightOf(30.0f)) == Vec3(0.0f));
        fog.volumetric = true;
        fog.density = 0.0f;
        CHECK(fogLightAlong(fog, lights, sightOf(30.0f)) == Vec3(0.0f));
    }
}
