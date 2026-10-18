// Reflections from the screen, the first of two passes: follows the reflection of each
// pixel of the view - or of the first of each block of reflectionDivisor pixels each way -
// through the depth buffer (common/ScreenReflections.slang), and writes where in the
// picture it met what was drawn: x and y in red and green, how much that is trusted in
// blue, and in alpha whether it met anything. A surface too rough to take anything from
// the screen is not followed.

#include "common/Environment.slang"
#include "common/Fullscreen.slang"
#include "common/GBufferSurface.slang"
#include "common/ScreenReflections.slang"

[shader("pixel")]
float4 screenReflectionTracePixel(FullscreenVaryings input) : SV_Target0
{
    const int2 pixel = min(int2(input.position.xy) * int(reflectionDivisor), int2(targetSize) - 1);
    const float depth = sceneDepth.Load(int3(pixel, 0)).r;
    // Depth is reversed: zero is where nothing was drawn.
    if (depth <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float3 position = worldPositionFromDepth((float2(pixel) + 0.5) * inverseTargetSize, depth, inverseViewProjection);
    const Surface surface = surfaceAt(int3(pixel, 0), position);
    const float roughness = saturate(surface.roughness);
    if (reflectionRoughnessFade(roughness, reflectionMaxRoughness) <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    // Along the way the lighting looks up the reflections of the surroundings.
    const float3 direction = reflectionDirection(surface.normal, directionToViewer(position), roughness);
    const ScreenReflectionHit hit = traceScreenReflection(sceneDepth, reflectionMaxDistance, reflectionThickness,
                                                          reflectionSteps, position, surface.normal, direction);
    return hit.found ? float4(hit.uv, hit.confidence, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
