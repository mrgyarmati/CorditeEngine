// Shows one thing the G-buffer holds instead of the lit picture: the debug views.
//
// The value is written where the lit color would go, so that everything after this pass
// - the display encoding, the editor's overlays - works as usual. Numbers that are not
// colors (normals, roughness) are written so that they come out of the display encoding
// as they are: a roughness of 0.5 is shown as mid-grey. Where nothing was drawn the
// background is written: no pixel is discarded, for the reason the lighting pass gives
// (passes/DeferredLighting.slang).

#include "common/Color.slang"
#include "common/Fog.slang"
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
static const uint kViewShadowCascades = 10;
static const uint kViewAmbientOcclusion = 11;
static const uint kViewFog = 12;

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
Texture2D<float4> ambientOcclusion : register(t5, space1);

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
    if (view == kViewFog)
    {
        // How much of the pixel is fog: white is all of it. Fog is in front of
        // everything, of where nothing was drawn too: there it is what the sky, or the
        // background, is seen through.
        const float2 uv = input.position.xy * inverseTargetSize;
        float amount = 0.0;
        if (depth <= 0.0)
        {
            amount = fogAmountTowards(viewerAt(uv), viewDirectionAt(uv));
        }
        else
        {
            const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
            amount = fogAmount(viewerOf(position), position);
        }
        return float4(asShown(float3(amount)), 1.0);
    }
    // Where nothing was drawn, the background shows.
    if (depth <= 0.0)
    {
        return float4(backgroundColor, 1.0);
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
    case kViewShadowCascades:
    {
        // Which cascade of the sun's shadow each pixel is looked up in: red, green, blue
        // and yellow from the nearest on, grey where the sun casts no shadow. The
        // brightness is the surface's own, so that the scene can still be made out.
        const float2 uv = input.position.xy * inverseTargetSize;
        const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
        const int cascade = shadowCascadeAt(viewDepth(position));
        float3 tint = float3(0.5);
        if (cascade == 0)
        {
            tint = float3(1.0, 0.25, 0.25);
        }
        else if (cascade == 1)
        {
            tint = float3(0.25, 1.0, 0.25);
        }
        else if (cascade == 2)
        {
            tint = float3(0.25, 0.25, 1.0);
        }
        else if (cascade == 3)
        {
            tint = float3(1.0, 1.0, 0.25);
        }
        const float3 base = gbufferBaseColor.Load(pixel).rgb;
        const float brightness = 0.35 + 0.65 * dot(base, float3(0.2126, 0.7152, 0.0722));
        color = asShown(tint * brightness);
        break;
    }
    case kViewAmbientOcclusion:
        // How much of the light from all around gets through to the surface: white is
        // all of it. White everywhere when the view draws no ambient occlusion.
        color = asShown(float3(screenOcclusion > 0.5 ? ambientOcclusion.Load(pixel).r : 1.0));
        break;
    default:
        break;
    }
    return float4(color, 1.0);
}
