// The G-buffer of a view as a pass that reads it has it bound (renderer/GBufferBindings.h:
// register space 1), and the surface a pixel of it describes - the same for every pass that
// asks, so that what one of them works out of a surface the others can take back exactly.

#pragma once

#include "common/GBuffer.slang"
#include "common/View.slang"

Texture2D<float4> gbufferBaseColor : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);
Texture2D<float4> gbufferParameters : register(t2, space1);
Texture2D<float4> gbufferEmissive : register(t3, space1);
Texture2D<float4> sceneDepth : register(t4, space1);
Texture2D<float4> ambientOcclusion : register(t5, space1);

// The surface drawn in `pixel`, which its depth puts at `position`: as the base pass wrote
// it down, with what stands around it in the screen taken into its occlusion
// (passes/AmbientOcclusion.slang) - which keeps part of the light from all around away
// from it, as the material's own occlusion does.
Surface surfaceAt(int3 pixel, float3 position)
{
    Surface surface = decodeGBuffer(gbufferBaseColor.Load(pixel), gbufferNormal.Load(pixel).xy,
                                    gbufferParameters.Load(pixel), gbufferEmissive.Load(pixel).rgb, position);
    if (screenOcclusion > 0.5)
    {
        surface.occlusion *= ambientOcclusion.Load(pixel).r;
    }
    return surface;
}
