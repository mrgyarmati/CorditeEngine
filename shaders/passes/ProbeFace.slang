// Copies the picture of one face of an environment probe into its face of a cube map
// (EnvironmentProbes::copyFace). The picture is the light a view of probeFaceView() was
// drawn with: the face seen from inside the cube, which is the face as a cube map holds it
// turned left to right - so each pixel is taken from across the middle of its row.

#include "common/Fullscreen.slang"

// The light of the view, as large as a face.
Texture2D<float4> picture : register(t0, space0);

float4 probeFacePixel(FullscreenVaryings input) : SV_Target0
{
    uint width;
    uint height;
    picture.GetDimensions(width, height);
    const int2 pixel = int2(input.position.xy);
    return picture.Load(int3(int(width) - 1 - pixel.x, pixel.y, 0));
}
