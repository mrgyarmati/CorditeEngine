// The depth pre-pass: draws the depth of every solid surface before anything is shaded.
//
// With the depth known, the base pass shades each pixel exactly once, however many
// surfaces lie behind one another, and the effects that work from depth can start early.
//
// A solid material needs no pixel shader for this. A masked one does: where its opacity
// is below the cutoff there is no surface, and no depth may be written.

#include "common/MeshVaryings.slang"
#include "materials/Material.slang"

[shader("pixel")]
void meshDepthMaskedPixel(MeshVaryings input)
{
    if (evaluateOpacity(input.uv0, input.uv1) < alphaCutoff)
    {
        discard;
    }
}
