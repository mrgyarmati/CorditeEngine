// The forward pass: shades a surface completely in one go and blends it over what is
// already in the picture.
//
// It draws what deferred shading cannot: surfaces that let what is behind them show
// through, of which there may be several in one pixel. Output is linear light; alpha is
// the opacity the blending uses.

#include "common/MeshVaryings.slang"
#include "common/SceneLighting.slang"
#include "materials/Material.slang"

[shader("pixel")]
float4 meshForwardPixel(MeshVaryings input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    const Surface surface = evaluateSurface(input, isFrontFace);
    return float4(shadeSurface(surface), surface.opacity);
}
