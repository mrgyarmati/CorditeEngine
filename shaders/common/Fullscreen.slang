// Fullscreen triangle shared by every screen-space pass.
//
// A single triangle that overhangs the screen covers it without the diagonal seam a
// two-triangle quad has, and needs no vertex buffer: positions come from the vertex index.
#pragma once

struct FullscreenVaryings
{
    float4 position : SV_Position;
    // (0, 0) is the top-left corner of the target and (1, 1) the bottom-right.
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
FullscreenVaryings fullscreenVertex(uint vertexId : SV_VertexID)
{
    FullscreenVaryings output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
