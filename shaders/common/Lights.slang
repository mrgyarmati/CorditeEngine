// The lights of a view, and which of them reach a point.
//
// A view's lights come as three buffers next to its constants (space0): the lights at
// t0, the cluster grid at t1 and the clusters' lists at t2. The volume the camera sees
// is cut into a grid of clusters, each with the list of the point and spot lights that
// touch it, so a pixel only asks the lights near it. Directional lights reach everywhere
// and are at the front of the light buffer, in no list.
//
// The C++ side - the grid, the lists, and the arithmetic of clusterIndexOf() - is
// engine/cordite/renderer/LightClusters.h.

#pragma once

#include "common/Brdf.slang"
#include "common/View.slang"

// Matches GpuLightType.
static const uint kLightDirectional = 0;
static const uint kLightPoint = 1;
static const uint kLightSpot = 2;

// Matches kClusterColumns, kClusterRows and kClusterSlices.
static const uint kClusterColumns = 16;
static const uint kClusterRows = 9;
static const uint kClusterSlices = 24;

// Matches GpuLight.
struct Light
{
    // In the world.
    float3 position;
    // Where a point or spot light has faded to nothing, in metres.
    float range;
    // Linear color times intensity.
    float3 color;
    // The kind of light in the low 8 bits, its lighting channels in the 8 above.
    uint typeAndChannels;
    // Unit vector along which a directional or spot light shines.
    float3 direction;
    float lightPadding0;
    // A spot light's brightness at an angle from its axis whose cosine is c:
    // saturate(c * spotScale + spotOffset).
    float spotScale;
    float spotOffset;
    float lightPadding1;
    float lightPadding2;
};

// Where the list of a cluster is in lightIndices. Matches ClusterRange.
struct ClusterRange
{
    uint offset;
    uint count;
};

StructuredBuffer<Light> lights : register(t0, space0);
StructuredBuffer<ClusterRange> clusterGrid : register(t1, space0);
StructuredBuffer<uint> lightIndices : register(t2, space0);

uint lightType(Light light)
{
    return light.typeAndChannels & 0xFF;
}

uint lightChannels(Light light)
{
    return (light.typeAndChannels >> 8) & 0xFF;
}

// The cluster a point is in: `uv` is its place in the picture, from (0, 0) at the top
// left to (1, 1); `depth` how far it is in front of the camera.
uint clusterIndexOf(float2 uv, float depth)
{
    // Brought into range as numbers, before they become indices: what is outside the
    // grid counts to its nearest cluster, and nothing becomes an index of nowhere.
    const uint column = uint(clamp(uv.x * float(kClusterColumns), 0.0, float(kClusterColumns - 1)));
    const uint row = uint(clamp(uv.y * float(kClusterRows), 0.0, float(kClusterRows - 1)));
    // Slices get deeper by a constant factor with perspective, and are equally deep
    // without.
    const float position = orthographic > 0.5 ? depth : log2(max(depth, 1.0e-4));
    const uint slice =
        uint(clamp(position * clusterDepthScale + clusterDepthBias, 0.0, float(kClusterSlices - 1)));
    return (slice * kClusterRows + row) * kClusterColumns + column;
}

// How bright a point or spot light is at a distance: it falls off with the square of
// the distance, and is brought smoothly to nothing at the light's range, so that the
// range can be where the light stops being worked out. (Karis, "Real Shading in Unreal
// Engine 4", 2013.)
float distanceFalloff(float distanceSquared, float range)
{
    const float ratio = distanceSquared / (range * range);
    const float window = saturate(1.0 - ratio * ratio);
    // Nearer than a centimetre the light gets no brighter: a point has no size, a lamp has.
    return window * window / max(distanceSquared, 1.0e-4);
}

// How bright a spot light is in a direction: all of it inside the inner cone, none
// outside the outer one.
float coneFalloff(Light light, float3 toLight)
{
    const float amount = saturate(dot(-toLight, light.direction) * light.spotScale + light.spotOffset);
    return amount * amount;
}

// The light leaving a surface towards the viewer that comes from one light.
float3 shadeLight(Surface surface, float3 toViewer, Light light)
{
    if (lightType(light) == kLightDirectional)
    {
        return shadeDirect(surface, toViewer, -light.direction, light.color);
    }

    const float3 offset = light.position - surface.position;
    const float distanceSquared = dot(offset, offset);
    if (distanceSquared >= light.range * light.range)
    {
        return float3(0.0);
    }
    const float3 toLight = offset * rsqrt(max(distanceSquared, 1.0e-8));
    float brightness = distanceFalloff(distanceSquared, light.range);
    if (lightType(light) == kLightSpot)
    {
        brightness *= coneFalloff(light, toLight);
    }
    return shadeDirect(surface, toViewer, toLight, light.color * brightness);
}

// The light leaving a surface towards the viewer that comes from the lights of the
// view: every directional one, and the point and spot lights of the surface's cluster.
// `uv` is where the surface is in the picture. A light shines on the surface when the
// two share a lighting channel.
float3 shadeLights(Surface surface, float3 toViewer, float2 uv)
{
    const uint channels = surface.lightChannels;
    float3 color = float3(0.0);
    for (uint index = 0; index < directionalLightCount; ++index)
    {
        const Light light = lights[index];
        if ((lightChannels(light) & channels) != 0)
        {
            color += shadeLight(surface, toViewer, light);
        }
    }

    const ClusterRange range = clusterGrid[clusterIndexOf(uv, viewDepth(surface.position))];
    for (uint entry = 0; entry < range.count; ++entry)
    {
        const Light light = lights[lightIndices[range.offset + entry]];
        if ((lightChannels(light) & channels) != 0)
        {
            color += shadeLight(surface, toViewer, light);
        }
    }
    return color;
}
