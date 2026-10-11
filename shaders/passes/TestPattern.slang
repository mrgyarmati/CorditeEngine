// Diagnostic pattern drawn when there is no scene to render. It is also the smallest
// complete example of a pass: a constant buffer, a fullscreen draw and one output.
//
// The pattern is fully determined by its constants, so rendering tests can compare it
// against a reference image.
#include "common/Fullscreen.slang"

cbuffer TestPatternConstants : register(b0)
{
    // Multiplies the pattern color.
    float4 tint;
    // Size of the render target in pixels.
    float2 resolution;
    // Seconds; moves the highlight bar across the screen.
    float time;
    float padding;
};

[shader("pixel")]
float4 testPatternPixel(FullscreenVaryings input) : SV_Target0
{
    const float2 uv = input.uv;

    // Square cells regardless of aspect ratio, eight along the shorter side.
    const float cellSize = min(resolution.x, resolution.y) / 8.0;
    const float2 cell = floor(uv * resolution / cellSize);
    const float checker = fmod(cell.x + cell.y, 2.0);

    // Red increases to the right and green downward, which makes a flipped or mirrored
    // image obvious at a glance.
    float3 color = float3(uv.x, uv.y, 0.25) * lerp(0.55, 1.0, checker);

    // A vertical bar that sweeps across once every four seconds.
    const float barCenter = frac(time * 0.25);
    const float bar = 1.0 - saturate(abs(uv.x - barCenter) * 40.0);
    color = lerp(color, float3(1.0, 1.0, 1.0), bar * 0.6);

    return float4(color * tint.rgb, 1.0);
}
