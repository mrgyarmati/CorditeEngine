// Ambient occlusion, first pass: how much of the light from all around reaches what a
// pixel shows, from the depth of what is drawn around it.
//
// Drawn for every pixel of the picture or for one in four (`divisor`), into a target of
// that size. Written down for each: the share of the light that gets through, 1 where
// nothing stands in the way, and how far the pixel is from the camera, which the
// second pass (AmbientOcclusionResolve.slang) needs to tell surfaces apart.
//
// The share is rough: a pixel looks in two or three directions only, each pixel in
// others than its neighbours. The second pass takes what the pixels around have found
// together.

#include "common/AmbientOcclusion.slang"
#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/View.slang"

Texture2D<float4> sceneDepth : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);

// What is further from a point than this part of the radius begins to count for less,
// and at the radius for nothing: no line shows where the looking ends.
static const float kFalloffStart = 0.4;

// A number from 0 to 1 for a pixel that is as unlike those of the pixels around it as
// can be (J. Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare", 2014).
float interleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// How far the steepest thing found from `pixel` along `step` (in pixels; the furthest
// look is `reach` of them away) rises above the surface at `position`, as an angle:
// nothing when all that is there is the surface.
float riseAlong(int2 pixel, float3 position, float3 normal, float3 toViewer, float2 step, float reach,
                float stepNoise)
{
    const int2 last = int2(targetSize) - 1;
    float rise = 0.0;
    for (uint index = 0; index < stepCount; ++index)
    {
        // More looks near the point than far from it, and never the point itself.
        const float part = (float(index) + stepNoise) / float(stepCount);
        const int2 there = pixel + int2(round(step * max(part * part * reach, 1.0)));
        if (any(there < int2(0, 0)) || any(there > last))
        {
            continue;
        }
        const float depth = sceneDepth.Load(int3(there, 0)).r;
        if (depth <= 0.0)
        {
            // Nothing was drawn there: nothing stands in the way.
            continue;
        }
        const float3 offset =
            worldPositionFromDepth((float2(there) + 0.5) * inverseTargetSize, depth, inverseViewProjection) - position;
        const float distance = length(offset);
        if (distance <= 1.0e-6)
        {
            continue;
        }
        const float weight = saturate((radius - distance) / (radius * (1.0 - kFalloffStart)));
        rise = max(rise, riseAboveSurface(offset / distance, normal, toViewer) * weight);
    }
    // What barely rises above the surface is the surface.
    return max(rise - bias, 0.0);
}

[shader("pixel")]
float2 ambientOcclusionPixel(FullscreenVaryings input) : SV_Target0
{
    // The pixel of the picture this texel stands for: the first of its block.
    const int2 texel = int2(input.position.xy);
    const int2 pixel = texel * int(divisor);
    const float depth = sceneDepth.Load(int3(pixel, 0)).r;
    // Depth is reversed: zero is as far as the view reaches, where nothing was drawn.
    if (depth <= 0.0)
    {
        return float2(1.0, kNowhere);
    }
    const float2 uv = (float2(pixel) + 0.5) * inverseTargetSize;
    const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
    const float3 normal = decodeNormal(gbufferNormal.Load(int3(pixel, 0)).xy);
    const float3 toViewer = directionToViewer(position);
    const float seenDepth = min(viewDepth(position), kNowhere);

    // One pixel to the right and one down, at the same depth: how large a pixel is out
    // there, and which way the screen runs in the world.
    const float3 right =
        worldPositionFromDepth(uv + float2(inverseTargetSize.x, 0.0), depth, inverseViewProjection) - position;
    const float3 down =
        worldPositionFromDepth(uv + float2(0.0, inverseTargetSize.y), depth, inverseViewProjection) - position;
    // The radius on the screen. Up close it would cover the picture: no more of it is
    // looked at than a quarter of its height.
    const float reach = min(radius / max(length(right), 1.0e-9), 0.25 * targetSize.y);
    if (reach < 1.0)
    {
        // So far away that everything within the radius is this pixel.
        return float2(1.0, seenDepth);
    }

    // Each pixel looks in directions of its own, and from a start of its own.
    const float sliceNoise = interleavedGradientNoise(float2(texel));
    const float stepNoise = interleavedGradientNoise(float2(texel) + float2(47.0, 17.0));

    float visible = 0.0;
    float open = 0.0;
    for (uint slice = 0; slice < sliceCount; ++slice)
    {
        const float turn = (float(slice) + sliceNoise) / float(sliceCount) * kPi;
        const float2 across = float2(cos(turn), sin(turn));
        const SliceNormal seen = projectIntoSlice(normal, toViewer, right * across.x + down * across.y);
        if (seen.length <= 1.0e-4)
        {
            continue;
        }
        // With nothing in the way a side is open down to the surface itself, a right
        // angle from the normal; what rises above the surface closes it by as much,
        // at the most up to the direction to the viewer, which is open: the pixel is seen.
        const float before =
            min(seen.angle - kHalfPi + riseAlong(pixel, position, normal, toViewer, -across, reach, stepNoise), 0.0);
        const float after =
            max(seen.angle + kHalfPi - riseAlong(pixel, position, normal, toViewer, across, reach, stepNoise), 0.0);
        visible += seen.length * sliceVisibility(seen.angle, before, after);
        open += seen.length * openSliceVisibility(seen.angle);
    }
    // As a share of what would come through the same slices with nothing in the way: a
    // surface with nothing around it then gets all of it, whichever slices it asked.
    return float2(open > 1.0e-4 ? saturate(visible / open) : 1.0, seenDepth);
}
