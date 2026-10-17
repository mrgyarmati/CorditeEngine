// Motion blur: what moved while the shutter was open is smeared along the way it went,
// before the rest of the picture is finished (engine/cordite/renderer/MotionBlurPass.h;
// the arithmetic is common/MotionBlur.slang, and MotionBlur.h says what it does).
//
// Three passes. The first finds the longest blur in each tile of the picture - a tile is
// as many pixels a side as the longest blur there may be - and the second the longest of
// each tile and the tiles around it: no pixel is reached by a blur from further away.
// The third blurs every pixel from points along the longest blur of its tile, taking in
// those that reach it. A pixel near which nothing moves keeps its color to the bit.

#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/Motion.slang"
#include "common/MotionBlur.slang"
#include "common/View.slang"

cbuffer MotionBlurConstants : register(b0, space1)
{
    // How much of the way a pixel went is blurred: the share of the time between two
    // pictures that the shutter is open.
    float amount;
    // The longest blur either way, in pixels: also how many pixels a side a tile is.
    uint radius;
    // How many points each pixel is blurred from.
    uint sampleCount;
    float motionBlurPadding0;
};

// The first pass reads nothing here; the second the longest blurs of the tiles, the third
// those of the tiles and the tiles around them.
Texture2D<float4> tiles : register(t0, space1);
// The picture as it was before it was blurred: what the third pass reads.
Texture2D<float4> source : register(t1, space1);
// The depth buffer of the view: reversed, nothing where nothing was drawn.
Texture2D<float> depthBuffer : register(t2, space1);
// The motion vectors of what was drawn (common/Motion.slang).
Texture2D<float4> motionVectors : register(t3, space1);

// How far away what the sky is: further than anything that is drawn.
static const float kSkyDepth = 1.0e9;

// How the pixel at `pixel` is blurred, either way, in pixels: by its motion vector, or
// where nothing was drawn by how the sky moved.
float2 blurAt(int2 pixel)
{
    const float depth = depthBuffer.Load(int3(pixel, 0));
    const float2 motion =
        depth <= 0.0 ? motionOfSky((float2(pixel) + 0.5) * inverseTargetSize) : motionVectors.Load(int3(pixel, 0)).xy;
    return blurOf(motion, targetSize, amount, float(radius));
}

// How far away what the pixel at `pixel` shows is, along the view.
float depthAt(int2 pixel)
{
    const float depth = depthBuffer.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        return kSkyDepth;
    }
    return viewDepth(worldPositionFromDepth((float2(pixel) + 0.5) * inverseTargetSize, depth, inverseViewProjection));
}

// The longer of two blurs; the first of two as long.
float2 longerOf(float2 first, float2 second)
{
    return dot(second, second) > dot(first, first) ? second : first;
}

[shader("pixel")]
float4 motionBlurTilesPixel(FullscreenVaryings input) : SV_Target0
{
    const int2 first = int2(input.position.xy) * int(radius);
    const int2 last = min(first + int(radius), int2(targetSize)) - 1;
    float2 longest = float2(0.0, 0.0);
    for (int y = first.y; y <= last.y; ++y)
    {
        for (int x = first.x; x <= last.x; ++x)
        {
            longest = longerOf(longest, blurAt(int2(x, y)));
        }
    }
    return float4(longest, 0.0, 1.0);
}

[shader("pixel")]
float4 motionBlurNeighboursPixel(FullscreenVaryings input) : SV_Target0
{
    uint width;
    uint height;
    tiles.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const int2 tile = int2(input.position.xy);
    float2 longest = float2(0.0, 0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            longest = longerOf(longest, tiles.Load(int3(clamp(tile + int2(x, y), int2(0, 0), last), 0)).xy);
        }
    }
    return float4(longest, 0.0, 1.0);
}

[shader("pixel")]
float4 motionBlurPixel(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    const float4 color = source.Load(int3(pixel, 0));
    const float2 longest = tiles.Load(int3(pixel / int(radius), 0)).xy;
    // Nothing near moves by half a pixel: nothing to blur.
    if (dot(longest, longest) < 0.25)
    {
        return color;
    }

    const int2 last = int2(targetSize) - 1;
    const float pixelSpeed = max(length(blurAt(pixel)), 0.5);
    const float pixelDepth = depthAt(pixel);
    // The pixel itself, as much as it stays where it is.
    float weight = 1.0 / pixelSpeed;
    float3 light = color.rgb * weight;
    for (uint index = 0; index < sampleCount; ++index)
    {
        const float2 offset = longest * motionBlurSampleAt(index, sampleCount, uint2(pixel));
        const int2 other = clamp(int2(floor(float2(pixel) + 0.5 + offset)), int2(0, 0), last);
        const float sampleWeight = motionBlurWeight(length(offset), max(length(blurAt(other)), 0.5), depthAt(other),
                                                    pixelSpeed, pixelDepth);
        weight += sampleWeight;
        light += source.Load(int3(other, 0)).rgb * sampleWeight;
    }
    return float4(light / weight, color.a);
}
