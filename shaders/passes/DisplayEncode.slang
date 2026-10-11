// Turns the linear scene color into the display-ready image.
//
// For now that is the sRGB encoding alone: values above one clip. Exposure, tone
// mapping and color grading will take place here once the engine renders in HDR
// throughout.

#include "common/Color.slang"
#include "common/Fullscreen.slang"

// Linear light, the same size as the render target.
Texture2D<float4> sceneColor : register(t0);

[shader("pixel")]
float4 displayEncodePixel(FullscreenVaryings input) : SV_Target0
{
    const float3 color = sceneColor.Load(int3(int2(input.position.xy), 0)).rgb;
    return float4(linearToSrgb(saturate(color)), 1.0);
}
