// Lights that light the fog: what they throw towards the viewer from the air in front of
// what the viewer sees. engine/cordite/renderer/VolumetricFog.h says what that is, and has
// the same arithmetic: change both.
//
// The passes of VolumetricFogPass work it out for a grid of boxes over the view
// (common/FogVolume.slang) into a volume bound with the view (space0): the sums along the
// columns of the grid at t8, read with the sampler at s4. Shading reads it in front of a
// surface (fogLightBefore) and of the sky (fogLightTowards); beyond the grid the
// directional lights add theirs in a closed form.

#pragma once

#include "common/Fog.slang"
#include "common/FogVolume.slang"
#include "common/Lights.slang"
#include "common/Math.slang"
#include "common/View.slang"

Texture3D<float4> fogVolume : register(t8, space0);
SamplerState fogVolumeSampler : register(s4, space0);

// Matches kMostFogAnisotropy.
static const float kMostFogAnisotropy = 0.9;

// The phase function of Henyey and Greenstein: of the light the fog scatters at a point,
// the share that goes into one steradian at an angle whose cosine is `cosine` to the way
// the light was going.
float henyeyGreenstein(float cosine, float anisotropy)
{
    const float g = clamp(anisotropy, -kMostFogAnisotropy, kMostFogAnisotropy);
    const float c = clamp(cosine, -1.0, 1.0);
    const float denominator = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (4.0 * kPi * denominator * sqrt(denominator));
}

// What `light` throws towards the viewer from `position`, per unit of the light the fog
// scatters there: its fogColor, as much of it as reaches the position by its distance and
// its cone - or, from a rect light, the light of its face times how much of everything
// around the position the face fills, within its range window - times the phase function
// of the way from the light's middle. Not what stands in its way, nor its cookie: the
// caller asks for those, which cost more. `toViewer` is a unit vector.
float3 lightInFog(Light light, float3 position, float3 toViewer)
{
    if (lightType(light) == kLightDirectional)
    {
        return light.fogColor * henyeyGreenstein(dot(light.direction, toViewer), fogAnisotropy);
    }
    const float3 offset = position - light.position;
    const float distanceSquared = dot(offset, offset);
    if (distanceSquared >= light.range * light.range)
    {
        return float3(0.0);
    }
    const float3 travels = offset / sqrt(max(distanceSquared, 1.0e-8));
    float brightness = 0.0;
    if (lightType(light) == kLightRect)
    {
        brightness = rangeWindow(distanceSquared, light.range) *
                     rectLightSolidAngle(light.position, light.rectRight, rectLightUp(light), position) /
                     max(rectLightArea(light), 1.0e-8);
    }
    else
    {
        brightness = distanceFalloff(distanceSquared, light.range);
        if (lightType(light) == kLightSpot)
        {
            brightness *= coneFalloff(light, -travels);
        }
    }
    return light.fogColor * (brightness * henyeyGreenstein(dot(travels, toViewer), fogAnisotropy));
}

// What the directional lights throw towards the viewer, per unit of scattered light, from
// wherever nothing stands in their way. `toViewer` is a unit vector.
float3 sunlightInFog(float3 toViewer)
{
    float3 light = float3(0.0);
    for (uint index = 0; index < directionalLightCount; ++index)
    {
        light += lightInFog(lights[index], float3(0.0), toViewer);
    }
    return light;
}

// The sum along the column at `uv` up to a share `share` of the way through the slices:
// between the sums at the ends of the slices around it, as far as it is between them, and
// from nothing in the first.
float3 fogVolumeAt(float2 uv, float share)
{
    const float slices = fogVolumeSize.z;
    // The texel of a slice holds the sum at its end.
    const float3 coordinates = float3(uv, share - 0.5 / slices);
    return fogVolume.SampleLevel(fogVolumeSampler, coordinates, 0.0).rgb * saturate(share * slices);
}

// What the directional lights throw towards the viewer from the fog between the end of the
// grid and what is seen, as if nothing stood in their way: their light times how much the
// fog's amount grows on the way. `amountThere` is the amount of what is seen.
float3 fogLightBeyondGrid(float3 viewer, float3 direction, float amountThere)
{
    const float gridEnd = fogVolumeDistance / max(dot(direction, cameraForward), 1.0e-4);
    const float grows = amountThere - fogAmount(viewer, viewer + direction * gridEnd);
    return fogScattering * sunlightInFog(-direction) * max(grows, 0.0);
}

// What the lights throw towards the viewer from the fog in front of `position`, which is
// seen at `uv` of the picture: nothing when no light lights the fog of the view.
float3 fogLightBefore(float2 uv, float3 position)
{
    if (fogVolumeDistance <= 0.0)
    {
        return float3(0.0);
    }
    const float depth = viewDepth(position);
    float3 light = fogVolumeAt(uv, fogSliceShare(depth));
    if (depth > fogVolumeDistance)
    {
        const float3 viewer = viewerOf(position);
        const float3 way = position - viewer;
        const float3 direction = way / max(length(way), 1.0e-6);
        light += fogLightBeyondGrid(viewer, direction, fogAmount(viewer, position));
    }
    return light;
}

// The same for what is infinitely far away along `direction`, seen at `uv`: the sky.
float3 fogLightTowards(float2 uv, float3 direction)
{
    if (fogVolumeDistance <= 0.0)
    {
        return float3(0.0);
    }
    const float3 viewer = viewerAt(uv);
    return fogVolumeAt(uv, 1.0) + fogLightBeyondGrid(viewer, direction, fogAmountTowards(viewer, direction));
}
