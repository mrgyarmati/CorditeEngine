// Copies a texture onto the render target, filtering when the sizes differ.
#include "common/Fullscreen.slang"

Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

[shader("pixel")]
float4 blitPixel(FullscreenVaryings input) : SV_Target0
{
    return sourceTexture.Sample(sourceSampler, input.uv);
}
