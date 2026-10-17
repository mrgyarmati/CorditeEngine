// The base pass: writes what the lighting needs to know about every visible solid
// surface into the G-buffer (see common/GBuffer.slang for its layout), and how far what
// each pixel shows moved since the view was last drawn (common/Motion.slang).
//
// It runs after the depth pre-pass and keeps only pixels whose depth is the one already
// there. That is also what cuts the holes of masked materials: where the pre-pass wrote
// nothing, nothing matches. It is compiled with MOTION, as the pre-pass is: the vertex
// stage of both is the same code (MeshPassDesc::motion).

#if !defined(MOTION)
#error "the base pass writes motion vectors: compile it with MOTION"
#endif

#include "common/GBuffer.slang"
#include "common/MeshVaryings.slang"
#include "common/Motion.slang"
#include "materials/Material.slang"

// What the base pass writes: the targets of the G-buffer, then the motion vector.
struct BasePassOutput
{
    GBufferOutput surface;
    float2 motion : SV_Target4;
};

[shader("pixel")]
BasePassOutput meshGBufferPixel(MeshVaryings input, bool isFrontFace : SV_IsFrontFace)
{
    BasePassOutput output;
    output.surface = encodeGBuffer(evaluateSurface(input, isFrontFace));
    output.motion = motionBetween(input.previousClipPosition, input.currentClipPosition);
    return output;
}
