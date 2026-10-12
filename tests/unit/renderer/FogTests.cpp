#include "cordite/renderer/Fog.h"

#include "cordite/math/Random.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

using namespace cordite;

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A fog that is as dense as `density` up to a height of two metres and loses an e-th
/// of that every four metres above.
FogSettings layeredFog(float density = 0.5f) {
    FogSettings fog;
    fog.color = Vec3(0.2f, 0.3f, 0.4f);
    fog.density = density;
    fog.height = 2.0f;
    fog.heightFalloff = 0.25f;
    return fog;
}

/// How dense a fog is at a height, as FogSettings says: the same up to `height`, and
/// less by a factor of e for every 1 / heightFalloff metres above.
double densityAt(const FogSettings& fog, double height) {
    const double above = std::max(height - static_cast<double>(fog.height), 0.0);
    return static_cast<double>(fog.density) * std::exp(-static_cast<double>(fog.heightFalloff) * above);
}

/// The fog on the way from `from` to `to`, added up over many short steps: what the
/// closed form is to agree with.
double fogAddedUp(const FogSettings& fog, const Vec3& from, const Vec3& to) {
    const double length = glm::length(glm::dvec3(to) - glm::dvec3(from));
    const double clear = fog.startDistance;
    if (length <= clear) {
        return 0.0;
    }
    constexpr int kSteps = 20000;
    double total = 0.0;
    for (int step = 0; step < kSteps; ++step) {
        const double distance = clear + (length - clear) * (static_cast<double>(step) + 0.5) / kSteps;
        const double height = static_cast<double>(from.y) + (static_cast<double>(to.y) - from.y) * distance / length;
        total += densityAt(fog, height);
    }
    return total / kSteps * (length - clear);
}

} // namespace

TEST_SUITE("Fog") {
    TEST_CASE("clear air leaves the light as it is") {
        const FogSettings air;
        CHECK_FALSE(air.isEnabled());
        const Vec3 from(1.0f, 2.0f, 3.0f);
        const Vec3 to(-40.0f, 9.0f, -300.0f);
        CHECK(fogOpticalDepth(air, from, to) == 0.0f);
        CHECK(fogOpticalDepthTowards(air, from, kWorldForward) == 0.0f);
        CHECK(fogAmount(air, from, to) == 0.0f);
        CHECK(fogAmountTowards(air, from, kWorldForward) == 0.0f);
        CHECK(applyFog(air, Vec3(0.3f, 0.6f, 0.9f), from, to) == Vec3(0.3f, 0.6f, 0.9f));
        CHECK(applyFogTowards(air, Vec3(0.3f, 0.6f, 0.9f), from, kWorldForward) == Vec3(0.3f, 0.6f, 0.9f));

        // A fog that may take none of a pixel is none either, however dense.
        FogSettings unseen = layeredFog();
        unseen.maxOpacity = 0.0f;
        CHECK_FALSE(unseen.isEnabled());
        CHECK(fogAmount(unseen, from, to) == 0.0f);
        CHECK(applyFog(unseen, Vec3(0.3f, 0.6f, 0.9f), from, to) == Vec3(0.3f, 0.6f, 0.9f));
    }

    TEST_CASE("an even fog takes the same part of the light with every metre, whichever way") {
        FogSettings fog;
        fog.density = 0.02f;
        REQUIRE(fog.isEnabled());
        const Vec3 from(3.0f, -1.0f, 7.0f);
        for (const Vec3& direction : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                      glm::normalize(Vec3(1.0f, 2.0f, -3.0f))}) {
            CAPTURE(direction);
            CHECK(fogOpticalDepth(fog, from, from + direction * 10.0f) == doctest::Approx(0.2f).epsilon(1.0e-5));
            CHECK(fogOpticalDepth(fog, from, from + direction * 100.0f) == doctest::Approx(2.0f).epsilon(1.0e-5));
            // 1 - e^-0.2 and 1 - e^-2: what the header promises for 0.02.
            CHECK(fogAmount(fog, from, from + direction * 10.0f) == doctest::Approx(0.18126925f).epsilon(1.0e-5));
            CHECK(fogAmount(fog, from, from + direction * 100.0f) == doctest::Approx(0.86466472f).epsilon(1.0e-5));
            // It never ends, so nothing shows through it from infinitely far away.
            CHECK(fogOpticalDepthTowards(fog, from, direction) == kInfinity);
            CHECK(fogAmountTowards(fog, from, direction) == 1.0f);
        }
        CHECK(fogOpticalDepth(fog, from, from) == 0.0f);
        CHECK(fogAmount(fog, from, from) == 0.0f);
    }

    TEST_CASE("a fog is at its densest up to its height and thins out above, by the same factor every metre") {
        const FogSettings fog = layeredFog();

        // Straight up from its height, through four metres: density / falloff * (1 - 1 / e).
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 6.0f, 0.0f)) ==
              doctest::Approx(1.2642411f).epsilon(1.0e-5));
        // Below its height it is the same everywhere: half of the light a metre, for five.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, -3.0f, 0.0f), Vec3(0.0f, 2.0f, 0.0f)) ==
              doctest::Approx(2.5f).epsilon(1.0e-5));
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, -30.0f, 0.0f), Vec3(4.0f, -30.0f, 3.0f)) ==
              doctest::Approx(2.5f).epsilon(1.0e-5));
        // A way that crosses the height has both: two metres below, four above.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 6.0f, 0.0f)) ==
              doctest::Approx(2.2642411f).epsilon(1.0e-5));
        // The same heights at a slant: ten metres of way for six of height.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 0.0f, 0.0f), Vec3(8.0f, 6.0f, 0.0f)) ==
              doctest::Approx(3.7737352f).epsilon(1.0e-5));
        // High up there is next to none of it left: at 82 metres, e^-20 of it.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 82.0f, 0.0f), Vec3(100.0f, 82.0f, 0.0f)) ==
              doctest::Approx(50.0f * std::exp(-20.0f)).epsilon(1.0e-4));

        // A level way has the density of its height, whatever that is.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f, 6.0f, -10.0f)) ==
              doctest::Approx(5.0f * std::exp(-1.0f)).epsilon(1.0e-5));
        // And one that is all but level gets the same as its neighbours that are.
        CHECK(fogOpticalDepth(fog, Vec3(0.0f, 6.0f, 0.0f), Vec3(0.0f, 6.00001f, -10.0f)) ==
              doctest::Approx(5.0f * std::exp(-1.0f)).epsilon(1.0e-4));
    }

    TEST_CASE("the fog on a way is its density added up along the way") {
        // The closed form against a sum over twenty thousand steps, for ways above,
        // below and through the height of fogs that thin out slowly, fast and not at all.
        Random random(20241012);
        for (const float falloff : {0.0f, 0.004f, 0.25f, 3.0f}) {
            for (const float clear : {0.0f, 4.0f}) {
                FogSettings fog = layeredFog(0.05f);
                fog.heightFalloff = falloff;
                fog.startDistance = clear;
                for (int index = 0; index < 60; ++index) {
                    const Vec3 from(random.range(-20.0f, 20.0f), random.range(-8.0f, 14.0f),
                                    random.range(-20.0f, 20.0f));
                    const Vec3 to(random.range(-20.0f, 20.0f), random.range(-8.0f, 14.0f), random.range(-20.0f, 20.0f));
                    CAPTURE(falloff);
                    CAPTURE(clear);
                    CAPTURE(from);
                    CAPTURE(to);
                    const auto expected = static_cast<float>(fogAddedUp(fog, from, to));
                    CHECK(fogOpticalDepth(fog, from, to) == doctest::Approx(expected).epsilon(2.0e-4));
                }
            }
        }
    }

    TEST_CASE("it is the same fog from either end of a way") {
        const FogSettings fog = layeredFog();
        Random random(7);
        for (int index = 0; index < 40; ++index) {
            const Vec3 from(random.range(-9.0f, 9.0f), random.range(-9.0f, 9.0f), random.range(-9.0f, 9.0f));
            const Vec3 to(random.range(-9.0f, 9.0f), random.range(-9.0f, 9.0f), random.range(-9.0f, 9.0f));
            CAPTURE(from);
            CAPTURE(to);
            CHECK(fogOpticalDepth(fog, from, to) == doctest::Approx(fogOpticalDepth(fog, to, from)).epsilon(1.0e-5));
        }
    }

    TEST_CASE("the fog begins at its start distance from the viewer") {
        FogSettings even;
        even.density = 0.02f;
        even.startDistance = 5.0f;
        const Vec3 from(1.0f, 1.0f, 1.0f);
        CHECK(fogOpticalDepth(even, from, from + Vec3(0.0f, 0.0f, -4.0f)) == 0.0f);
        CHECK(fogOpticalDepth(even, from, from + Vec3(0.0f, 0.0f, -5.0f)) == 0.0f);
        CHECK(fogAmount(even, from, from + Vec3(0.0f, 0.0f, -5.0f)) == 0.0f);
        // Ten metres away, five of them are fog.
        CHECK(fogOpticalDepth(even, from, from + Vec3(0.0f, 0.0f, -10.0f)) == doctest::Approx(0.1f).epsilon(1.0e-5));

        // Where the fog thins out, what counts is the height at which the way enters it:
        // two metres up, this one is at the fog's height, and has four more to go.
        FogSettings layered = layeredFog();
        layered.startDistance = 2.0f;
        CHECK(fogOpticalDepth(layered, Vec3(0.0f), Vec3(0.0f, 6.0f, 0.0f)) ==
              doctest::Approx(1.2642411f).epsilon(1.0e-5));
        CHECK(fogOpticalDepthTowards(layered, Vec3(0.0f), kWorldUp) == doctest::Approx(2.0f).epsilon(1.0e-5));
    }

    TEST_CASE("a line of sight without end comes out of a fog that thins out, if it climbs") {
        const FogSettings fog = layeredFog();
        const Vec3 atHeight(0.0f, 2.0f, 0.0f);

        // Straight up from its height there is density / falloff of it, all told.
        CHECK(fogOpticalDepthTowards(fog, atHeight, kWorldUp) == doctest::Approx(2.0f).epsilon(1.0e-5));
        CHECK(fogAmountTowards(fog, atHeight, kWorldUp) == doctest::Approx(0.86466472f).epsilon(1.0e-5));
        // At thirty degrees the same heights take twice as long.
        const Vec3 slanted(std::sqrt(0.75f), 0.5f, 0.0f);
        CHECK(fogOpticalDepthTowards(fog, atHeight, slanted) == doctest::Approx(4.0f).epsilon(1.0e-5));
        CHECK(fogAmountTowards(fog, atHeight, slanted) == doctest::Approx(0.98168436f).epsilon(1.0e-5));
        // From four metres higher, an e-th of it is left.
        CHECK(fogOpticalDepthTowards(fog, Vec3(0.0f, 6.0f, 0.0f), kWorldUp) ==
              doctest::Approx(0.73575888f).epsilon(1.0e-5));
        // From two metres below, there are those two metres at full density first.
        CHECK(fogOpticalDepthTowards(fog, Vec3(0.0f), kWorldUp) == doctest::Approx(3.0f).epsilon(1.0e-5));

        // It is what the fog on a way comes to as the way gets longer.
        for (const Vec3& from : {Vec3(0.0f), atHeight, Vec3(5.0f, 9.0f, -2.0f)}) {
            for (const Vec3& direction : {kWorldUp, slanted, glm::normalize(Vec3(-3.0f, 1.0f, 2.0f))}) {
                CAPTURE(from);
                CAPTURE(direction);
                const float farAway = fogOpticalDepth(fog, from, from + direction * 4000.0f);
                CHECK(fogOpticalDepthTowards(fog, from, direction) == doctest::Approx(farAway).epsilon(1.0e-4));
            }
        }

        // A line that is level, or goes down, never comes out of it.
        for (const Vec3& direction : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), Vec3(0.6f, -0.8f, 0.0f)}) {
            CAPTURE(direction);
            CHECK(fogOpticalDepthTowards(fog, Vec3(0.0f, 50.0f, 0.0f), direction) == kInfinity);
            CHECK(fogAmountTowards(fog, Vec3(0.0f, 50.0f, 0.0f), direction) == 1.0f);
        }
        // And the less a line climbs, the more fog it has to cross: there is no jump
        // between the two. (In a fog thin enough for the difference to be seen.)
        const FogSettings thin = layeredFog(0.005f);
        float before = 0.0f;
        for (const float degrees : {80.0f, 40.0f, 10.0f, 1.0f, 0.01f}) {
            const float angle = toRadians(degrees);
            const float amount = fogAmountTowards(thin, atHeight, Vec3(std::cos(angle), std::sin(angle), 0.0f));
            CAPTURE(degrees);
            CHECK(amount > before);
            before = amount;
        }
        CHECK(before == doctest::Approx(1.0f));
    }

    TEST_CASE("the fog never takes more of a pixel than its greatest opacity") {
        FogSettings fog = layeredFog();
        fog.maxOpacity = 0.6f;
        const Vec3 from(0.0f, 1.0f, 0.0f);
        // Thick fog: all of it would be fog.
        CHECK(fogAmount(fog, from, from + Vec3(500.0f, 0.0f, 0.0f)) == 0.6f);
        CHECK(fogAmountTowards(fog, from, Vec3(1.0f, 0.0f, 0.0f)) == 0.6f);
        CHECK_NEAR(applyFogTowards(fog, Vec3(1.0f), from, Vec3(1.0f, 0.0f, 0.0f)), Vec3(0.52f, 0.58f, 0.64f), 1.0e-6f);
        // Thin fog is as it would be without.
        CHECK(fogAmount(fog, from, from + Vec3(0.2f, 0.0f, 0.0f)) == doctest::Approx(1.0f - std::exp(-0.1f)));
    }

    TEST_CASE("the fog has its own color, and the sun's where the view looks at the sun") {
        FogSettings fog = layeredFog();
        fog.sunColor = Vec3(1.0f, 0.5f, 0.25f);
        fog.towardsSun = Vec3(0.0f, 0.0f, -1.0f);
        fog.sunExponent = 2.0f;

        CHECK_NEAR(fogColorAlong(fog, Vec3(0.0f, 0.0f, -1.0f)), Vec3(1.2f, 0.8f, 0.65f), 1.0e-6f);
        // Sixty degrees from the sun: the cosine is a half, its square a quarter.
        CHECK_NEAR(fogColorAlong(fog, Vec3(std::sqrt(0.75f), 0.0f, -0.5f)), Vec3(0.45f, 0.425f, 0.4625f), 1.0e-6f);
        // Nothing of it at a right angle, or with the sun behind.
        CHECK_NEAR(fogColorAlong(fog, Vec3(1.0f, 0.0f, 0.0f)), Vec3(0.2f, 0.3f, 0.4f), 1.0e-6f);
        CHECK_NEAR(fogColorAlong(fog, Vec3(0.0f, 0.0f, 1.0f)), Vec3(0.2f, 0.3f, 0.4f), 1.0e-6f);

        // The higher the exponent, the closer around the sun the light stays.
        fog.sunExponent = 32.0f;
        const Vec3 nearby = fogColorAlong(fog, glm::normalize(Vec3(0.2f, 0.0f, -1.0f)));
        fog.sunExponent = 2.0f;
        CHECK(nearby.r < fogColorAlong(fog, glm::normalize(Vec3(0.2f, 0.0f, -1.0f))).r);

        // A fog without a sun's light has its own color everywhere.
        fog.sunColor = Vec3(0.0f);
        CHECK(fogColorAlong(fog, Vec3(0.0f, 0.0f, -1.0f)) == Vec3(0.2f, 0.3f, 0.4f));
    }

    TEST_CASE("what is seen through fog is what the fog lets through, and the fog's color for the rest") {
        // Half of the light gets through ten metres of this one: e^-(ln 2).
        FogSettings fog;
        fog.color = Vec3(0.0f, 0.0f, 1.0f);
        fog.density = std::log(2.0f) / 10.0f;
        const Vec3 from(0.0f, 1.0f, 0.0f);
        CHECK_NEAR(applyFog(fog, Vec3(1.0f, 0.0f, 0.0f), from, from + Vec3(0.0f, 0.0f, -10.0f)), Vec3(0.5f, 0.0f, 0.5f),
                   1.0e-6f);
        CHECK_NEAR(applyFog(fog, Vec3(1.0f, 0.0f, 0.0f), from, from + Vec3(0.0f, 0.0f, -20.0f)),
                   Vec3(0.25f, 0.0f, 0.75f), 1.0e-6f);
        CHECK(applyFog(fog, Vec3(1.0f, 0.0f, 0.0f), from, from) == Vec3(1.0f, 0.0f, 0.0f));
        // The sky is behind all of it.
        CHECK(applyFogTowards(fog, Vec3(1.0f, 0.0f, 0.0f), from, kWorldUp) == Vec3(0.0f, 0.0f, 1.0f));

        // Towards the sun the fog that is seen is the brighter one.
        fog.sunColor = Vec3(2.0f, 0.0f, 0.0f);
        fog.towardsSun = Vec3(0.0f, 0.0f, -1.0f);
        fog.sunExponent = 1.0f;
        CHECK_NEAR(applyFog(fog, Vec3(0.0f), from, from + Vec3(0.0f, 0.0f, -10.0f)), Vec3(1.0f, 0.0f, 0.5f), 1.0e-6f);
        CHECK_NEAR(applyFog(fog, Vec3(0.0f), from, from + Vec3(0.0f, 0.0f, 10.0f)), Vec3(0.0f, 0.0f, 0.5f), 1.0e-6f);
    }

    TEST_CASE("a fog's numbers are brought into their ranges before they are used") {
        FogSettings wild;
        wild.color = Vec3(-1.0f, 0.5f, 1.0e9f);
        wild.density = -3.0f;
        wild.height = kNotANumber;
        wild.heightFalloff = kInfinity;
        wild.startDistance = -10.0f;
        wild.maxOpacity = 7.0f;
        wild.sunColor = Vec3(kNotANumber, 1.0f, 1.0f);
        wild.towardsSun = Vec3(0.0f, 0.0f, -4.0f);
        wild.sunExponent = 0.0f;
        const FogSettings tame = sanitized(wild);
        CHECK(tame.color == Vec3(0.0f, 0.5f, 65504.0f));
        CHECK(tame.density == 0.0f);
        CHECK(tame.height == 0.0f);
        CHECK(tame.heightFalloff == 0.0f);
        CHECK(tame.startDistance == 0.0f);
        CHECK(tame.maxOpacity == 1.0f);
        CHECK(tame.sunColor == Vec3(0.0f));
        CHECK(tame.towardsSun == Vec3(0.0f, 0.0f, -1.0f));
        CHECK(tame.sunExponent == 1.0f);
        CHECK_FALSE(tame.isEnabled());

        wild.density = kNotANumber;
        wild.maxOpacity = -1.0f;
        wild.towardsSun = Vec3(0.0f);
        wild.sunExponent = kNotANumber;
        wild.heightFalloff = 1.0e9f;
        wild.startDistance = kInfinity;
        const FogSettings other = sanitized(wild);
        CHECK(other.density == 0.0f);
        CHECK(other.maxOpacity == 0.0f);
        CHECK(other.towardsSun == kWorldUp);
        CHECK(other.sunExponent == 8.0f);
        CHECK(other.heightFalloff == 100.0f);
        CHECK(other.startDistance == 0.0f);

        // What is in its range already is left as it is.
        FogSettings fine = layeredFog();
        fine.startDistance = 3.0f;
        fine.maxOpacity = 0.9f;
        fine.sunColor = Vec3(4.0f, 2.0f, 1.0f);
        fine.towardsSun = glm::normalize(Vec3(1.0f, 2.0f, 3.0f));
        fine.sunExponent = 16.0f;
        const FogSettings same = sanitized(fine);
        CHECK(same.color == fine.color);
        CHECK(same.density == fine.density);
        CHECK(same.height == fine.height);
        CHECK(same.heightFalloff == fine.heightFalloff);
        CHECK(same.startDistance == fine.startDistance);
        CHECK(same.maxOpacity == fine.maxOpacity);
        CHECK(same.sunColor == fine.sunColor);
        CHECK_NEAR(same.towardsSun, fine.towardsSun, 1.0e-6f);
        CHECK(same.sunExponent == fine.sunExponent);
    }

    TEST_CASE("however far, high or deep: what comes of a fog is a number") {
        // The densest fog, thinning out as fast as a fog can, and one that hardly does.
        for (const float falloff : {100.0f, 1.0e-5f, 0.0f}) {
            FogSettings fog;
            fog.color = Vec3(0.5f);
            fog.density = 100.0f;
            fog.heightFalloff = falloff;
            fog.sunColor = Vec3(65504.0f);
            fog.sunExponent = 256.0f;
            const FogSettings used = sanitized(fog);
            for (const float fromHeight : {-1.0e6f, -3.0f, 0.0f, 1.0e-6f, 40.0f, 1.0e6f}) {
                for (const float toHeight : {-1.0e6f, 0.0f, 2.0e-6f, 40.0f, 1.0e6f}) {
                    for (const float across : {0.0f, 1.0e-6f, 50.0f, 1.0e6f}) {
                        const Vec3 from(0.0f, fromHeight, 0.0f);
                        const Vec3 to(across, toHeight, 0.0f);
                        CAPTURE(falloff);
                        CAPTURE(from);
                        CAPTURE(to);
                        const float depth = fogOpticalDepth(used, from, to);
                        CHECK_FALSE(std::isnan(depth));
                        CHECK(depth >= 0.0f);
                        const float amount = fogAmount(used, from, to);
                        CHECK(amount >= 0.0f);
                        CHECK(amount <= 1.0f);
                        CHECK(isFinite(applyFog(used, Vec3(65504.0f), from, to)));

                        const Vec3 direction = normalizeOr(to - from, kWorldUp);
                        const float depthTowards = fogOpticalDepthTowards(used, from, direction);
                        CHECK_FALSE(std::isnan(depthTowards));
                        CHECK(depthTowards >= 0.0f);
                        const float amountTowards = fogAmountTowards(used, from, direction);
                        CHECK(amountTowards >= 0.0f);
                        CHECK(amountTowards <= 1.0f);
                        CHECK(isFinite(applyFogTowards(used, Vec3(65504.0f), from, direction)));
                    }
                }
            }
        }

        // A way that is no way is no fog.
        const FogSettings fog = layeredFog();
        CHECK(fogOpticalDepth(fog, Vec3(0.0f), Vec3(kNotANumber, 0.0f, 0.0f)) == 0.0f);
        CHECK(fogAmount(fog, Vec3(0.0f), Vec3(kNotANumber, 0.0f, 0.0f)) == 0.0f);
    }
}
