// The light that leaves a surface towards the viewer, under the lights of the view.
//
// The deferred lighting pass and the forward pass both call shadeSurface(), so a surface
// looks the same whichever of them draws it. Until the engine has lights of its own,
// "the lights" are one key light and ambient light from a sky and a ground.

#pragma once

#include "common/Brdf.slang"
#include "common/View.slang"

float3 shadeSurface(Surface surface)
{
    const float3 toViewer = directionToViewer(surface.position);
    float3 color = shadeDirect(surface, toViewer, keyLightDirection, keyLightColor);
    color += shadeHemisphere(surface, toViewer, skyColor, groundColor);
    color += surface.emissive;
    return color;
}
