// Deferred lighting: lights every pixel of the G-buffer once.
//
// The base pass has written down what each visible surface is; this pass reads that
// back, works out where the surface is from its depth, and computes the light that
// leaves it towards the viewer. Output is linear light. Pixels in which nothing was
// drawn show the sky of the environment, when the view has one that is to be seen;
// otherwise they are left alone and keep the background.
//
// Compiled a second time with LIGHTING_ONLY, for the debug view that shows the light
// without the materials' colors: every surface a plain, matte grey. And a third time
// with REFLECTIONS_ONLY, for the one that shows what there is to reflect: every surface
// a perfect mirror, lit by its surroundings alone.

#include "common/Fullscreen.slang"
#include "common/GBuffer.slang"
#include "common/SceneLighting.slang"

Texture2D<float4> gbufferBaseColor : register(t0, space1);
Texture2D<float4> gbufferNormal : register(t1, space1);
Texture2D<float4> gbufferParameters : register(t2, space1);
Texture2D<float4> gbufferEmissive : register(t3, space1);
Texture2D<float4> sceneDepth : register(t4, space1);

// The direction in which the surface drawn in a pixel runs along one axis of the
// screen: from the pixel one step back along `axis` to the pixel one step on, in the
// world, at unit length. Of those two neighbours the one whose depth is nearer the
// pixel's own is asked, because at the edge of an object the other shows something
// else. Zero when there is no neighbour to ask: beyond the border of the target there
// is none, and where nothing was drawn (depth zero) there is nothing to be beside.
float3 surfaceDirectionAlong(int2 pixel, int2 axis, float3 position, float depth)
{
    const int2 last = int2(targetSize) - 1;
    const int2 before = max(pixel - axis, int2(0, 0));
    const int2 after = min(pixel + axis, last);
    const float depthBefore = any(before != pixel) ? sceneDepth.Load(int3(before, 0)).r : 0.0;
    const float depthAfter = any(after != pixel) ? sceneDepth.Load(int3(after, 0)).r : 0.0;
    if (depthBefore <= 0.0 && depthAfter <= 0.0)
    {
        return float3(0.0);
    }

    const bool useBefore =
        depthBefore > 0.0 && (depthAfter <= 0.0 || abs(depthBefore - depth) < abs(depthAfter - depth));
    const int2 neighbour = useBefore ? before : after;
    const float3 beside = worldPositionFromDepth((float2(neighbour) + 0.5) * inverseTargetSize,
                                                 useBefore ? depthBefore : depthAfter, inverseViewProjection);
    return normalizeOr(useBefore ? position - beside : beside - position, float3(0.0));
}

// The normal of the surface itself in a pixel - of the triangle drawn there, whatever
// its normal map says - worked out from where the surface is in the pixels beside it,
// and facing the viewer. The G-buffer has no room for it. `fallback` stands in where
// the pixels beside it cannot tell: for a surface seen exactly edge-on, and for a
// thing a single pixel wide in front of the background.
float3 geometricNormalAt(int2 pixel, float3 position, float depth, float3 toViewer, float3 fallback)
{
    // Both directions have unit length, whatever a pixel covers - a fraction of a
    // millimetre up close, metres far away - so their cross product is as long as the
    // sine of the angle between them, and too short only where they do not span a plane.
    const float3 across = surfaceDirectionAlong(pixel, int2(1, 0), position, depth);
    const float3 down = surfaceDirectionAlong(pixel, int2(0, 1), position, depth);
    const float3 normal = cross(across, down);
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared <= 1.0e-8)
    {
        return fallback;
    }
    const float3 unit = normal * rsqrt(lengthSquared);
    return dot(unit, toViewer) < 0.0 ? -unit : unit;
}

[shader("pixel")]
float4 deferredLightingPixel(FullscreenVaryings input) : SV_Target0
{
    const int3 pixel = int3(int2(input.position.xy), 0);
    const float depth = sceneDepth.Load(pixel).r;
    const float2 uv = input.position.xy * inverseTargetSize;
    // Depth is reversed: zero is as far as the view reaches, where nothing was drawn.
    if (depth <= 0.0)
    {
        if (environmentVisible < 0.5)
        {
            discard;
        }
        // The sky. How far the view turns from this pixel to the ones beside and below
        // it tells how much of the sky the pixel covers.
        const float3 direction = viewDirectionAt(uv);
        const float3 beside = viewDirectionAt(uv + float2(inverseTargetSize.x, 0.0));
        const float3 below = viewDirectionAt(uv + float2(0.0, inverseTargetSize.y));
        const float turn = max(length(beside - direction), length(below - direction));
        return float4(storableLight(environmentSkyLight(direction, turn)), 1.0);
    }

    const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
    Surface surface = decodeGBuffer(gbufferBaseColor.Load(pixel), gbufferNormal.Load(pixel).xy,
                                    gbufferParameters.Load(pixel), gbufferEmissive.Load(pixel).rgb, position);
    // Shadows alone are looked up with the surface's own normal; a view without any
    // does not go looking for it.
    if (shadowViewCount > 0)
    {
        surface.geometricNormal =
            geometricNormalAt(pixel.xy, position, depth, directionToViewer(position), surface.normal);
    }
#if defined(LIGHTING_ONLY)
    surface.baseColor = float3(0.5);
    surface.metallic = 0.0;
    surface.emissive = float3(0.0);
#endif
#if defined(REFLECTIONS_ONLY)
    // A mirror throws back all the light of one direction, and nothing else.
    surface.baseColor = float3(1.0);
    surface.metallic = 1.0;
    surface.roughness = 0.0;
    surface.occlusion = 1.0;
    return float4(storableLight(shadeSurroundings(surface, directionToViewer(position))), 1.0);
#else
    return float4(shadeSurface(surface, uv), 1.0);
#endif
}
