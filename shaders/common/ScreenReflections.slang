// Reflections from the screen (renderer/ScreenReflections.h): what a shiny surface reflects,
// found in the picture that has been drawn. The arithmetic of ScreenReflections.cpp,
// written down a second time - change both - and the constants of the two passes that
// draw them (passes/ScreenReflectionTrace.slang, passes/ScreenReflectionComposite.slang).

#pragma once

#include "common/GBuffer.slang"
#include "common/View.slang"

// Matches ScreenReflectionConstants in renderer/ScreenReflectionPass.cpp; beside the
// G-buffer, in register space 1 (renderer/GBufferBindings.h).
cbuffer ScreenReflectionConstants : register(b0, space1)
{
    // ScreenReflectionSettings, within their ranges.
    float reflectionIntensity;
    float reflectionMaxRoughness;
    float reflectionMaxDistance;
    float reflectionThickness;
    uint reflectionDivisor;
    uint reflectionSteps;
    float reflectionPadding0;
    float reflectionPadding1;
};

// Matches kScreenEdgeFade.
static const float kScreenEdgeFade = 0.1;

// How many times the way between the last step in front of what was drawn and the first
// behind it is halved.
static const uint kScreenReflectionHalvings = 4;

// screenEdgeFade(): how much a reflection found at `uv` is trusted for where it is.
float screenEdgeFade(float2 uv)
{
    // What is no number is nowhere in the picture - asked first, since a GPU's min() may
    // pass over one.
    if (!all(isfinite(uv)))
    {
        return 0.0;
    }
    const float toBorder = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    if (toBorder <= 0.0)
    {
        return 0.0;
    }
    return min(toBorder / kScreenEdgeFade, 1.0);
}

// reflectionDistanceFade(): how much a reflection that went `travelled` metres is trusted.
float reflectionDistanceFade(float travelled, float maxDistance)
{
    if (!(maxDistance > 0.0) || !isfinite(maxDistance) || !isfinite(travelled))
    {
        return 0.0;
    }
    return saturate((maxDistance - travelled) / (0.25 * maxDistance));
}

// reflectionRoughnessFade(): how much a surface of `roughness` takes from the screen.
float reflectionRoughnessFade(float roughness, float maxRoughness)
{
    if (!(maxRoughness > 0.0) || !isfinite(maxRoughness) || !isfinite(roughness))
    {
        return 0.0;
    }
    return saturate((maxRoughness - roughness) / (0.5 * maxRoughness));
}

// Where the point whose clip coordinates are `clip` is in the picture: x and y from (0, 0)
// at the top left to (1, 1), z its reversed depth.
float3 pictureOf(float4 clip)
{
    const float3 ndc = clip.xyz / clip.w;
    return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, ndc.z);
}

// How far in front of the camera a point is, along the way it looks, in metres.
float distanceAlongView(float3 position)
{
    return dot(position - cameraPosition, cameraForward);
}

// How wide a pixel of the picture is where `position` is, in metres.
float pixelWidthAt(float3 position)
{
    const float3 at = pictureOf(mul(viewProjection, float4(position, 1.0)));
    const float3 beside =
        worldPositionFromDepth(at.xy + float2(inverseTargetSize.x, 0.0), at.z, inverseViewProjection);
    return length(beside - position);
}

// The largest share of the way across the picture from `from` to `to` that is still in
// the picture: 1 when all of it is.
float shareInPicture(float2 from, float2 to)
{
    float share = 1.0;
    [unroll]
    for (int axis = 0; axis < 2; ++axis)
    {
        const float along = to[axis] - from[axis];
        if (along > 0.0)
        {
            share = min(share, (1.0 - from[axis]) / along);
        }
        else if (along < 0.0)
        {
            share = min(share, -from[axis] / along);
        }
    }
    return max(share, 0.0);
}

// The reversed depth of what was drawn at `uv`, as its pixel holds it: 0 where nothing
// was drawn, and outside the picture, of which nothing is known.
float drawnDepthAt(Texture2D<float4> depth, float2 uv)
{
    const int2 pixel = int2(floor(uv * targetSize));
    if (any(pixel < int2(0, 0)) || any(pixel >= int2(targetSize)))
    {
        return 0.0;
    }
    return depth.Load(int3(pixel, 0)).r;
}

// Whether the point `at` of the way - x and y in the picture, z its reversed depth - lies
// behind what was drawn there, whose depth is `drawn`.
bool liesBehind(float3 at, float drawn)
{
    return drawn > 0.0 && drawn > at.z;
}

// ScreenReflectionHit.
struct ScreenReflectionHit
{
    bool found;
    float2 uv;
    float travelled;
    float confidence;
};

// traceScreenReflection(): follows the reflection that leaves the surface at `origin`,
// whose unit normal is `normal`, along the unit vector `direction`, through `depth`.
ScreenReflectionHit traceScreenReflection(Texture2D<float4> depth, float maxDistance, float thickness, uint steps,
                                          float3 origin, float3 normal, float3 direction)
{
    ScreenReflectionHit hit;
    hit.found = false;
    hit.uv = float2(0.0, 0.0);
    hit.travelled = 0.0;
    hit.confidence = 0.0;
    if (steps == 0 || !(maxDistance > 0.0) || !isfinite(maxDistance))
    {
        return hit;
    }
    // A pixel's width above the surface, which the depth buffer holds as it is at the
    // middles of its pixels.
    const float3 setOut = origin + normal * pixelWidthAt(origin);
    // To maxDistance, or to the near plane: in front of it z is at most w.
    const float4 clipStart = mul(viewProjection, float4(setOut, 1.0));
    float4 clipEnd = mul(viewProjection, float4(setOut + direction * maxDistance, 1.0));
    const float marginStart = clipStart.w - clipStart.z;
    const float marginEnd = clipEnd.w - clipEnd.z;
    if (!(marginStart > 0.0))
    {
        return hit;
    }
    if (marginEnd < 0.0)
    {
        clipEnd = lerp(clipStart, clipEnd, marginStart / (marginStart - marginEnd));
    }
    const float3 start = pictureOf(clipStart);
    const float3 end = pictureOf(clipEnd);

    // Across the picture x, y and the depth change evenly: only the part in it is walked.
    const float inPicture = shareInPicture(start.xy, end.xy);
    if (!(length((end.xy - start.xy) * targetSize) * inPicture >= 1.0))
    {
        return hit;
    }
    float before = 0.0;
    for (uint step = 1; step <= steps; ++step)
    {
        const float share = inPicture * float(step) / float(steps);
        const float3 at = lerp(start, end, share);
        if (liesBehind(at, drawnDepthAt(depth, at.xy)))
        {
            // The way back to the step before is halved, keeping the end that lies behind.
            float inFront = before;
            float behindEnd = share;
            for (uint halving = 0; halving < kScreenReflectionHalvings; ++halving)
            {
                const float middle = 0.5 * (inFront + behindEnd);
                const float3 point = lerp(start, end, middle);
                if (liesBehind(point, drawnDepthAt(depth, point.xy)))
                {
                    behindEnd = middle;
                }
                else
                {
                    inFront = middle;
                }
            }
            const float3 met = lerp(start, end, behindEnd);
            const float3 reflected = worldPositionFromDepth(met.xy, met.z, inverseViewProjection);
            const float3 drawn =
                worldPositionFromDepth(met.xy, drawnDepthAt(depth, met.xy), inverseViewProjection);
            if (distanceAlongView(reflected) - distanceAlongView(drawn) <= thickness)
            {
                hit.found = true;
                hit.uv = met.xy;
                hit.travelled = length(reflected - origin);
                hit.confidence = screenEdgeFade(met.xy) * reflectionDistanceFade(hit.travelled, maxDistance);
                return hit;
            }
        }
        before = share;
    }
    return hit;
}
