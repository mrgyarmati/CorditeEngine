// Fog: air that is not clear, and what it does to light on its way to the viewer.
//
// The further away something is, the less of its light arrives, and the more of the
// fog's own color is seen in its place. The fog is as dense as `fogDensity` says up to
// `fogHeight` and thins out above, with every metre by the same factor; how much of it
// lies on a line of sight has a closed form. What that is, and why, is in
// engine/cordite/renderer/Fog.h, which has the same arithmetic a second time: tests hold
// what the passes draw against it.
//
// The numbers are the view's (common/View.slang). `fogDensity` is zero when it has no
// fog, and every function here then leaves the light as it is.

#pragma once

#include "common/Math.slang"
#include "common/View.slang"

// An optical depth that nothing shows through: that of a line of sight which never
// comes out of the fog.
static const float kFogOpaque = 1.0e30;

// The mean of e to the minus x for x from nothing to `span`: (1 - e^-span) / span. Where
// the span is small the subtraction would leave little of it, so the first terms of
// its series are taken instead.
float fogMeanDecayOver(float span)
{
    return span < 0.05 ? 1.0 - span * (0.5 - span / 6.0) : (1.0 - exp(-span)) / span;
}

// The mean density of the fog on a straight way from one height to another.
float fogMeanDensityBetween(float startHeight, float endHeight)
{
    // From here on heights are above where the fog is densest.
    const float start = startHeight - fogHeight;
    const float end = endHeight - fogHeight;
    const float lowest = min(start, end);
    const float highest = max(start, end);

    // The part of the way that is below that height, where the fog is the same
    // everywhere, ...
    float below = 0.0;
    if (lowest < 0.0)
    {
        below = highest > 0.0 ? -lowest / (highest - lowest) : 1.0;
    }
    // ... and the mean of what it thins out to on the rest.
    const float lowestAbove = max(lowest, 0.0);
    const float highestAbove = max(highest, 0.0);
    const float above =
        exp(-fogHeightFalloff * lowestAbove) * fogMeanDecayOver(fogHeightFalloff * (highestAbove - lowestAbove));
    return fogDensity * (below + (1.0 - below) * above);
}

// How much fog lies on the straight way from `from` to `to`: e to the minus this much
// of the light gets through.
float fogOpticalDepth(float3 from, float3 to)
{
    const float3 way = to - from;
    const float distance = length(way);
    // Also when the distance is no number.
    if (!(distance > fogStartDistance))
    {
        return 0.0;
    }
    const float startHeight = from.y + way.y * (fogStartDistance / distance);
    return fogMeanDensityBetween(startHeight, to.y) * (distance - fogStartDistance);
}

// The same along a line of sight without end: from `from` on, along the unit vector
// `direction`.
float fogOpticalDepthTowards(float3 from, float3 direction)
{
    // By how many e-ths the density falls with every metre along the line. Where it
    // does not fall - the line is level or goes down, or the fog does not thin out -
    // the line never comes out of the fog.
    const float thinning = fogHeightFalloff * direction.y;
    if (!(thinning > 1.0e-6))
    {
        return kFogOpaque;
    }
    // What is below the height the fog is densest up to is crossed first, at its full
    // density; then the fog thins out for good.
    const float start = from.y + direction.y * fogStartDistance - fogHeight;
    const float below = max(-start, 0.0) / direction.y;
    const float above = exp(-fogHeightFalloff * max(start, 0.0)) / thinning;
    return min(fogDensity * (below + above), kFogOpaque);
}

// What part of what is behind a fog of the given optical depth is fog.
float fogAmountOf(float opticalDepth)
{
    return min(1.0 - exp(-opticalDepth), fogMaxOpacity);
}

// What part of what is seen of `to` from `from` is fog: 0 none of it, 1 all.
float fogAmount(float3 from, float3 to)
{
    return fogDensity > 0.0 ? fogAmountOf(fogOpticalDepth(from, to)) : 0.0;
}

// The same for what is infinitely far away along the unit vector `direction`: the sky.
float fogAmountTowards(float3 from, float3 direction)
{
    return fogDensity > 0.0 ? fogAmountOf(fogOpticalDepthTowards(from, direction)) : 0.0;
}

// The color of the fog as seen along the unit vector `direction`, away from the viewer:
// its own, and towards the sun what it throws on of the sun's light.
float3 fogColorAlong(float3 direction)
{
    // The exponent is at least one, so nothing to the power of it is nothing.
    return fogColor + fogSunColor * pow(max(dot(direction, fogTowardsSun), 0.0), fogSunExponent);
}

// The light that arrives at `from` of the `light` that leaves `to` towards it: what the
// fog lets through, and the fog's own in place of the rest. `light` must be a number.
float3 seenThroughFog(float3 light, float3 from, float3 to)
{
    if (fogDensity <= 0.0)
    {
        return light;
    }
    const float3 direction = normalizeOr(to - from, cameraForward);
    return lerp(light, fogColorAlong(direction), fogAmount(from, to));
}

// The same for `light` that comes from infinitely far away, against the unit vector
// `direction`: that of the sky.
float3 seenThroughFogTowards(float3 light, float3 from, float3 direction)
{
    if (fogDensity <= 0.0)
    {
        return light;
    }
    return lerp(light, fogColorAlong(direction), fogAmountTowards(from, direction));
}
