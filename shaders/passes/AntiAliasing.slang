// Anti-aliasing, worked out from the finished picture: the steps that slanted edges
// have in a picture made of pixels are smoothed.
//
// This is "fast approximate anti-aliasing" (T. Lottes, "FXAA 3.11", 2011), written out
// here in its high-quality form. For every pixel:
//
// 1. Is it on an edge? Its brightness is compared with that of the four pixels beside
//    it; where they differ by little, the pixel is left as it is.
// 2. Which way does the edge run - along the rows or along the columns - and on which
//    side of the pixel is it? The side whose pixel differs most.
// 3. How far does the edge run before it steps to the next row? It is followed both
//    ways, on the line between the two rows, until the brightness there is no longer
//    half way between them.
// 4. A pixel near the end of such a run is nearly half covered by what is on the other
//    side; one in its middle is not covered at all. The pixel is read again, moved
//    towards the edge by that much, and the filtering of the texture does the blend.
//
// A pixel that stands out from all eight around it - a speck, the end of a line - is
// moved a little whatever the edge says, which softens what is finer than a pixel.
//
// The picture comes in the colors of a display, with its brightness in alpha
// (passes/PostProcess.slang writes that), and goes out the same way. Film grain is laid
// over it here, after the smoothing: before it, every grain would be an edge.

#include "common/Color.slang"
#include "common/Fullscreen.slang"
#include "common/PostProcess.slang"

// rgb: the finished picture, in the colors of a display. a: how bright each pixel is.
Texture2D<float4> picture : register(t0, space0);
SamplerState linearClamp : register(s0, space0);

// How much of the way to its neighbour a pixel that stands out from all around it is
// moved at the most.
static const float kSubpixelBlend = 0.75;

// How far an edge is followed with each step, in pixels: closely at first, where a
// pixel's width matters, then in strides. A search of fewer steps leaves out the first
// of them; one of five has its own.
static const uint kMostSteps = 12;
static const float kStepLengths[kMostSteps] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
static const uint kFewSteps = 5;
static const float kFewStepLengths[kFewSteps] = {1.0, 1.5, 2.0, 4.0, 12.0};

float stepLength(uint step, uint count)
{
    return count <= kFewSteps ? kFewStepLengths[min(step, kFewSteps - 1)]
                              : kStepLengths[min(kMostSteps - min(count, kMostSteps) + step, kMostSteps - 1)];
}

float brightnessAt(float2 uv)
{
    return picture.SampleLevel(linearClamp, uv, 0).a;
}

// The color of the pixel at `uv` once its edges are smoothed. `texel` is the size of a
// pixel in texture coordinates.
float3 smoothed(float2 uv, float2 texel)
{
    const float4 here = picture.SampleLevel(linearClamp, uv, 0);
    const float up = brightnessAt(uv + float2(0.0, -texel.y));
    const float down = brightnessAt(uv + float2(0.0, texel.y));
    const float left = brightnessAt(uv + float2(-texel.x, 0.0));
    const float right = brightnessAt(uv + float2(texel.x, 0.0));

    // 1. An edge is where the pixel and its four neighbours are not all alike: by a
    // share of the brightest of them, so that what is dark may differ by less, and by
    // something at the least, so that noise in the dark is not taken for an edge.
    const float brightest = max(here.a, max(max(up, down), max(left, right)));
    const float darkest = min(here.a, min(min(up, down), min(left, right)));
    const float range = brightest - darkest;
    if (range < max(antiAliasingEdgeMinimum, brightest * antiAliasingEdgeThreshold))
    {
        return here.rgb;
    }

    const float upLeft = brightnessAt(uv + float2(-texel.x, -texel.y));
    const float upRight = brightnessAt(uv + float2(texel.x, -texel.y));
    const float downLeft = brightnessAt(uv + float2(-texel.x, texel.y));
    const float downRight = brightnessAt(uv + float2(texel.x, texel.y));

    // 2. An edge that runs along the rows shows as a change from one row to the next:
    // in this column twice as much as in the two beside it.
    const float acrossRows = abs(upLeft + downLeft - 2.0 * left) + 2.0 * abs(up + down - 2.0 * here.a) +
                             abs(upRight + downRight - 2.0 * right);
    const float acrossColumns = abs(upLeft + upRight - 2.0 * up) + 2.0 * abs(left + right - 2.0 * here.a) +
                                abs(downLeft + downRight - 2.0 * down);
    const bool alongRows = acrossRows >= acrossColumns;

    // The two pixels on either side of this one, across the edge, and which of them
    // differs more: between it and this pixel the edge runs.
    const float before = alongRows ? up : left;
    const float after = alongRows ? down : right;
    const float changeBefore = abs(before - here.a);
    const float changeAfter = abs(after - here.a);
    const bool edgeIsBefore = changeBefore >= changeAfter;
    // One pixel across the edge, towards it; and the brightness half way.
    const float across = (alongRows ? texel.y : texel.x) * (edgeIsBefore ? -1.0 : 1.0);
    const float onEdge = 0.5 * ((edgeIsBefore ? before : after) + here.a);
    const float change = 0.25 * max(changeBefore, changeAfter);

    // 3. Along the edge, on the line between the two rows: there, the filtering of the
    // texture gives the mean of both, and where that leaves what it is here the run
    // of the edge is over.
    const float2 start = uv + (alongRows ? float2(0.0, 0.5 * across) : float2(0.5 * across, 0.0));
    const float2 along = alongRows ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 back = start;
    float2 ahead = start;
    float endBack = 0.0;
    float endAhead = 0.0;
    bool foundBack = false;
    bool foundAhead = false;
    const uint steps = clamp(antiAliasingSteps, 1u, kMostSteps);
    for (uint step = 0; step < steps; ++step)
    {
        const float2 stride = along * stepLength(step, steps);
        if (!foundBack)
        {
            back -= stride;
            endBack = brightnessAt(back) - onEdge;
            foundBack = abs(endBack) >= change;
        }
        if (!foundAhead)
        {
            ahead += stride;
            endAhead = brightnessAt(ahead) - onEdge;
            foundAhead = abs(endAhead) >= change;
        }
        if (foundBack && foundAhead)
        {
            break;
        }
    }

    // 4. How far the nearer end is, as a share of the whole run: at the end itself half
    // of the pixel belongs to the other side, in the middle of the run none of it.
    // That holds on the side of the edge that gives way at that end - where the
    // brightness there has gone the other way than this pixel's has - and on the other
    // side the pixel is left alone.
    const float distanceBack = alongRows ? uv.x - back.x : uv.y - back.y;
    const float distanceAhead = alongRows ? ahead.x - uv.x : ahead.y - uv.y;
    const bool backIsNearer = distanceBack < distanceAhead;
    const float nearer = min(distanceBack, distanceAhead);
    const float run = distanceBack + distanceAhead;
    const bool endIsDarker = (backIsNearer ? endBack : endAhead) < 0.0;
    const bool hereIsDarker = here.a - onEdge < 0.0;
    const float covered = endIsDarker != hereIsDarker ? 0.5 - nearer / run : 0.0;

    // What stands out from all eight around it is softened whatever the edge says.
    const float around =
        (2.0 * (up + down + left + right) + (upLeft + upRight + downLeft + downRight)) * (1.0 / 12.0);
    const float standsOut = saturate(abs(around - here.a) / max(range, 1.0e-6));
    const float eased = standsOut * standsOut * (3.0 - 2.0 * standsOut);
    const float softened = eased * eased * kSubpixelBlend;

    const float moved = max(covered, softened) * across;
    return picture.SampleLevel(linearClamp, uv + (alongRows ? float2(0.0, moved) : float2(moved, 0.0)), 0).rgb;
}

[shader("pixel")]
float4 antiAliasingPixel(FullscreenVaryings input) : SV_Target0
{
    uint width = 0;
    uint height = 0;
    picture.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(max(width, 1u), max(height, 1u));
    // The middle of the pixel, exactly: everything is measured from there.
    const float2 uv = (floor(input.position.xy) + 0.5) * texel;
    return float4(saturate(withGrain(smoothed(uv, texel), uint2(input.position.xy))), 1.0);
}
