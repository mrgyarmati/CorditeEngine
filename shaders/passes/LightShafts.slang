// Light shafts: the light of the sun scattered towards the viewer along rays from where
// the sun is in the picture, past what stands in its way (engine/cordite/renderer/LightShaftPass.h;
// K. Mitchell, "Volumetric light scattering as a post-process", GPU Gems 3, 2007).
//
// Five passes. The first reads the picture and the depth into an image half as wide and
// as high: where nothing was drawn, what the sky gives to the shafts (common/LightShafts.slang);
// everything else gives nothing, and is what blocks them. Three blur that along the way to
// the sun - eight taps each, the steps of each pass eight times those of the pass before,
// which makes 512 taps over the length of the shafts. The last adds the blur to the light
// of the scene.

#include "common/Fullscreen.slang"
#include "common/LightShafts.slang"
#include "common/View.slang"

// The first pass reads the picture, each pass after it what the one before made.
Texture2D<float4> source : register(t0, space1);
// The depth buffer of the view: reversed, nothing where nothing was drawn.
Texture2D<float> depthBuffer : register(t1, space1);
SamplerState linearClamp : register(s0, space1);

[shader("pixel")]
float4 lightShaftSourcePixel(FullscreenVaryings input) : SV_Target0
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const int2 first = int2(input.position.xy) * 2;
    float3 sum = 0.0;
    [unroll]
    for (int index = 0; index < 4; ++index)
    {
        // A picture of an odd size: the last block reads its last pixel twice.
        const int2 pixel = min(first + int2(index & 1, index >> 1), last);
        // Only where nothing was drawn: everything else stands in the way of the light.
        if (!(depthBuffer.Load(int3(pixel, 0)) > 0.0))
        {
            const float2 uv = (float2(pixel) + 0.5) * inverseTargetSize;
            const float cosToSun = dot(viewDirectionAt(uv), towardsSun);
            sum += lightShaftSourceOf(source.Load(int3(pixel, 0)).rgb, sunColor, cosToSun, threshold);
        }
    }
    return float4(sum * 0.25, 1.0);
}

[shader("pixel")]
float4 lightShaftBlurPixel(FullscreenVaryings input) : SV_Target0
{
    // Eight taps from here towards the sun, read between texels: along the way the
    // shafts are soft. Taps beyond the edge of the picture read its edge, so that a sun
    // out of the picture sends its shafts in from there.
    const float2 step = (sunPoint - input.uv) * stepShare;
    float3 sum = 0.0;
    [unroll]
    for (int tap = 0; tap < 8; ++tap)
    {
        sum += source.SampleLevel(linearClamp, input.uv + step * float(tap), 0).rgb;
    }
    return float4(sum * 0.125, 1.0);
}

[shader("pixel")]
float4 lightShaftCompositePixel(FullscreenVaryings input) : SV_Target0
{
    // Added to the light of the scene, by the blending of the pass. Read between texels:
    // the shafts are soft across their rays as well, and have no edges to keep.
    return float4(source.SampleLevel(linearClamp, input.uv, 0).rgb * intensity, 0.0);
}
