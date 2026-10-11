#pragma once

/// @file
/// Approximate comparisons and printing of math types for doctest.

#include "cordite/math/Math.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <string>

namespace cordite::test {

inline std::string describe(const Vec2& value) {
    return fmt::format("({}, {})", value.x, value.y);
}

inline std::string describe(const Vec3& value) {
    return fmt::format("({}, {}, {})", value.x, value.y, value.z);
}

inline std::string describe(const Vec4& value) {
    return fmt::format("({}, {}, {}, {})", value.x, value.y, value.z, value.w);
}

inline std::string describe(const Quat& value) {
    return fmt::format("(x={}, y={}, z={}, w={})", value.x, value.y, value.z, value.w);
}

inline bool nearlyEqual(const Vec2& a, const Vec2& b, float tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}

inline bool nearlyEqual(const Vec4& a, const Vec4& b, float tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance &&
           std::abs(a.w - b.w) <= tolerance;
}

inline bool nearlyEqual(const Vec3& a, const Vec3& b, float tolerance) {
    return cordite::nearlyEqual(a, b, tolerance);
}

/// Two quaternions describe the same rotation when they are equal or exact opposites.
inline bool sameRotation(const Quat& a, const Quat& b, float tolerance) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= tolerance;
}

inline bool nearlyEqual(const Mat4& a, const Mat4& b, float tolerance) {
    for (int column = 0; column < 4; ++column) {
        if (!nearlyEqual(a[column], b[column], tolerance)) {
            return false;
        }
    }
    return true;
}

} // namespace cordite::test

/// Checks that two vectors are equal within a tolerance, printing both on failure.
#define CHECK_NEAR(actual, expected, tolerance)                                                                        \
    CHECK_MESSAGE(::cordite::test::nearlyEqual((actual), (expected), (tolerance)),                                     \
                  "actual " << ::cordite::test::describe(actual) << ", expected "                                      \
                            << ::cordite::test::describe(expected))
