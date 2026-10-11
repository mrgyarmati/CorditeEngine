#pragma once

/// @file
/// Directions and the places they have in an image of everything around.
///
/// Such an image is "equirectangular": as wide as the horizon is round and as high as
/// the way from straight up to straight down, twice as wide as high, like a map of the
/// world. It is what HDR images of skies come as. The middle of the image is what is
/// seen looking along -Z; a quarter of the width to its right is +X; the top row is
/// straight up (+Y), the bottom row straight down.
///
/// @code
/// const Vec2 where = equirectUv(directionToTheSun);           // (0, 0) is the top left of the image
/// const Vec3 shown = equirectDirection(Vec2(0.5f, 0.5f));     // (0, 0, -1): straight ahead
///
/// // An environment turned by 30 degrees shows, in a direction of the world, what its
/// // image holds for that direction turned back.
/// const Vec2 inImage = equirectUv(turnedAboutVertical(direction, -toRadians(30.0f)));
/// @endcode
///
/// The mapping is written down a second time in shaders/common/Environment.slang; tests
/// that render hold the two against each other.

#include "cordite/math/Math.h"

namespace cordite {

/// Where a direction is in an equirectangular image: (0, 0) at the top left, (1, 1) at
/// the bottom right. `direction` need not have unit length; one without any, or with a
/// number in it that is none, is taken for straight ahead.
[[nodiscard]] Vec2 equirectUv(const Vec3& direction);

/// The unit direction a place in an equirectangular image shows: the opposite of
/// equirectUv().
[[nodiscard]] Vec3 equirectDirection(const Vec2& uv);

/// `direction` turned about the vertical axis by `radians`, counter-clockwise seen from
/// above: a quarter turn brings what is straight ahead (-Z) to the left (-X).
[[nodiscard]] Vec3 turnedAboutVertical(const Vec3& direction, float radians);

} // namespace cordite
