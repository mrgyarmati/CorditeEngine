// Deferred lighting: lights every pixel of the G-buffer once.
//
// The base pass has written down what each visible surface is; this pass reads that
// back, works out where the surface is from its depth, and computes the light that
// leaves it towards the viewer. Output is linear light. Pixels in which nothing was
// drawn are left alone: they keep the background.
//
// Compiled a second time with LIGHTING_ONLY, for the debug view that shows the light
// without the materials' colors: every surface a plain, matte grey.

#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/SceneLighting.slang"

Texture2D<float4> gbufferBaseColor : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);
Texture2D<float4> gbufferParameters : register(t2, space1);
Texture2D<float4> gbufferEmissive : register(t3, space1);
Texture2D<float4> sceneDepth : register(t4, space1);

[shader("pixel")]
float4 deferredLightingPixel(FullscreenVaryings input) : SV_Target0
{
    const int3 pixel = int3(int2(input.position.xy), 0);
    const float depth = sceneDepth.Load(pixel).r;
    // Depth is reversed: zero is as far as the view reaches, where nothing was drawn.
    if (depth <= 0.0)
    {
        discard;
    }

    const float2 uv = input.position.xy * inverseTargetSize;
    const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
    Surface surface = decodeGBuffer(gbufferBaseColor.Load(pixel), gbufferNormal.Load(pixel).xy,
                                    gbufferParameters.Load(pixel), gbufferEmissive.Load(pixel).rgb, position);
#if defined(LIGHTING_ONLY)
    surface.baseColor = float3(0.5);
    surface.metallic = 0.0;
    surface.emissive = float3(0.0);
#endif
    return float4(shadeSurface(surface, uv), 1.0);
}
