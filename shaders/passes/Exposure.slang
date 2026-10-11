// Automatic exposure: how many stops a view adds to its picture so that it comes out,
// on average, as mid-grey - and gets there slowly, as an eye gets used to light.
//
// The smallest level of the bloom chain holds, in alpha, the logarithm of the brightness
// of the picture around each of its few texels (passes/Bloom.slang). Their average is
// the logarithm of the brightness of the picture as a whole, in the sense that counts
// for an eye: a small glaring light does not outweigh a dark room. The exposure of a
// view is one number, kept in a texture of one texel from one frame to the next.

#include "common/Fullscreen.slang"
#include "common/PostProcess.slang"
#include "common/ToneMapping.slang"

// The smallest level of the bloom chain that the quality in use fills.
Texture2D<float4> smallestLevel : register(t0, space0);
// The exposure the view had in the frame before, in stops.
Texture2D<float4> previousExposure : register(t1, space0);

[shader("pixel")]
float4 exposureMeterPixel(FullscreenVaryings input) : SV_Target0
{
    uint width = 0;
    uint height = 0;
    smallestLevel.GetDimensions(width, height);
    float sum = 0.0;
    for (uint y = 0; y < height; ++y)
    {
        for (uint x = 0; x < width; ++x)
        {
            sum += smallestLevel.Load(int3(x, y, 0)).a;
        }
    }
    const float averageLog = sum / float(max(width * height, 1));

    // The stops that make the average mid-grey, within what the settings allow.
    const float wanted = clamp(log2(kMidGrey) - averageLog, exposureMinimum, exposureMaximum);
    if (instantAdaptation > 0.5)
    {
        return float4(wanted, 0.0, 0.0, 1.0);
    }
    // Part of the way there: fewer stops are for a brighter picture.
    const float before = clamp(previousExposure.Load(int3(0, 0, 0)).r, exposureMinimum, exposureMaximum);
    const float share = wanted < before ? adaptationUp : adaptationDown;
    return float4(lerp(before, wanted, share), 0.0, 0.0, 1.0);
}
