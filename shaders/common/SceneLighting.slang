// The light that leaves a surface towards the viewer, under the lights of the view.
//
// The deferred lighting pass and the forward pass both call shadeSurface(), so a surface
// looks the same whichever of them draws it. A lighting feature is added here, never to
// one of the two.

#pragma once

#include "common/Brdf.slang"
#include "common/Lights.slang"
#include "common/View.slang"

// `uv` is where the surface is in the picture, from (0, 0) at the top left to (1, 1):
// it decides which lights are asked.
float3 shadeSurface(Surface surface, float2 uv)
{
    const float3 toViewer = directionToViewer(surface.position);
    float3 color = shadeLights(surface, toViewer, uv);
    color += shadeHemisphere(surface, toViewer, skyColor, groundColor);
    color += surface.emissive;
    return color;
}
