#include "cordite/renderer/AmbientOcclusion.h"

#include "cordite/math/Random.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/ScreenEffects.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

using namespace cordite;

namespace {

Vec3 randomDirection(Random& random) {
    return glm::normalize(Vec3(random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f), random.range(-1.0f, 1.0f)));
}

/// Two directions at right angles to `toViewer` and to each other: the screen, as far
/// as slices are concerned.
struct Screen {
    explicit Screen(const Vec3& toViewer)
        : right(glm::normalize(glm::cross(std::abs(toViewer.y) < 0.9f ? kWorldUp : Vec3(1.0f, 0.0f, 0.0f), toViewer))),
          up(glm::cross(toViewer, right)) {}

    Vec3 right;
    Vec3 up;
};

/// How much light from all around reaches a surface that faces `normal`, when all of
/// the world on the far side of a plane through the point is hidden - `open` is the
/// plane's normal, on the side that is not: every slice asked, as the shader asks them.
float visibilityBesideAWall(const Vec3& normal, const Vec3& toViewer, const Vec3& open) {
    constexpr int kSlices = 720;
    const Screen screen(toViewer);
    double total = 0.0;
    for (int index = 0; index < kSlices; ++index) {
        const float turn = (static_cast<float>(index) + 0.5f) / static_cast<float>(kSlices) * kPi;
        const Vec3 direction = screen.right * std::cos(turn) + screen.up * std::sin(turn);
        const SliceNormal seen = projectIntoSlice(normal, toViewer, direction);

        // In the slice, a direction at an angle a from the viewer is
        // toViewer * cos(a) + direction * sin(a). The wall hides those on its far side,
        // which leaves half a turn open: from where the wall's plane crosses the slice
        // on one side of the viewer to where it does on the other.
        const float towardsViewer = glm::dot(open, toViewer);
        const float along = glm::dot(open, direction);
        float before = -kHalfPi;
        float after = kHalfPi;
        if (along < 0.0f) {
            after = std::atan2(towardsViewer, -along);
            before = after - kPi;
        } else if (along > 0.0f) {
            before = -std::atan2(towardsViewer, along);
            after = before + kPi;
        }
        total += static_cast<double>(seen.length) * sliceVisibility(seen.angle, before, after);
    }
    return static_cast<float>(total / kSlices);
}

} // namespace

TEST_SUITE("AmbientOcclusion") {
    TEST_CASE("a slice seen along the normal lets through the square of the sine of its horizons") {
        // With the normal towards the viewer and the horizon at the same angle on both
        // sides, the light comes through a cone: of all of it, the sine squared.
        for (const float degrees : {0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 90.0f}) {
            CAPTURE(degrees);
            const float horizon = toRadians(degrees);
            const float sine = std::sin(horizon);
            CHECK(sliceVisibility(0.0f, -horizon, horizon) == doctest::Approx(sine * sine).epsilon(1.0e-4));
        }
        // One side open and the other shut is half of it.
        CHECK(sliceVisibility(0.0f, -kHalfPi, 0.0f) == doctest::Approx(0.5f));
        CHECK(sliceVisibility(0.0f, 0.0f, kHalfPi) == doctest::Approx(0.5f));
        CHECK(sliceVisibility(0.0f, 0.0f, 0.0f) == doctest::Approx(0.0f));
        CHECK(openSliceVisibility(0.0f) == 1.0f);
    }

    TEST_CASE("a horizon further round than a right angle from the normal is the surface's own") {
        for (const float degrees : {-70.0f, -30.0f, 0.0f, 20.0f, 55.0f, 85.0f}) {
            CAPTURE(degrees);
            const float normal = toRadians(degrees);
            const float open = openSliceVisibility(normal);
            // Exactly at the surface, and anywhere beyond it.
            CHECK(sliceVisibility(normal, normal - kHalfPi, normal + kHalfPi) == doctest::Approx(open).epsilon(1.0e-4));
            CHECK(sliceVisibility(normal, -kPi, kPi) == doctest::Approx(open).epsilon(1.0e-4));
            CHECK(sliceVisibility(normal, -10.0f, 10.0f) == doctest::Approx(open).epsilon(1.0e-4));
            // More light never comes through a slice than through an open one, and never
            // less than none.
            CHECK(sliceVisibility(normal, -0.3f, 0.4f) <= open + 1.0e-5f);
            CHECK(sliceVisibility(normal, -0.3f, 0.4f) >= -1.0e-5f);
            // Horizons that have closed in on the viewer from both sides let nothing through.
            CHECK(sliceVisibility(normal, 0.0f, 0.0f) == doctest::Approx(0.0f).epsilon(1.0e-4));
        }
        // An open slice lets through the more the further its normal leans: 1 seen
        // head-on, half of pi seen edge-on.
        CHECK(openSliceVisibility(toRadians(60.0f)) ==
              doctest::Approx(0.5f + toRadians(60.0f) * std::sin(toRadians(60.0f))).epsilon(1.0e-5));
        CHECK(openSliceVisibility(kHalfPi) == doctest::Approx(kHalfPi));
        CHECK(openSliceVisibility(-0.7f) == doctest::Approx(openSliceVisibility(0.7f)));
    }

    TEST_CASE("a slice sees the normal by its shadow on the slice") {
        const Vec3 toViewer(0.0f, 0.0f, 1.0f);
        const Vec3 across(1.0f, 0.0f, 0.0f);
        // Towards the viewer: no angle, full length, in every slice.
        SliceNormal seen = projectIntoSlice(toViewer, toViewer, across);
        CHECK(seen.angle == doctest::Approx(0.0f));
        CHECK(seen.length == doctest::Approx(1.0f));
        // Leaning the way the slice runs: a positive angle; the other way: a negative one.
        seen = projectIntoSlice(glm::normalize(Vec3(1.0f, 0.0f, 1.0f)), toViewer, across);
        CHECK(seen.angle == doctest::Approx(toRadians(45.0f)));
        CHECK(seen.length == doctest::Approx(1.0f));
        seen = projectIntoSlice(glm::normalize(Vec3(-1.0f, 0.0f, std::sqrt(3.0f))), toViewer, across);
        CHECK(seen.angle == doctest::Approx(toRadians(-30.0f)));
        // Leaning out of the slice: its shadow is shorter, and points at the viewer.
        seen = projectIntoSlice(glm::normalize(Vec3(0.0f, 1.0f, 1.0f)), toViewer, across);
        CHECK(seen.angle == doctest::Approx(0.0f).epsilon(1.0e-4));
        CHECK(seen.length == doctest::Approx(std::sqrt(0.5f)));
        // At a right angle to the slice: no shadow at all.
        seen = projectIntoSlice(Vec3(0.0f, 1.0f, 0.0f), toViewer, across);
        CHECK(seen.length == doctest::Approx(0.0f));
        CHECK(seen.angle == 0.0f);

        // The slice's direction may be of any length, and lean towards the viewer.
        const SliceNormal plain = projectIntoSlice(glm::normalize(Vec3(0.4f, 0.3f, 0.8f)), toViewer, across);
        const SliceNormal leaning =
            projectIntoSlice(glm::normalize(Vec3(0.4f, 0.3f, 0.8f)), toViewer, Vec3(5.0f, 0.0f, 3.0f));
        CHECK(leaning.angle == doctest::Approx(plain.angle));
        CHECK(leaning.length == doctest::Approx(plain.length));
        // A slice that runs the other way sees the same normal on its other side.
        const SliceNormal back = projectIntoSlice(glm::normalize(Vec3(0.4f, 0.3f, 0.8f)), toViewer, -across);
        CHECK(back.angle == doctest::Approx(-plain.angle));
        CHECK(back.length == doctest::Approx(plain.length));
    }

    TEST_CASE("what stands beside a point rises above its surface by an angle in the slice it is in") {
        const Vec3 up(0.0f, 1.0f, 0.0f);
        // Seen from straight above: the rise is the angle above the floor.
        CHECK(riseAboveSurface(Vec3(1.0f, 0.0f, 0.0f), up, up) == doctest::Approx(0.0f));
        CHECK(riseAboveSurface(glm::normalize(Vec3(1.0f, 1.0f, 0.0f)), up, up) == doctest::Approx(toRadians(45.0f)));
        CHECK(riseAboveSurface(glm::normalize(Vec3(-1.0f, std::sqrt(3.0f), 0.0f)), up, up) ==
              doctest::Approx(toRadians(60.0f)));
        // Below the floor: less than nothing.
        CHECK(riseAboveSurface(glm::normalize(Vec3(1.0f, -1.0f, 0.0f)), up, up) == doctest::Approx(toRadians(-45.0f)));

        // Anything on the surface rises by nothing, wherever it is and wherever it is
        // seen from - which is what keeps a floor seen at a low angle out of its own light.
        Random random(9);
        for (int trial = 0; trial < 200; ++trial) {
            const Vec3 normal = randomDirection(random);
            Vec3 toViewer = randomDirection(random);
            if (glm::dot(normal, toViewer) < 0.02f) {
                toViewer = glm::normalize(toViewer + normal * (0.05f - 2.0f * glm::dot(normal, toViewer)));
            }
            const Vec3 any = randomDirection(random);
            const Vec3 onSurface = normalizeOr(any - normal * glm::dot(any, normal), Vec3(0.0f));
            if (onSurface == Vec3(0.0f)) {
                continue;
            }
            CAPTURE(normal);
            CAPTURE(toViewer);
            CAPTURE(onSurface);
            CHECK(std::abs(riseAboveSurface(onSurface, normal, toViewer)) < 1.0e-4f);
        }

        // In a slice, the rise is the way from the surface's edge on that side to the
        // direction: what a horizon measured from the viewer and the normal's angle give.
        for (int trial = 0; trial < 200; ++trial) {
            const Vec3 toViewer = randomDirection(random);
            Vec3 normal = randomDirection(random);
            if (glm::dot(normal, toViewer) < 0.05f) {
                normal = glm::normalize(normal + toViewer * (0.1f - 2.0f * glm::dot(normal, toViewer)));
            }
            const Screen screen(toViewer);
            const float turn = random.range(0.0f, kPi);
            const Vec3 direction = screen.right * std::cos(turn) + screen.up * std::sin(turn);
            const SliceNormal seen = projectIntoSlice(normal, toViewer, direction);
            if (seen.length < 0.05f) {
                continue;
            }
            CAPTURE(toViewer);
            CAPTURE(normal);
            CAPTURE(turn);
            // Something at an angle from the viewer, on the side the slice runs to, between
            // the viewer and the surface.
            const float open = seen.angle + kHalfPi;
            const float angle = random.range(0.02f, 0.98f) * open;
            const Vec3 towards = toViewer * std::cos(angle) + direction * std::sin(angle);
            CHECK(riseAboveSurface(towards, normal, toViewer) == doctest::Approx(open - angle).epsilon(2.0e-3));
            // And on the other side.
            const float openBefore = kHalfPi - seen.angle;
            const float angleBefore = random.range(0.02f, 0.98f) * openBefore;
            const Vec3 back = toViewer * std::cos(angleBefore) - direction * std::sin(angleBefore);
            CHECK(riseAboveSurface(back, normal, toViewer) ==
                  doctest::Approx(openBefore - angleBefore).epsilon(2.0e-3));
        }

        // Towards the viewer there is no slice to measure in.
        CHECK(riseAboveSurface(up, up, up) == 0.0f);
    }

    TEST_CASE("the open slices of any surface add up to all the light there is") {
        Random random(21);
        for (int trial = 0; trial < 40; ++trial) {
            const Vec3 toViewer = randomDirection(random);
            // A surface the viewer can see.
            Vec3 normal = randomDirection(random);
            if (glm::dot(normal, toViewer) < 0.05f) {
                normal = glm::normalize(normal + toViewer * (0.1f - 2.0f * glm::dot(normal, toViewer)));
            }
            CAPTURE(toViewer);
            CAPTURE(normal);
            const Screen screen(toViewer);
            constexpr int kSlices = 360;
            double total = 0.0;
            for (int index = 0; index < kSlices; ++index) {
                const float turn = (static_cast<float>(index) + 0.5f) / static_cast<float>(kSlices) * kPi;
                const SliceNormal seen =
                    projectIntoSlice(normal, toViewer, screen.right * std::cos(turn) + screen.up * std::sin(turn));
                total += static_cast<double>(seen.length) * openSliceVisibility(seen.angle);
            }
            CHECK(total / kSlices == doctest::Approx(1.0).epsilon(2.0e-3));
        }
    }

    TEST_CASE("beside a wall a surface gets the light of the part of the sky the wall leaves") {
        // A floor at the foot of a wall that stands on it: half.
        const Vec3 up(0.0f, 1.0f, 0.0f);
        const Vec3 awayFromWall(1.0f, 0.0f, 0.0f);
        for (const Vec3& toViewer : {Vec3(0.0f, 1.0f, 0.0f), glm::normalize(Vec3(1.0f, 1.0f, 0.0f)),
                                     glm::normalize(Vec3(0.3f, 0.2f, 0.9f)), glm::normalize(Vec3(2.0f, 0.3f, -1.0f))}) {
            CAPTURE(toViewer);
            CHECK(visibilityBesideAWall(up, toViewer, awayFromWall) == doctest::Approx(0.5f).epsilon(0.01));
        }

        // In general: half of one plus the cosine of the angle between the way the
        // surface faces and the way the wall leaves open - whatever it is seen from.
        Random random(5);
        int tried = 0;
        while (tried < 60) {
            const Vec3 normal = randomDirection(random);
            const Vec3 open = randomDirection(random);
            const Vec3 toViewer = randomDirection(random);
            // The viewer sees the surface, from the open side of the wall.
            if (glm::dot(normal, toViewer) < 0.1f || glm::dot(open, toViewer) < 0.1f) {
                continue;
            }
            ++tried;
            CAPTURE(normal);
            CAPTURE(open);
            CAPTURE(toViewer);
            const float expected = 0.5f * (1.0f + glm::dot(normal, open));
            CHECK(visibilityBesideAWall(normal, toViewer, open) == doctest::Approx(expected).epsilon(0.01).scale(1.0));
        }
    }

    TEST_CASE("the settings of a view come from the look of the scene and the quality of the view") {
        PostProcessSettings look;
        look.ambientOcclusionIntensity = 1.5f;
        look.ambientOcclusionRadius = 0.8f;
        look.ambientOcclusionBias = 6.0f;

        const AmbientOcclusionSettings medium = ambientOcclusionSettingsFor(look, ScreenEffectsQuality::Medium);
        CHECK(medium.isEnabled());
        CHECK(medium.intensity == 1.5f);
        CHECK(medium.radius == 0.8f);
        CHECK(medium.bias == doctest::Approx(toRadians(6.0f)));
        CHECK(medium.divisor == 2);
        CHECK(medium.slices == 2);
        CHECK(medium.steps == 4);

        const AmbientOcclusionSettings high = ambientOcclusionSettingsFor(look, ScreenEffectsQuality::High);
        CHECK(high.divisor == 1);
        CHECK(high.slices == 3);
        CHECK(high.steps == 5);
        CHECK(high.intensity == 1.5f);
        CHECK(ambientOcclusionSettingsFor(look, ScreenEffectsQuality::Low).steps == 2);

        // Switched off by the view, or by the scene.
        CHECK_FALSE(ambientOcclusionSettingsFor(look, ScreenEffectsQuality::Off).isEnabled());
        look.ambientOcclusionIntensity = 0.0f;
        CHECK_FALSE(ambientOcclusionSettingsFor(look, ScreenEffectsQuality::High).isEnabled());

        // What a scene says when it says nothing.
        const AmbientOcclusionSettings plain = ambientOcclusionSettingsFor({}, ScreenEffectsQuality::Medium);
        CHECK(plain.intensity == 1.0f);
        CHECK(plain.radius == 0.5f);
        CHECK(plain.bias == doctest::Approx(toRadians(2.0f)));
    }

    TEST_CASE("the qualities of the screen effects have the names scripts use") {
        CHECK(nameOf(ScreenEffectsQuality::Off) == "off");
        CHECK(nameOf(ScreenEffectsQuality::Low) == "low");
        CHECK(nameOf(ScreenEffectsQuality::Medium) == "medium");
        CHECK(nameOf(ScreenEffectsQuality::High) == "high");
        CHECK(displayNameOf(ScreenEffectsQuality::Off) == "Off");
        CHECK(displayNameOf(ScreenEffectsQuality::High) == "High");
        for (const ScreenEffectsQuality quality : kScreenEffectsQualities) {
            CHECK(screenEffectsQualityFromName(nameOf(quality)) == quality);
        }
        CHECK(screenEffectsQualityFromName("MEDIUM") == ScreenEffectsQuality::Medium);
        CHECK_FALSE(screenEffectsQualityFromName("ultra").has_value());
        CHECK_FALSE(screenEffectsQualityFromName("").has_value());

        // What a quality costs never goes down on the way up, and "off" has the numbers
        // of the lowest.
        CHECK(screenEffectsSettingsFor(ScreenEffectsQuality::Off) ==
              screenEffectsSettingsFor(ScreenEffectsQuality::Low));
        const ScreenEffectsQualitySettings low = screenEffectsSettingsFor(ScreenEffectsQuality::Low);
        const ScreenEffectsQualitySettings medium = screenEffectsSettingsFor(ScreenEffectsQuality::Medium);
        const ScreenEffectsQualitySettings high = screenEffectsSettingsFor(ScreenEffectsQuality::High);
        CHECK(low.ambientOcclusionSlices * low.ambientOcclusionSteps <
              medium.ambientOcclusionSlices * medium.ambientOcclusionSteps);
        CHECK(medium.ambientOcclusionSlices * medium.ambientOcclusionSteps <
              high.ambientOcclusionSlices * high.ambientOcclusionSteps);
        CHECK(low.ambientOcclusionDivisor >= medium.ambientOcclusionDivisor);
        CHECK(medium.ambientOcclusionDivisor >= high.ambientOcclusionDivisor);
        CHECK(high.ambientOcclusionDivisor == 1);
    }
}
