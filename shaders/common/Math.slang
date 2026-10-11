// Small numeric helpers shared by the shaders.

#pragma once

static const float kPi = 3.14159265358979;

// `value` at unit length, or `fallback` when it has no length to speak of. Plain
// normalize() of a zero vector is "not a number", and one such pixel spreads through
// every pass that blurs the image.
float3 normalizeOr(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared) : fallback;
}

// Where a unit direction is in an image of everything around (an "equirectangular"
// one, twice as wide as high): (0, 0) at its top left. The middle of the image is
// straight ahead (-Z), +X a quarter of its width to the right, the top row straight up.
// Matches equirectUv() in engine/cordite/math/Equirectangular.cpp.
float2 equirectUv(float3 direction)
{
    // Straight up and straight down are every way round at once. Any place in their
    // row will do, but the arc tangent of nothing over nothing is no number on some
    // GPUs.
    const bool isLevel = direction.x != 0.0 || direction.z != 0.0;
    const float around = isLevel ? atan2(direction.x, -direction.z) : 0.0;
    const float down = acos(clamp(direction.y, -1.0, 1.0));
    return float2(around / (2.0 * kPi) + 0.5, down / kPi);
}
