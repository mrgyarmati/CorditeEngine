// The light a viewer gets from a surface: what leaves it towards the viewer under the
// lights of the view, and what the fog between the two makes of that.
//
// The deferred lighting pass and the forward pass both call shadeSurface(), so a surface
// looks the same whichever of them draws it. A lighting feature is added here, never to
// one of the two.

#pragma once

#include "common/Brdf.slang"
#include "common/Environment.slang"
#include "common/Fog.slang"
#include "common/Lights.slang"
#include "common/View.slang"

// The most light a pixel of the scene color can hold: the largest of the half floats it
// is made of. What a GPU makes of more than that is its own business: some keep the
// largest half float, others "infinitely much" - and the passes that blur the picture
// make "not a number" of that, which spreads. A sun in an image of the sky, times the
// brightness an environment is given, gets there easily.
static const float kMostLight = 65504.0;

// `light` as the scene color can hold it. Every pass that writes scene color ends here.
float3 storableLight(float3 light)
{
    return min(light, float3(kMostLight));
}

// The light of everything around a scene, as it leaves a surface towards the viewer:
// that of the sky and the ground of its ambient lights, and that of its environment.
float3 shadeSurroundings(Surface surface, float3 toViewer)
{
    return shadeHemisphere(surface, toViewer, skyColor, groundColor) + shadeEnvironment(surface, toViewer);
}

// `uv` is where the surface is in the picture, from (0, 0) at the top left to (1, 1):
// it decides which lights are asked.
float3 shadeSurface(Surface surface, float2 uv)
{
    const float3 toViewer = directionToViewer(surface.position);
    float3 color = shadeLights(surface, toViewer, uv);
    color += shadeSurroundings(surface, toViewer);
    color += surface.emissive;
    // Made storable before the fog and after it: fog mixes, and what is mixed with
    // "infinitely much" is not a number.
    return storableLight(seenThroughFog(storableLight(color), viewerOf(surface.position), surface.position));
}
