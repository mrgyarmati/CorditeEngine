// The last pass of a view: from the light of the scene to the picture a screen shows.
//
// In this order: chromatic aberration, bloom, exposure, white balance, vignette, tone
// mapping, grading, the lookup table, film grain, and the encoding of a display. What
// each of them does, and to what numbers, is described in
// engine/cordite/renderer/PostProcessSettings.h.

#include "common/Color.slang"
#include "common/Fullscreen.slang"
#include "common/PostProcess.slang"
#include "common/ToneMapping.slang"

// Linear light, the size of the target.
Texture2D<float4> sceneColor : register(t0, space0);
// The largest level of the bloom that the quality in use fills.
Texture2D<float4> bloom : register(t1, space0);
// One texel: the stops automatic exposure adds.
Texture2D<float4> exposure : register(t2, space0);
// The lookup table: `colorLutSize` slices side by side, each as many texels wide and
// high. Red runs across a slice, green down it, blue from slice to slice. Its colors are
// those of a display (sRGB-encoded), as an image editor has them.
Texture2D<float4> colorLut : register(t3, space0);
SamplerState linearClamp : register(s0, space0);

// A number that looks random, from another (M. Jarzynski and M. Olano, "Hash Functions
// for GPU Rendering", 2020).
uint hash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Between 0 and 1, different for every pixel and every seed, the same on every run.
float randomAt(uint2 pixel, uint seed)
{
    return float(hash(pixel.x + hash(pixel.y + hash(seed)))) * (1.0 / 4294967296.0);
}

// The color the lookup table has for a color: of the two slices on either side of its
// blue, the texels around its red and green, blended.
float3 lookUpColor(float3 color)
{
    const float size = colorLutSize;
    const float3 place = saturate(color) * (size - 1.0);
    const float slice = min(floor(place.b), size - 2.0);
    const float2 inSlice = (place.rg + 0.5) / float2(size * size, size);
    const float sliceWidth = 1.0 / size;
    const float3 low = colorLut.SampleLevel(linearClamp, float2(inSlice.x + slice * sliceWidth, inSlice.y), 0).rgb;
    const float3 high =
        colorLut.SampleLevel(linearClamp, float2(inSlice.x + (slice + 1.0) * sliceWidth, inSlice.y), 0).rgb;
    return lerp(low, high, place.b - slice);
}

[shader("pixel")]
float4 postProcessPixel(FullscreenVaryings input) : SV_Target0
{
    const uint2 pixel = uint2(input.position.xy);
    // -1 to 1 across the picture.
    const float2 fromCentre = input.uv * 2.0 - 1.0;

    // The lens: red is read a little further out and blue a little further in, more
    // towards the corners. Without it every pixel is read exactly.
    float3 light = sceneColor.Load(int3(pixel, 0)).rgb;
    if (chromaticAberration > 0.0)
    {
        const float2 shift = fromCentre * (dot(fromCentre, fromCentre) * 0.25 * chromaticAberration);
        light.r = sceneColor.SampleLevel(linearClamp, input.uv + shift, 0).r;
        light.b = sceneColor.SampleLevel(linearClamp, input.uv - shift, 0).b;
    }

    if (bloomIntensity > 0.0)
    {
        light += bloom.SampleLevel(linearClamp, input.uv, 0).rgb * bloomIntensity;
    }

    float scale = exposureScale;
    if (autoExposure > 0.5)
    {
        scale *= exp2(exposure.Load(int3(0, 0, 0)).r);
    }
    light *= scale * whiteBalance * vignetteScale(vignette, fromCentre);

    float3 color = applyToneMapper(toneMapper, light);
    color = applyGrading(color, saturation, contrast, lift, inverseGamma, gain);

    // From here on in the colors of a display.
    float3 shown = linearToSrgb(color);
    if (colorLutSize > 1.5 && colorLutIntensity > 0.0)
    {
        shown = lerp(shown, lookUpColor(shown), colorLutIntensity);
    }
    if (filmGrain > 0.0)
    {
        // Less of it in the highlights, where film has little.
        const float grain = randomAt(pixel, grainSeed) - 0.5;
        shown += grain * filmGrain * (1.0 - 0.5 * luminance(shown));
    }
    if (dither > 0.5)
    {
        // The sum of two random numbers: mostly small, a level at the most. It breaks
        // the bands a slow gradient would otherwise show at 256 levels. Towards black
        // and towards white it dies away, so that black stays black and white white.
        const float3 room = saturate(min(shown, 1.0 - shown) * 255.0);
        shown += room * (randomAt(pixel, 0xD17Eu) + randomAt(pixel, 0xD17Fu) - 1.0) / 255.0;
    }
    return float4(saturate(shown), 1.0);
}
