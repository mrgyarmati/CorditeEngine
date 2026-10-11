// Pixel shaders of the debug views that draw the meshes themselves rather than showing
// a channel of the G-buffer.

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
