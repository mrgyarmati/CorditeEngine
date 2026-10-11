#pragma once

/// @file
/// JSON conversion for math types. Vectors and quaternions are arrays of numbers;
/// quaternions are stored as [x, y, z, w].
///
/// @code
/// Json node;
/// node["position"] = toJson(transform.position);
///
/// Vec3 position;
/// CORDITE_TRY(fromJson(node["position"], position));
/// @endcode

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"

namespace cordite {

[[nodiscard]] Json toJson(const Vec2& value);
[[nodiscard]] Json toJson(const Vec3& value);
[[nodiscard]] Json toJson(const Vec4& value);
[[nodiscard]] Json toJson(const Quat& value);
[[nodiscard]] Json toJson(const Transform& value);
[[nodiscard]] Json toJson(const Aabb& value);

/// Each fromJson leaves `out` untouched and returns a ParseError when the JSON does not
/// have the expected shape or contains non-finite numbers.
Status fromJson(const Json& json, Vec2& out);
Status fromJson(const Json& json, Vec3& out);
Status fromJson(const Json& json, Vec4& out);

/// The quaternion is normalized unless it already has unit length, in which case it is
/// read back exactly as written. A zero quaternion is rejected.
Status fromJson(const Json& json, Quat& out);

/// Reads {"position": [...], "rotation": [...], "scale": [...]}; missing members keep
/// their identity defaults.
Status fromJson(const Json& json, Transform& out);
Status fromJson(const Json& json, Aabb& out);

} // namespace cordite
