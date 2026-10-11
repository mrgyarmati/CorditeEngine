// Small numeric helpers shared by the shaders.

#pragma once

// `value` at unit length, or `fallback` when it has no length to speak of. Plain
// normalize() of a zero vector is "not a number", and one such pixel spreads through
// every pass that blurs the image.
float3 normalizeOr(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-12 ? value * rsqrt(lengthSquared) : fallback;
}
