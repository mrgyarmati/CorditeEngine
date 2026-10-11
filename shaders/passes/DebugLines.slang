// Draws colored world-space line segments for tools and debugging.
//
// Colors are display colors and are written unchanged; the pass runs on the finished
// image. Lines can fade out with distance from a point, which is how the editor grid
// disappears towards the horizon instead of ending at a hard edge.

cbuffer DebugLineConstants : register(b0)
{
    float4x4 viewProjection;
    // Alpha falls from 1 at fadeStart to 0 at fadeEnd metres from fadeOrigin.
    float3 fadeOrigin;
    float fadeStart;
    float fadeEnd;
    // Scalars, not a float3: a vector would be aligned differently by different layout rules.
    float padding0;
    float padding1;
    float padding2;
};

// Matches DebugVertex. The order of the members is the order of the vertex attributes.
struct DebugLineVertex
{
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct DebugLineVaryings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float3 worldPosition : TEXCOORD0;
};

[shader("vertex")]
DebugLineVaryings debugLineVertex(DebugLineVertex input)
{
    DebugLineVaryings output;
    output.position = mul(viewProjection, float4(input.position, 1.0));
    output.color = input.color;
    output.worldPosition = input.position;
    return output;
}

[shader("pixel")]
float4 debugLinePixel(DebugLineVaryings input) : SV_Target0
{
    float fade = 1.0 - smoothstep(fadeStart, fadeEnd, distance(input.worldPosition, fadeOrigin));
    return float4(input.color.rgb, input.color.a * fade);
}
