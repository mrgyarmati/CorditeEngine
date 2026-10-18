// Reflections from the screen, the second of two passes: what the reflection of each pixel
// met (passes/ScreenReflectionTrace.slang) takes the place of what the surroundings gave
// the lighting to reflect - the sky and ground of the ambient lights, the environment, the
// probes - as much as it is trusted, weighed as the lighting weighed the surroundings
// (shadeSurroundings() in common/SceneLighting.slang) and seen through the same fog. Every
// pixel is written; where nothing takes the place of anything, the light is as it was, to
// the bit.
//
// Compiled a second time with SCREEN_REFLECTIONS_ONLY, for the debug view that shows what
// the screen gives: the light met, as much as it is trusted, over black.

#include "common/Fullscreen.slang"
#include "common/GBufferSurface.slang"
#include "common/SceneLighting.slang"
#include "common/ScreenReflections.slang"

// The light of the scene as the lighting left it - a copy, since this pass draws into the
// scene color - and what the first pass found.
Texture2D<float4> litScene : register(t0, space2);
Texture2D<float4> reflectionHits : register(t1, space2);

[shader("pixel")]
float4 screenReflectionCompositePixel(FullscreenVaryings input) : SV_Target0
{
    const int3 pixel = int3(int2(input.position.xy), 0);
#if defined(SCREEN_REFLECTIONS_ONLY)
    const float3 kept = float3(0.0, 0.0, 0.0);
#else
    const float3 kept = litScene.Load(pixel).rgb;
#endif
    const float depth = sceneDepth.Load(pixel).r;
    const float4 found = reflectionHits.Load(int3(pixel.xy / int(reflectionDivisor), 0));
    if (depth <= 0.0 || found.w <= 0.0)
    {
        return float4(kept, 1.0);
    }
    const float3 position = worldPositionFromDepth((float2(pixel.xy) + 0.5) * inverseTargetSize, depth,
                                                   inverseViewProjection);
    const Surface surface = surfaceAt(pixel, position);
    const float trust =
        found.z * reflectionRoughnessFade(saturate(surface.roughness), reflectionMaxRoughness) * reflectionIntensity;
    if (!(trust > 0.0))
    {
        return float4(kept, 1.0);
    }
    const int2 metPixel = clamp(int2(found.xy * targetSize), int2(0, 0), int2(targetSize) - 1);
    const float3 met = litScene.Load(int3(metPixel, 0)).rgb;
#if defined(SCREEN_REFLECTIONS_ONLY)
    return float4(storableLight(met * trust), 1.0);
#else
    const float3 toViewer = directionToViewer(position);
    const float3 change = (met - surroundingLight(surface, toViewer).reflected) *
                          surroundingsReflectance(surface, toViewer) * surface.occlusion * trust;
    const float through = 1.0 - fogAmount(viewerOf(position), position);
    return float4(storableLight(max(kept + change * through, float3(0.0, 0.0, 0.0))), 1.0);
#endif
}
