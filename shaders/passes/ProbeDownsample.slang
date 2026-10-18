// Makes one level of one face of the picture of an environment probe from the level
// before it (EnvironmentProbes::makeLevels): each texel the average of the four it covers.
// The prefilter reads the smaller levels to blur with few reads (passes/
// EnvironmentPrefilter.slang).

#include "common/Fullscreen.slang"

// The level before, of the same face: a view of that one layer of the cube.
Texture2DArray<float4> larger : register(t0, space0);

float4 probeDownsamplePixel(FullscreenVaryings input) : SV_Target0
{
    const int2 corner = int2(input.position.xy) * 2;
    const float4 sum = larger.Load(int4(corner, 0, 0)) + larger.Load(int4(corner + int2(1, 0), 0, 0)) +
                       larger.Load(int4(corner + int2(0, 1), 0, 0)) + larger.Load(int4(corner + int2(1, 1), 0, 0));
    return sum * 0.25;
}
