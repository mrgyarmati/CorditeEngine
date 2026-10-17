// Pixel shaders of the debug views that draw the meshes themselves rather than showing
// a channel of the G-buffer.

#include "common/Math.slang"
#include "common/MeshVaryings.slang"

// Wireframe: every edge in one light color. Drawn with lines instead of filled
// triangles; the shader has nothing to decide.
[shader("pixel")]
float4 meshWireframePixel(MeshVaryings input) : SV_Target0
{
    return float4(0.55, 0.62, 0.70, 1.0);
}

// Overdraw: every surface adds a little, hidden or not, so a pixel is the brighter the
// more often it is drawn. Eight layers reach white in the channel that fills up last.
[shader("pixel")]
float4 meshOverdrawPixel(MeshVaryings input) : SV_Target0
{
    return float4(0.25, 0.125, 0.0625, 1.0);
}

// Levels of detail: each mesh in the color of the level it is drawn at - the mesh itself
// grey, then green, yellow, orange and red as the levels get coarser - and a little
// darker where it turns away from above, so that shapes can be told apart.
[shader("pixel")]
float4 meshLodPixel(MeshVaryings input) : SV_Target0
{
    const float3 kLevelColors[5] = {
        float3(0.60, 0.60, 0.60), float3(0.20, 0.75, 0.20), float3(0.85, 0.80, 0.15),
        float3(0.90, 0.45, 0.10), float3(0.85, 0.15, 0.10),
    };
    const float3 normal = normalizeOr(input.normal, float3(0.0, 1.0, 0.0));
    const float facing = 0.55 + 0.45 * saturate(dot(normal, float3(0.0, 1.0, 0.0)) * 0.5 + 0.5);
    return float4(kLevelColors[min(input.lod, 4u)] * facing, 1.0);
}
