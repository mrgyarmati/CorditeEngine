// Ambient occlusion, second pass: a value for every pixel of the picture, from what the
// first pass (AmbientOcclusion.slang) found for it and for the pixels around it.
//
// The first pass looks in few directions from each pixel, in other ones from pixel to
// pixel, and perhaps only from one pixel in four. Here a pixel takes the average of what
// was found around it - but only on its own surface: what is at another depth than the
// surface would have there is left out, so that the darkness under a thing does not
// spread onto the thing.

#include "common/AmbientOcclusion.slang"
#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/View.slang"

// r: the share of the light that gets through, g: the distance from the camera.
Texture2D<float4> tracedOcclusion : register(t0, space1);
Texture2D<float4> sceneDepth : register(t1, space1);

// How the depth runs along one axis of the traced image at `centre`, per texel: the
// difference to the neighbour on the side where it is smaller. On the other side there
// may be another surface.
float depthSlope(int2 centre, int2 axis, int2 last, float centreDepth)
{
    const float before = tracedOcclusion.Load(int3(max(centre - axis, int2(0, 0)), 0)).g;
    const float after = tracedOcclusion.Load(int3(min(centre + axis, last), 0)).g;
    const float toBefore = centreDepth - before;
    const float toAfter = after - centreDepth;
    return abs(toBefore) < abs(toAfter) ? toBefore : toAfter;
}

[shader("pixel")]
float ambientOcclusionResolvePixel(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    const float depth = sceneDepth.Load(int3(pixel, 0)).r;
    if (depth <= 0.0)
    {
        return 1.0;
    }
    const float2 uv = (float2(pixel) + 0.5) * inverseTargetSize;
    const float seenDepth = viewDepth(worldPositionFromDepth(uv, depth, inverseViewProjection));

    uint width;
    uint height;
    tracedOcclusion.GetDimensions(width, height);
    const int2 last = int2(int(width), int(height)) - 1;
    const int2 centre = min(pixel / int(divisor), last);
    const float2 atCentre = tracedOcclusion.Load(int3(centre, 0)).rg;
    const float2 slope = float2(depthSlope(centre, int2(1, 0), last, atCentre.g),
                                depthSlope(centre, int2(0, 1), last, atCentre.g));
    // How far from the depth the surface would have a texel may be and still count as
    // the surface: a hundredth of the distance, and more where it runs steeply.
    const float tolerance = max(0.01 * seenDepth + 0.5 * (abs(slope.x) + abs(slope.y)), 1.0e-6);

    // Five by five texels when every pixel was traced, three by three of twice the
    // size when one in four was: much the same part of the picture.
    const int reach = divisor > 1 ? 1 : 2;
    float sum = 0.0;
    float weights = 0.0;
    for (int y = -reach; y <= reach; ++y)
    {
        for (int x = -reach; x <= reach; ++x)
        {
            const int2 texel = clamp(centre + int2(x, y), int2(0, 0), last);
            const float2 found = tracedOcclusion.Load(int3(texel, 0)).rg;
            // Where the texel is from this pixel, in texels, and the depth this pixel's
            // surface would have there.
            const float2 offset = (float2(texel * int(divisor)) - float2(pixel)) / float(divisor);
            const float expected = seenDepth + dot(offset, slope);
            const float near = exp(-dot(offset, offset) / float(2 * reach * reach));
            const float weight = near * saturate(1.0 - abs(found.g - expected) / tolerance);
            sum += found.r * weight;
            weights += weight;
        }
    }
    // A pixel with no surface of its own among the texels - a hair in front of a wall -
    // takes what is nearest.
    const float share = weights > 1.0e-4 ? sum / weights : atCentre.r;
    return pow(saturate(share), intensity);
}
