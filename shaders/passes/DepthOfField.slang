// Depth of field: what is not at the focus distance is blurred, before the rest of the
// picture is finished (engine/cordite/renderer/DepthOfFieldPass.h).
//
// Three passes. The first reads the picture and the depth into an image half as wide and
// as high: the light of each block of four pixels, and how blurred it is - as blurred as
// the nearest of the four, so that what is in front grows over what is behind instead of
// the two being averaged. The second blurs that: a texel is made of the texels around it
// whose own blur reaches it - so that something sharp does not bleed into the blur around
// it, and what is blurred in front spreads over what is behind - and alpha says how much
// of the picture is to be replaced there. The third lays the blur over the picture, as
// much as the pixel's own blur or what in front reaches it says, taking for each pixel the
// texels of the blur that are blurred like it; where neither says anything, the picture is
// left exactly as it was.

#include "common/DepthOfField.slang"
#include "common/Fullscreen.slang"

// The first pass reads the picture, the second what the first made, the third what the
// second made.
Texture2D<float4> source : register(t0, space1);
// The depth buffer of the view: reversed, nothing where nothing was drawn.
Texture2D<float> depthBuffer : register(t1, space1);
// The third pass reads, besides the blur, what the first made: how blurred each texel is
// itself, in alpha.
Texture2D<float4> prefiltered : register(t2, space1);

// The angle between two taps of the blur: they lie on a spiral (H. Vogel, "A better way to
// construct the sunflower head", 1979), each as much of a disc further out as the one
// before, which covers it evenly whatever their number.
static const float kGoldenAngle = 2.39996323;

[shader("pixel")]
float4 depthOfFieldPrefilterPixel(FullscreenVaryings input) : SV_Target0
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const int2 first = int2(input.position.xy) * 2;
    float3 light = 0.0;
    // Of the four, the nearest: the blur only grows with the depth, so that is the least.
    float blur = farBlur;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        // A picture of an odd size: the last block reads its last pixel twice.
        const int2 pixel = min(first + int2(index & 1, index >> 1), last);
        light += source.Load(int3(pixel, 0)).rgb;
        const float2 uv = (float2(pixel) + 0.5) * inverseTargetSize;
        blur = min(blur, focusBlurOfPixel(uv, depthBuffer.Load(int3(pixel, 0))));
    }
    return float4(light * 0.25, blur);
}

[shader("pixel")]
float4 depthOfFieldGatherPixel(FullscreenVaryings input) : SV_Target0
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const int2 centre = int2(input.position.xy);
    const float4 here = source.Load(int3(centre, 0));
    const float hereSize = abs(here.a) * blurRadius;

    // The taps go outwards, and each one that does not reach this texel adds what has
    // been found so far, so that the outer ones cannot outweigh the inner (D. Gustafsson,
    // "Bokeh depth of field in a single pass", 2018).
    float3 sum = here.rgb;
    float count = 1.0;
    // How much of what was found came from in front of this texel.
    float inFront = 0.0;
    for (uint tap = 0; tap < tapCount; ++tap)
    {
        const float distance = sqrt((float(tap) + 0.5) / float(tapCount)) * blurRadius;
        const float angle = float(tap) * kGoldenAngle;
        // The texel the tap falls in, read as it is: filtered between texels, how blurred
        // a tap is would be a mixture across the edge of something, and a sharp thing
        // in front would glow into the blur behind it.
        const int2 there = clamp(centre + int2(round(distance * float2(cos(angle), sin(angle)))), int2(0, 0), last);
        const float4 tapped = source.Load(int3(there, 0));
        // How far its own blur reaches. What is behind this texel counts as far as this
        // texel's own blur reaches, and no further than twice that: a sharp thing in front
        // of a blurred background keeps its edge, and a blurred thing in front lets what is
        // behind it show through its edge.
        float size = abs(tapped.a) * blurRadius;
        if (tapped.a > here.a)
        {
            size = clamp(size, hereSize, hereSize * 2.0);
        }
        const float reaches = smoothstep(distance - 0.5, distance + 0.5, size);
        sum += lerp(sum / count, tapped.rgb, reaches);
        count += 1.0;
        if (tapped.a < here.a)
        {
            inFront += reaches;
        }
    }
    // A quarter of the disc covered by what is in front of it is enough for the blur to
    // take the place of the picture there; the blur already holds what it covers, and how
    // much.
    return float4(sum / count, saturate(4.0 * inFront / count));
}

[shader("pixel")]
float4 depthOfFieldCompositePixel(FullscreenVaryings input) : SV_Target0
{
    const float blur = focusBlurOfPixel(input.uv, depthBuffer.Load(int3(int2(input.position.xy), 0)));
    if (showFocus > 0.5)
    {
        // Black where it is sharp, red in front of that and blue behind, as much as it is
        // blurred.
        return float4(max(-blur, 0.0), 0.0, max(blur, 0.0), 1.0);
    }
    // The four texels of the blur around the pixel, each as much as it is near the pixel
    // and as it is blurred like the pixel: the texels along the edge of something sharp -
    // made of it and of what is behind it - are not spread over the blur beside them, a
    // pixel further (bilateral upsampling).
    uint width;
    uint height;
    source.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const float2 place = input.uv * float2(width, height) - 0.5;
    const int2 first = int2(floor(place));
    const float2 within = place - float2(first);
    float4 sum = 0.0;
    float weights = 0.0;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        const int2 offset = int2(index & 1, index >> 1);
        const int2 texel = clamp(first + offset, int2(0, 0), last);
        const float2 near = lerp(1.0 - within, within, float2(offset));
        const float weight = near.x * near.y / (1.0e-3 + abs(prefiltered.Load(int3(texel, 0)).a - blur));
        sum += source.Load(int3(texel, 0)) * weight;
        weights += weight;
    }
    const float4 blurred = sum / weights;
    // A pixel whose own blur is a texel of the half-sized image wide, or wider, is all
    // blur; one that is less blurred is laid over less. What in front reaches it comes on
    // top. Drawn with the blending of alpha: with nothing of either, the picture is
    // untouched.
    const float amount = max(saturate(abs(blur) * blurRadius), blurred.a);
    return float4(blurred.rgb, amount);
}
