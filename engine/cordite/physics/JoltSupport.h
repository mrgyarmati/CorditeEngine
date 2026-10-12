#pragma once

/// @file
/// For the sources of the physics module alone: Jolt, and the engine's values as Jolt's.
///
/// Every header of Jolt needs Jolt/Jolt.h before it. A source that includes this file
/// among the engine's headers has it there, whatever order a formatter gives the
/// headers of Jolt that follow:
///
/// @code
/// #include "cordite/physics/PhysicsWorld.h"
///
/// #include "cordite/core/Log.h"
/// #include "cordite/physics/JoltSupport.h"
///
/// #include <Jolt/Physics/Body/BodyInterface.h>
///
/// bodies.SetPosition(id, toJolt(transform.position), JPH::EActivation::Activate);
/// const Vec3 velocity = fromJolt(bodies.GetLinearVelocity(id));
/// @endcode
///
/// Nothing outside the module can include it: Jolt is linked privately, so that no
/// other module comes to depend on the library behind the physics.

#include "cordite/math/Math.h"

#include <Jolt/Jolt.h>

#include <cmath>

namespace cordite {

[[nodiscard]] inline JPH::Vec3 toJolt(const Vec3& value) {
    return {value.x, value.y, value.z};
}

[[nodiscard]] inline JPH::Quat toJolt(const Quat& value) {
    return {value.x, value.y, value.z, value.w};
}

[[nodiscard]] inline Vec3 fromJolt(JPH::Vec3Arg value) {
    return {value.GetX(), value.GetY(), value.GetZ()};
}

[[nodiscard]] inline Quat fromJolt(JPH::QuatArg value) {
    // glm's constructor takes w first.
    return {value.GetW(), value.GetX(), value.GetY(), value.GetZ()};
}

/// Whether a value is made of numbers, beside isFinite(Vec3) of Math.h: what comes from a
/// file, a script or a component is checked before Jolt gets it, which takes numbers for
/// granted.
[[nodiscard]] inline bool isFinite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] inline bool isFinite(const Quat& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

/// Whether a quaternion can be brought to unit length, to be a rotation.
[[nodiscard]] inline bool isRotation(const Quat& value) noexcept {
    return isFinite(value) && glm::length(value) > kEpsilon;
}

} // namespace cordite
