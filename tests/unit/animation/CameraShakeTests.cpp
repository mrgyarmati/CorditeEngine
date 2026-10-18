#include "cordite/animation/CameraShake.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/math/Transform.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace cordite;
using cordite::test::unwrap;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// A shake whose limits are all different, so that a mix-up of two of them shows.
CameraShakeComponent shakeAt(float trauma, float phase) {
    return {.trauma = trauma,
            .decay = 1.0f,
            .maxOffset = Vec3(0.1f, 0.2f, 0.3f),
            .maxRotation = Vec3(4.0f, 5.0f, 6.0f),
            .frequency = 12.0f,
            .phase = phase};
}

/// The six numbers of an offset: how far along X, Y and Z, and how many degrees about
/// them, as cordite.euler() has them.
std::array<float, 6> waysOf(const Transform& offset) {
    const Vec3 angles = eulerDegreesFromQuat(offset.rotation);
    return {offset.position.x, offset.position.y, offset.position.z, angles.x, angles.y, angles.z};
}

/// The six ways of a shake at trauma 1, at `samples` phases a twentieth of a turn apart.
std::vector<std::array<float, 6>> sampleShake(size_t samples) {
    std::vector<std::array<float, 6>> ways;
    for (size_t sample = 0; sample < samples; ++sample) {
        ways.push_back(waysOf(cameraShakeOffset(shakeAt(1.0f, static_cast<float>(sample) * 0.05f))));
    }
    return ways;
}

/// How alike two of the six ways move over the samples: the correlation of the two, from -1 to 1.
double correlationOf(const std::vector<std::array<float, 6>>& ways, size_t first, size_t second) {
    double meanFirst = 0.0;
    double meanSecond = 0.0;
    for (const std::array<float, 6>& way : ways) {
        meanFirst += way.at(first);
        meanSecond += way.at(second);
    }
    meanFirst /= static_cast<double>(ways.size());
    meanSecond /= static_cast<double>(ways.size());
    double both = 0.0;
    double firstSquared = 0.0;
    double secondSquared = 0.0;
    for (const std::array<float, 6>& way : ways) {
        const double a = way.at(first) - meanFirst;
        const double b = way.at(second) - meanSecond;
        both += a * b;
        firstSquared += a * a;
        secondSquared += b * b;
    }
    return both / std::sqrt(firstSquared * secondSquared);
}

/// A world with one camera that shakes, and that camera.
struct ShakenWorld {
    ShakenWorld() {
        camera = world.createEntity("Camera");
        world.add<CameraShakeComponent>(camera) = {.trauma = 1.0f, .decay = 2.0f, .frequency = 10.0f};
    }

    [[nodiscard]] const CameraShakeComponent& shake() const { return *world.tryGet<CameraShakeComponent>(camera); }

    [[nodiscard]] CameraShakeComponent& edit() { return *world.tryGet<CameraShakeComponent>(camera); }

    World world;
    Entity camera;
};

} // namespace

TEST_SUITE("CameraShake") {
    TEST_CASE("a camera without trauma is not shaken, wherever in its shaking it is") {
        for (const float phase : {0.0f, 0.5f, 1.3f, 77.7f, 255.9f}) {
            CAPTURE(phase);
            CHECK(cameraShakeOffset(shakeAt(0.0f, phase)) == Transform{});
            // Less than none, and what is no number, is none.
            CHECK(cameraShakeOffset(shakeAt(-0.5f, phase)) == Transform{});
            CHECK(cameraShakeOffset(shakeAt(kNotANumber, phase)) == Transform{});
        }
        // A camera that shakes moves at all.
        CHECK_FALSE(cameraShakeOffset(shakeAt(1.0f, 0.5f)) == Transform{});
    }

    TEST_CASE("a shake moves and turns the camera no further than it may, and nearly as far") {
        const CameraShakeComponent limits = shakeAt(1.0f, 0.0f);
        const std::array<float, 6> furthest = {limits.maxOffset.x,   limits.maxOffset.y,   limits.maxOffset.z,
                                               limits.maxRotation.x, limits.maxRotation.y, limits.maxRotation.z};
        std::array<float, 6> reached{};
        // A whole period of the noise, a twentieth of a turn at a time.
        for (const std::array<float, 6>& ways : sampleShake(static_cast<size_t>(kCameraShakePeriod) * 20)) {
            for (size_t way = 0; way < ways.size(); ++way) {
                CAPTURE(way);
                // The angles come back from a rotation, to a part in a hundred thousand.
                CHECK(std::abs(ways.at(way)) <= furthest.at(way) * 1.00001f);
                reached.at(way) = std::max(reached.at(way), std::abs(ways.at(way)));
            }
        }
        for (size_t way = 0; way < reached.size(); ++way) {
            CAPTURE(way);
            CHECK(reached.at(way) > 0.7f * furthest.at(way));
        }
    }

    TEST_CASE("a camera is shaken by its trauma squared, and by no more than a trauma of one") {
        for (const float phase : {0.3f, 1.7f, 42.25f, 200.6f}) {
            CAPTURE(phase);
            const std::array<float, 6> full = waysOf(cameraShakeOffset(shakeAt(1.0f, phase)));
            const std::array<float, 6> half = waysOf(cameraShakeOffset(shakeAt(0.5f, phase)));
            for (size_t way = 0; way < full.size(); ++way) {
                CAPTURE(way);
                CHECK(std::abs(half.at(way) - 0.25f * full.at(way)) <= 1e-4f * std::abs(full.at(way)) + 1e-6f);
            }
            CHECK(cameraShakeOffset(shakeAt(3.0f, phase)) == cameraShakeOffset(shakeAt(1.0f, phase)));
            CHECK(cameraShakeOffset(shakeAt(1.0f, phase)).scale == Vec3(1.0f));
        }
    }

    TEST_CASE("the six ways a camera shakes are noises of their own, and none comes to rest at every turn") {
        const std::vector<std::array<float, 6>> ways = sampleShake(static_cast<size_t>(kCameraShakePeriod) * 20);
        for (size_t first = 0; first < 6; ++first) {
            for (size_t second = first + 1; second < 6; ++second) {
                CAPTURE(first);
                CAPTURE(second);
                CHECK(std::abs(correlationOf(ways, first, second)) < 0.3);
            }
        }
        // Perlin noise is nothing at every whole turn along a row of its lattice; the
        // camera would come back to where it is at every turn, all six ways at once.
        const std::array<float, 6> furthest = {0.1f, 0.2f, 0.3f, 4.0f, 5.0f, 6.0f};
        for (size_t way = 0; way < 6; ++way) {
            CAPTURE(way);
            float atWholeTurns = 0.0f;
            for (size_t turn = 0; turn < static_cast<size_t>(kCameraShakePeriod); ++turn) {
                atWholeTurns = std::max(atWholeTurns, std::abs(ways.at(turn * 20).at(way)));
            }
            CHECK(atWholeTurns > 0.5f * furthest.at(way));
        }
    }

    TEST_CASE("a shake is smooth: a little time moves the camera a little") {
        const std::array<float, 6> furthest = {0.1f, 0.2f, 0.3f, 4.0f, 5.0f, 6.0f};
        float largestStep = 0.0f;
        for (size_t sample = 0; sample < 2000; ++sample) {
            const float phase = static_cast<float>(sample) * 0.0371f;
            const std::array<float, 6> before = waysOf(cameraShakeOffset(shakeAt(1.0f, phase)));
            const std::array<float, 6> after = waysOf(cameraShakeOffset(shakeAt(1.0f, phase + 0.001f)));
            for (size_t way = 0; way < before.size(); ++way) {
                largestStep = std::max(largestStep, std::abs(after.at(way) - before.at(way)) / furthest.at(way));
            }
        }
        // A thousandth of a turn moves the camera by no more than a hundredth of the
        // furthest it may go: a hundred times what noise that jumped about would allow.
        CHECK(largestStep < 0.01f);
        CHECK(largestStep > 0.0f);
    }

    TEST_CASE("a shake is the same every time, and goes round at the end of its period without a jump") {
        CHECK(cameraShakeOffset(shakeAt(0.8f, 12.34f)) == cameraShakeOffset(shakeAt(0.8f, 12.34f)));

        const auto apart = [](float first, float second) {
            const std::array<float, 6> a = waysOf(cameraShakeOffset(shakeAt(1.0f, first)));
            const std::array<float, 6> b = waysOf(cameraShakeOffset(shakeAt(1.0f, second)));
            float largest = 0.0f;
            for (size_t way = 0; way < a.size(); ++way) {
                largest = std::max(largest, std::abs(a.at(way) - b.at(way)));
            }
            return largest;
        };
        // A period on, the noise is where it was - to the digits a float has left there.
        CHECK(apart(3.25f, 3.25f + kCameraShakePeriod) < 1e-3f);
        CHECK(apart(0.0f, kCameraShakePeriod) < 1e-5f);
        // And just before the end it is nearly where it is at the beginning.
        CHECK(apart(kCameraShakePeriod - 0.001f, 0.0f) < 0.01f);
    }

    TEST_CASE("what a shake holds that is no number counts as nothing") {
        CameraShakeComponent shake = shakeAt(1.0f, 7.5f);
        const Transform whole = cameraShakeOffset(shake);
        shake.maxOffset.y = kNotANumber;
        shake.maxRotation.z = -std::numeric_limits<float>::infinity();
        const Transform partly = cameraShakeOffset(shake);
        CHECK(partly.position.x == whole.position.x);
        CHECK(partly.position.y == 0.0f);
        CHECK(partly.position.z == whole.position.z);
        CHECK(eulerDegreesFromQuat(partly.rotation).z == doctest::Approx(0.0f).epsilon(1e-4));
        CHECK(std::isfinite(partly.rotation.w));

        // A phase that is no number is the beginning.
        CHECK(cameraShakeOffset(shakeAt(1.0f, kNotANumber)) == cameraShakeOffset(shakeAt(1.0f, 0.0f)));
    }

    TEST_CASE("time lets the trauma of a shake fall by its decay, and moves its phase on by its frequency") {
        ShakenWorld shaken;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == doctest::Approx(0.5f));
        CHECK(shaken.shake().phase == doctest::Approx(2.5f));
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 0.0f);
        CHECK(shaken.shake().phase == doctest::Approx(5.0f));
        // It falls to nothing and no further; the shaking goes on, ready for the next
        // time something happens.
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 0.0f);
        CHECK(shaken.shake().phase == doctest::Approx(7.5f));

        // A trauma beyond 1 is 1, and falls from there.
        shaken.edit().trauma = 4.0f;
        shakeCameras(shaken.world, 0.125f);
        CHECK(shaken.shake().trauma == doctest::Approx(0.75f));
    }

    TEST_CASE("the phase of a shake goes round within its period") {
        ShakenWorld shaken;
        shaken.edit().phase = kCameraShakePeriod - 1.0f;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().phase == doctest::Approx(1.5f));

        // From wherever it was set to, it comes back into the period.
        shaken.edit().phase = -0.75f;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().phase == doctest::Approx(1.75f));
        shaken.edit().phase = 10.0f * kCameraShakePeriod + 0.5f;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().phase == doctest::Approx(3.0f));
        CHECK(shaken.shake().phase < kCameraShakePeriod);
    }

    TEST_CASE("no time, less than none and what is no time let nothing pass") {
        ShakenWorld shaken;
        shaken.edit().phase = 3.0f;
        for (const float step : {0.0f, -1.0f, kNotANumber, std::numeric_limits<float>::infinity()}) {
            CAPTURE(step);
            shakeCameras(shaken.world, step);
            CHECK(shaken.shake().trauma == 1.0f);
            CHECK(shaken.shake().phase == 3.0f);
        }
    }

    TEST_CASE("a camera that is switched off, or hangs under something that is, does not shake on") {
        ShakenWorld shaken;
        shaken.world.setEnabled(shaken.camera, false);
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 1.0f);
        CHECK(shaken.shake().phase == 0.0f);

        shaken.world.setEnabled(shaken.camera, true);
        const Entity rig = shaken.world.createEntity("Rig");
        REQUIRE(shaken.world.setParent(shaken.camera, rig).has_value());
        shaken.world.setEnabled(rig, false);
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 1.0f);

        shaken.world.setEnabled(rig, true);
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == doctest::Approx(0.5f));
    }

    TEST_CASE("numbers of a shake that are no numbers let time pass as if they were nothing") {
        ShakenWorld shaken;
        CameraShakeComponent& shake = shaken.edit();
        shake.decay = kNotANumber;
        shake.frequency = -3.0f;
        shake.phase = kNotANumber;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 1.0f);
        CHECK(shaken.shake().phase == 0.0f);

        shaken.edit().trauma = kNotANumber;
        shakeCameras(shaken.world, 0.25f);
        CHECK(shaken.shake().trauma == 0.0f);
    }

    TEST_CASE("a camera shake is registered after the animator, and saved without what only a game has") {
        ComponentRegistry registry;
        REQUIRE(registerAnimationComponents(registry).has_value());
        REQUIRE(registry.components().size() == 2);
        CHECK(registry.components()[1]->type.name() == "CameraShake");
        const ComponentDescriptor* descriptor = registry.find("CameraShake");
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->traits.optional);
        CHECK_FALSE(descriptor->traits.description.empty());
        const TypeInfo& type = descriptor->type;
        REQUIRE(type.properties().size() == 6);
        for (const PropertyInfo& property : type.properties()) {
            CAPTURE(property.name());
            CHECK_FALSE(property.attributes().description.empty());
        }
        // That it shakes, and where in its shaking it is, are the game's.
        CHECK(type.findProperty("trauma")->attributes().transient);
        CHECK(type.findProperty("phase")->attributes().transient);
        CHECK(type.findProperty("maxOffset")->attributes().unit == "m");
        CHECK(type.findProperty("maxRotation")->attributes().unit == "deg");
        CHECK(type.findProperty("frequency")->attributes().unit == "Hz");
        CHECK(type.findProperty("decay")->attributes().unit == "1/s");

        // A new one shakes a little, and a scene says nothing of a shake that is going on.
        CameraShakeComponent shaking;
        shaking.trauma = 0.75f;
        shaking.phase = 9.0f;
        CHECK(type.toJson(&shaking) == unwrap(parseJson(R"({"decay": 1.0, "maxOffset": [0.05, 0.05, 0.05],
                                                            "maxRotation": [3.0, 3.0, 2.0], "frequency": 12.0})")));

        // What is out of range is refused, from a file and as an edit.
        CHECK_FALSE(type.parse(unwrap(parseJson(R"({"maxRotation": [3.0, 31.0, 2.0]})"))).has_value());
        CHECK_FALSE(type.parse(unwrap(parseJson(R"({"maxOffset": [0.0, -0.1, 0.0]})"))).has_value());
        CHECK_FALSE(type.parse(unwrap(parseJson(R"({"frequency": 101.0})"))).has_value());
        CHECK_FALSE(type.parseEdit(unwrap(parseJson(R"({"trauma": 1.5})"))).has_value());
        CHECK_FALSE(type.parseEdit(unwrap(parseJson(R"({"phase": -1.0})"))).has_value());
        // A game raises the trauma by an edit, as a script does.
        CHECK(type.parseEdit(unwrap(parseJson(R"({"trauma": 0.5})"))).has_value());
    }
}
