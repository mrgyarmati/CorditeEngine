#pragma once

/// @file
/// Makes vectors and quaternions usable as reflected properties (see
/// cordite/core/Reflection.h). Include this wherever a TypeBuilder describes a struct
/// that has math members.
///
/// @code
/// TypeBuilder<Transform>("Transform")
///     .property("position", &Transform::position, {.unit = "m"})
///     .property("rotation", &Transform::rotation)
///     .property("scale", &Transform::scale);
/// @endcode
///
/// Vectors are JSON arrays of numbers; quaternions are stored as [x, y, z, w] and
/// normalized when read.

#include "cordite/core/Reflection.h"
#include "cordite/math/Math.h"

namespace cordite {

template <>
struct PropertyCodec<Vec2> {
    static constexpr PropertyType kType = PropertyType::Vec2;
    static Json toJson(const Vec2& value);
    static Status fromJson(const Json& json, Vec2& out);
};

template <>
struct PropertyCodec<Vec3> {
    static constexpr PropertyType kType = PropertyType::Vec3;
    static Json toJson(const Vec3& value);
    static Status fromJson(const Json& json, Vec3& out);
};

template <>
struct PropertyCodec<Vec4> {
    static constexpr PropertyType kType = PropertyType::Vec4;
    static Json toJson(const Vec4& value);
    static Status fromJson(const Json& json, Vec4& out);
};

template <>
struct PropertyCodec<Quat> {
    static constexpr PropertyType kType = PropertyType::Quat;
    static Json toJson(const Quat& value);
    static Status fromJson(const Json& json, Quat& out);
};

} // namespace cordite
