// The last pass of a view: from the light of the scene to the picture a screen shows.
//
// In this order: chromatic aberration, bloom, exposure, white balance, vignette, tone
// mapping, grading, the lookup table, film grain, and the encoding of a display. What
// each of them does, and to what numbers, is described in
// engine/cordite/renderer/PostProcessSettings.h.
//
// When the edges of the picture are to be smoothed (passes/AntiAliasing.slang), this is
// the last pass but one: it leaves the grain to that pass, which would take every
// grain for an edge, and writes how bright each pixel is into alpha for it.

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
    if (antiAliased < 0.5)
    {
        shown = withGrain(shown, pixel);
    }
    if (dither > 0.5)
    {
        // The sum of two random numbers: mostly small, a level at the most. It breaks
        // the bands a slow gradient would otherwise show at 256 levels. Towards black
        // and towards white it dies away, so that black stays black and white white.
        const float3 room = saturate(min(shown, 1.0 - shown) * 255.0);
        shown += room * (randomAt(pixel, 0xD17Eu) + randomAt(pixel, 0xD17Fu) - 1.0) / 255.0;
    }
    shown = saturate(shown);
    return float4(shown, antiAliased > 0.5 ? luminance(shown) : 1.0);
}
