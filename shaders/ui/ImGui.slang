// Draws the user interface: textured, vertex-colored triangles in screen space.
//
// Colors arrive sRGB-encoded and are written unchanged to an 8-bit target, which is what
// Dear ImGui expects: its blending is designed in that encoding.

cbuffer UiConstants : register(b0)
{
    // Maps UI coordinates (pixels, origin at the top-left) to clip space:
    // clip = position * positionScale + positionOffset.
    float2 positionScale;
    float2 positionOffset;
};

Texture2D<float4> uiTexture : register(t0);
SamplerState uiSampler : register(s0);

// Matches ImDrawVert. The order of the members is the order of the vertex attributes.
struct UiVertex
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct UiVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
UiVaryings uiVertex(UiVertex input)
{
    UiVaryings output;
    output.position = float4(input.position * positionScale + positionOffset, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("pixel")]
float4 uiPixel(UiVaryings input) : SV_Target0
{
    return input.color * uiTexture.Sample(uiSampler, input.uv);
}
