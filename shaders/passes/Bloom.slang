// Bloom: light that is brighter than a display can show spills over what is around it.
//
// The picture is made smaller level by level, each half the size of the one before and
// a blur of it; then the levels are added up again from the smallest, each spread a
// little, so that the result holds a narrow glow and a wide one and everything between
// (J. Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare",
// 2014). The final pass adds a share of the largest level to the picture.
//
// The levels carry one more thing: in alpha, the logarithm of how bright the picture is
// there, of all of it and not only of what blooms. Averaged over the smallest level it
// says how bright the picture is as a whole, which is what automatic exposure asks
// (passes/Exposure.slang).

#include "common/Color.slang"
#include "common/Fullscreen.slang"
#include "common/PostProcess.slang"

// The scene color, or a level of the bloom.
Texture2D<float4> source : register(t0, space0);
SamplerState linearClamp : register(s0, space0);

// What is brighter than the threshold, with a soft start: light just above it blooms a
// little, not all at once.
float3 aboveThreshold(float3 color)
{
    const float brightness = max(color.r, max(color.g, color.b));
    const float knee = bloomThreshold * 0.5;
    float soft = clamp(brightness - bloomThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-4);
    const float share = max(soft, brightness - bloomThreshold) / max(brightness, 1.0e-4);
    return color * share;
}

// The thirteen places a level is read at to make the next one: a square of nine, two
// texels apart, and the four between them. Each read blends four texels, so together
// they cover six by six texels without a gap, weighted towards the middle.
struct Taps
{
    float4 outer[9];
    float4 inner[4];
};

Taps readTaps(float2 uv, float2 texel)
{
    Taps taps;
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            const float2 offset = float2(x - 1, y - 1) * 2.0 * texel;
            taps.outer[y * 3 + x] = source.SampleLevel(linearClamp, uv + offset, 0);
        }
    }
    for (int corner = 0; corner < 4; ++corner)
    {
        const float2 offset = float2((corner & 1) * 2 - 1, (corner >> 1) * 2 - 1) * texel;
        taps.inner[corner] = source.SampleLevel(linearClamp, uv + offset, 0);
    }
    return taps;
}

// The size of a texel of the level being read, in texture coordinates: the target is
// half as large (or, for the first level at a low quality, a quarter), and a step of a
// pixel of it moves the texture coordinate by one divided by its size.
float2 sourceTexel(float2 uv)
{
    return float2(ddx(uv.x), ddy(uv.y)) * 0.5;
}

// From the scene color to the first level of the bloom.
[shader("pixel")]
float4 bloomPrefilterPixel(FullscreenVaryings input) : SV_Target0
{
    const Taps taps = readTaps(input.uv, sourceTexel(input.uv));

    // Five overlapping boxes of four reads each: the middle one counts half, the four
    // around it an eighth each. Within a box, a read counts for less the brighter it is
    // ("Karis average"): one glaring pixel among dark ones then adds little, where
    // averaged plainly it would flicker as a large blot whenever it moves. A box whose
    // reads are all alike is their plain average, so a lamp of some size blooms in full.
    static const int kBoxes[4][4] = {
        {0, 1, 3, 4},
        {1, 2, 4, 5},
        {3, 4, 6, 7},
        {4, 5, 7, 8},
    };
    float3 bloom = float3(0.0);
    float logSum = 0.0;
    for (int box = 0; box < 5; ++box)
    {
        float3 sum = float3(0.0);
        float weights = 0.0;
        float logAverage = 0.0;
        for (int tap = 0; tap < 4; ++tap)
        {
            const float4 value = box < 4 ? taps.outer[kBoxes[box][tap]] : taps.inner[tap];
            const float3 blooming = aboveThreshold(value.rgb);
            const float weight = 1.0 / (1.0 + luminance(blooming));
            sum += blooming * weight;
            weights += weight;
            logAverage += logLuminance(luminance(value.rgb)) * 0.25;
        }
        const float share = box < 4 ? 0.125 : 0.5;
        bloom += sum / weights * share;
        logSum += logAverage * share;
    }
    return float4(bloom, logSum);
}

// From one level of the bloom to the next smaller one.
[shader("pixel")]
float4 bloomDownsamplePixel(FullscreenVaryings input) : SV_Target0
{
    const Taps taps = readTaps(input.uv, sourceTexel(input.uv));
    float4 result = taps.outer[4] * 0.125;
    result += (taps.outer[0] + taps.outer[2] + taps.outer[6] + taps.outer[8]) * 0.03125;
    result += (taps.outer[1] + taps.outer[3] + taps.outer[5] + taps.outer[7]) * 0.0625;
    result += (taps.inner[0] + taps.inner[1] + taps.inner[2] + taps.inner[3]) * 0.125;
    return result;
}

// From a level of the bloom into the next larger one, which is blended with it: what is
// written here counts for `bloomScatter`, what is there already for the rest.
[shader("pixel")]
float4 bloomUpsamplePixel(FullscreenVaryings input) : SV_Target0
{
    // The level being read is half the size of the target: its texels are two pixels wide.
    const float2 texel = float2(ddx(input.uv.x), ddy(input.uv.y)) * 2.0;
    float3 result = float3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            // A tent: 1 2 1 / 2 4 2 / 1 2 1, sixteenths.
            const float weight = float((2 - abs(x)) * (2 - abs(y))) / 16.0;
            result += source.SampleLevel(linearClamp, input.uv + float2(x, y) * texel, 0).rgb * weight;
        }
    }
    return float4(result, bloomScatter);
}
