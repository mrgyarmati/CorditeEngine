// Shows one thing the G-buffer holds instead of the lit picture: the debug views.
//
// The value is written where the lit color would go, so that everything after this pass
// - the display encoding, the editor's overlays - works as usual. Numbers that are not
// colors (normals, roughness) are written so that they come out of the display encoding
// as they are: a roughness of 0.5 is shown as mid-grey.

#include "common/Color.slang"
#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/Lights.slang"
#include "common/View.slang"

// Matches RenderDebugView in engine/cordite/renderer/RenderDebugView.h, for the views
// this pass draws.
static const uint kViewBaseColor = 1;
static const uint kViewNormals = 2;
static const uint kViewRoughness = 3;
static const uint kViewMetallic = 4;
static const uint kViewSpecular = 5;
static const uint kViewOcclusion = 6;
static const uint kViewEmissive = 7;
static const uint kViewDepth = 8;
static const uint kViewLightCount = 9;

cbuffer DebugViewConstants : register(b0, space1)
{
    uint view;
    float debugPadding0;
    float debugPadding1;
    float debugPadding2;
};

Texture2D<float4> gbufferBaseColor : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);
Texture2D<float4> gbufferParameters : register(t2, space1);
Texture2D<float4> gbufferEmissive : register(t3, space1);
Texture2D<float4> sceneDepth : register(t4, space1);

// A number that is to be seen as it is, in the color the display encoding turns back
// into it.
float3 asShown(float3 value)
{
    return srgbToLinear(saturate(value));
}

[shader("pixel")]
float4 debugViewPixel(FullscreenVaryings input) : SV_Target0
{
    const int3 pixel = int3(int2(input.position.xy), 0);
    const float depth = sceneDepth.Load(pixel).r;
    // Where nothing was drawn, the background stays.
    if (depth <= 0.0)
    {
        discard;
    }

    const float4 parameters = gbufferParameters.Load(pixel);
    float3 color = float3(0.0);
    switch (view)
    {
    case kViewBaseColor:
        color = gbufferBaseColor.Load(pixel).rgb;
        break;
    case kViewNormals:
        // The usual picture of a normal: each axis from -1 to 1 as a color from 0 to 1.
        color = asShown(decodeNormal(gbufferNormal.Load(pixel).xy) * 0.5 + 0.5);
        break;
    case kViewRoughness:
        color = asShown(float3(parameters.r));
        break;
    case kViewMetallic:
        color = asShown(float3(parameters.g));
        break;
    case kViewSpecular:
        color = asShown(float3(parameters.b));
        break;
    case kViewOcclusion:
        color = asShown(float3(parameters.a));
        break;
    case kViewEmissive:
        color = gbufferEmissive.Load(pixel).rgb;
        break;
    case kViewDepth:
    {
        // Near is white and far is black; halfway to black at four metres.
        const float3 position =
            worldPositionFromDepth(input.position.xy * inverseTargetSize, depth, inverseViewProjection);
        color = asShown(float3(1.0 / (1.0 + 0.25 * max(viewDepth(position), 0.0))));
        break;
    }
    case kViewLightCount:
    {
        // How many point and spot lights the pixel has to ask: none is dark grey, then
        // from blue through green to red at eight and more.
        const float2 uv = input.position.xy * inverseTargetSize;
        const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
        const uint count = clusterGrid[clusterIndexOf(uv, viewDepth(position))].count;
        const float heat = saturate(float(count) / 8.0);
        const float3 ramp = heat < 0.5 ? lerp(float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 0.0), heat * 2.0)
                                       : lerp(float3(0.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), heat * 2.0 - 1.0);
        color = count == 0 ? asShown(float3(0.1)) : asShown(ramp);
        break;
    }
    default:
        break;
    }
    return float4(color, 1.0);
}
