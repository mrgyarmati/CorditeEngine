// The base pass: writes what the lighting needs to know about every visible solid
// surface into the G-buffer (see common/GBuffer.slang for its layout).
//
// It runs after the depth pre-pass and keeps only pixels whose depth is the one already
// there. That is also what cuts the holes of masked materials: where the pre-pass wrote
// nothing, nothing matches.

#include "common/GBuffer.slang"
#include "common/MeshVaryings.slang"
#include "materials/Material.slang"

[shader("pixel")]
GBufferOutput meshGBufferPixel(MeshVaryings input, bool isFrontFace : SV_IsFrontFace)
{
    return encodeGBuffer(evaluateSurface(input, isFrontFace));
}
