#include "cordite/renderer/EnvironmentProbes.h"

#include "cordite/math/Transform.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <array>
#include <cmath>
#include <limits>

using namespace cordite;

namespace {

constexpr float kNotANumber = std::numeric_limits<float>::quiet_NaN();

/// The probe of a box `size` metres a side about `position`, turned by `rotation`.
GpuEnvironmentProbe boxProbe(const Vec3& position, const Vec3& size, float blendDistance,
                             const Quat& rotation = kIdentityQuat) {
    const Transform transform{.position = position, .rotation = rotation, .scale = size};
    return gpuProbeOf(transform.toMatrix(), {.blendDistance = blendDistance}, 3);
}

} // namespace

TEST_SUITE("EnvironmentProbes") {
    TEST_CASE("a probe gives all of the light inside its box, and fades out evenly towards its faces") {
        const GpuEnvironmentProbe probe = boxProbe(Vec3(1.0f, 2.0f, 3.0f), Vec3(4.0f, 2.0f, 6.0f), 0.5f);
        CHECK(probe.slot == 3);
        CHECK_NEAR(probe.boxSize, Vec3(4.0f, 2.0f, 6.0f), 1.0e-5f);
        CHECK_NEAR(probe.capturePosition, Vec3(1.0f, 2.0f, 3.0f), 1.0e-6f);
        CHECK(probeWeight(probe, Vec3(1.0f, 2.0f, 3.0f)) == 1.0f);
        // A metre from the box's middle along Y is the face: two metres high.
        CHECK(probeWeight(probe, Vec3(1.0f, 2.75f, 3.0f)) == doctest::Approx(0.5).epsilon(1.0e-5));
        CHECK(probeWeight(probe, Vec3(1.0f, 2.9f, 3.0f)) == doctest::Approx(0.2).epsilon(1.0e-5));
        CHECK(probeWeight(probe, Vec3(1.0f, 3.0f, 3.0f)) == 0.0f);
        CHECK(probeWeight(probe, Vec3(1.0f, 3.1f, 3.0f)) == 0.0f);
        // The nearest face decides: 0.25 from +X, further from the others.
        CHECK(probeWeight(probe, Vec3(2.75f, 2.0f, 3.0f)) == doctest::Approx(0.5).epsilon(1.0e-5));
        CHECK(probeWeight(probe, Vec3(-1.1f, 2.0f, 3.0f)) == 0.0f);

        // Without a blend, all of it up to the faces.
        const GpuEnvironmentProbe sharp = boxProbe(Vec3(0.0f), Vec3(2.0f), 0.0f);
        CHECK(probeWeight(sharp, Vec3(0.999f, 0.0f, 0.0f)) == 1.0f);
        CHECK(probeWeight(sharp, Vec3(1.001f, 0.0f, 0.0f)) == 0.0f);
    }

    TEST_CASE("a turned box is a box in its own axes") {
        const Quat turn = glm::angleAxis(kHalfPi, Vec3(0.0f, 1.0f, 0.0f));
        // Four metres along its own X, which the turn lays along the world's -Z.
        const GpuEnvironmentProbe probe = boxProbe(Vec3(0.0f), Vec3(4.0f, 2.0f, 2.0f), 0.0f, turn);
        CHECK(probeWeight(probe, Vec3(0.0f, 0.0f, -1.9f)) == 1.0f);
        CHECK(probeWeight(probe, Vec3(1.5f, 0.0f, 0.0f)) == 0.0f);
    }

    TEST_CASE("a box of no size, and numbers that are none, light nothing and stay numbers") {
        const GpuEnvironmentProbe flat = boxProbe(Vec3(0.0f), Vec3(2.0f, 0.0f, 2.0f), 0.5f);
        CHECK(probeWeight(flat, Vec3(0.0f)) == 0.0f);
        CHECK(probeWeight(flat, Vec3(5.0f)) == 0.0f);
        // What the GPU is given is made of numbers, too: a GPU's min() may pass over what is
        // none, and light with it.
        CHECK(isFinite(flat.worldToBox));

        Mat4 broken(1.0f);
        broken[3] = Vec4(kNotANumber, 0.0f, 0.0f, 1.0f);
        const GpuEnvironmentProbe none =
            gpuProbeOf(broken, {.blendDistance = kNotANumber, .intensity = kNotANumber}, 0);
        CHECK(probeWeight(none, Vec3(0.0f)) == 0.0f);
        CHECK(isFinite(none.worldToBox));
        CHECK(none.blendDistance == 0.0f);
        CHECK(none.intensity == 0.0f);
        CHECK(none.capturePosition == Vec3(0.0f));
        CHECK(probeWeight(boxProbe(Vec3(0.0f), Vec3(2.0f), 0.5f), Vec3(kNotANumber)) == 0.0f);

        // What is out of range is held to it.
        const GpuEnvironmentProbe wild = gpuProbeOf(Mat4(1.0f), {.blendDistance = -3.0f, .intensity = 1.0e9f}, 0);
        CHECK(wild.blendDistance == 0.0f);
        CHECK(wild.intensity == 100.0f);
    }

    TEST_CASE("a reflection is looked up where it meets the box, seen from where the picture was taken") {
        // A box four metres a side about (1, 0, 0), its picture taken in its middle.
        const GpuEnvironmentProbe probe = boxProbe(Vec3(1.0f, 0.0f, 0.0f), Vec3(4.0f), 0.0f);
        REQUIRE((probe.flags & kProbeBoxProjection) != 0);
        // From a metre left of the middle, straight up: the ceiling a metre left of where the
        // picture sees straight up.
        const Vec3 up = probeReflectionDirection(probe, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f));
        CHECK_NEAR(up, glm::normalize(Vec3(-1.0f, 2.0f, 0.0f)), 1.0e-5f);
        // Towards the corner it reaches first: here the +Z face, half a metre along.
        const Vec3 slant = glm::normalize(Vec3(1.0f, 0.0f, 3.0f));
        const Vec3 position(0.0f, 0.0f, 1.5f);
        const float along = 0.5f / slant.z;
        CHECK_NEAR(probeReflectionDirection(probe, position, slant),
                   glm::normalize(position + slant * along - Vec3(1.0f, 0.0f, 0.0f)), 1.0e-5f);
        // From where the picture was taken, nothing changes.
        CHECK_NEAR(probeReflectionDirection(probe, Vec3(1.0f, 0.0f, 0.0f), slant), slant, 1.0e-5f);
        // Along an axis, exactly.
        CHECK_NEAR(probeReflectionDirection(probe, Vec3(1.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)),
                   glm::normalize(Vec3(2.0f, 1.0f, 0.0f)), 1.0e-5f);

        // Without box projection, as if everything were far away.
        const GpuEnvironmentProbe far = gpuProbeOf(Mat4(1.0f), {.boxProjection = false}, 0);
        CHECK((far.flags & kProbeBoxProjection) == 0);
        CHECK(probeReflectionDirection(far, Vec3(0.3f, 0.2f, 0.1f), slant) == slant);
        // And a probe whose reflections could not be made says so.
        CHECK((gpuProbeOf(Mat4(1.0f), {}, 0, false).flags & kProbeReflects) == 0);
        CHECK((gpuProbeOf(Mat4(1.0f), {}, 0, true).flags & kProbeReflects) != 0);
    }

    TEST_CASE("the faces of a cube map are the six axes, each seen the way a cube map has it") {
        constexpr std::array<Vec3, kCubeFaceCount> kAxes = {Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f),
                                                            Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                                            Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f)};
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            CAPTURE(face);
            CHECK_NEAR(cubeFaceDirection(face, Vec2(0.5f)), kAxes[face], 1.0e-6f);
            // The corners are the corners of the cube.
            const Vec3 corner = cubeFaceDirection(face, Vec2(0.0f));
            CHECK(std::abs(glm::dot(corner, kAxes[face]) - 1.0f / std::sqrt(3.0f)) < 1.0e-5f);
        }
        // The top left of +X is towards +Y and +Z; of -Z towards +Y and +X... as a GPU reads
        // them: the face of +X runs from +Z on its left to -Z on its right.
        CHECK(cubeFaceDirection(0, Vec2(0.0f, 0.5f)).z > 0.0f);
        CHECK(cubeFaceDirection(0, Vec2(0.5f, 0.0f)).y > 0.0f);
        CHECK(cubeFaceDirection(5, Vec2(0.0f, 0.5f)).x > 0.0f);
        CHECK(cubeFaceDirection(2, Vec2(0.5f, 0.0f)).z < 0.0f);
    }

    TEST_CASE("the camera of a face looks along it, a quarter turn each way, and sees it turned left to right") {
        const Vec3 position(1.0f, 2.0f, 3.0f);
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            CAPTURE(face);
            const CameraView view = probeFaceView(position, face);
            CHECK_NEAR(view.position, position, 1.0e-6f);
            CHECK_NEAR(view.forward, cubeFaceDirection(face, Vec2(0.5f)), 1.0e-5f);
            // The left edge of the camera's picture is the right edge of the face.
            const Vec2 size(64.0f);
            const Vec3 left = screenRay(view, Vec2(0.0f, 32.0f), size).direction;
            CHECK_NEAR(left, cubeFaceDirection(face, Vec2(1.0f, 0.5f)), 1.0e-4f);
            const Vec3 top = screenRay(view, Vec2(32.0f, 0.0f), size).direction;
            CHECK_NEAR(top, cubeFaceDirection(face, Vec2(0.5f, 0.0f)), 1.0e-4f);
        }
    }
}
