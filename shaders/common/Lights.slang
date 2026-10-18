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
#include "common/Shadows.slang"
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
    // The first of the light's shadow views (common/Shadows.slang), or -1 when it
    // casts no shadows.
    int shadowView;
    // A spot light's brightness at an angle from its axis whose cosine is c:
    // saturate(c * spotScale + spotOffset).
    float spotScale;
    float spotOffset;
    // Shadow-map texels by which a surface is looked up nearer the light, and along
    // its normal.
    float shadowBias;
    float shadowNormalOffset;
    // Where the cookie of the light is laid on what it lights: its right, divided by the
    // tangent of a spot light's outer cone or by the size of a directional light's cookie
    // (cookieAxisOf, renderer/LightCookies.h); nothing without a cookie.
    float3 cookieRight;
    // The layer of the cookie in lightCookies, or -1 for none.
    int cookie;
    // What the light throws into a fog lit by lights (common/VolumetricFog.slang), as
    // `color` is what it shines on surfaces with: black for a light that gives it nothing.
    float3 fogColor;
    float lightPadding;
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

// The cookies of the view (renderer/LightCookies.h): pictures lights shine through, one
// layer each, in the colors of a display - a filter is a color.
Texture2DArray<float4> lightCookies : register(t7, space0);
SamplerState cookieSampler : register(s3, space0);

// Matches kLightCookieSize.
static const float kLightCookieSize = 256.0;

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

// What of the color of a light its cookie lets through towards `position`: all of it for
// a light without one. Where on the cookie the position is matches cookieUvOf
// (renderer/LightCookies.h): change both. The level of the cookie's pictures is the one
// whose texels are about as large as `footprint`, the size in metres of what the light is
// worked out for there - worked out rather than taken from neighbouring pixels, which may
// be shading other lights.
float3 cookieFilterAt(Light light, float3 position, float footprint)
{
    if (light.cookie < 0)
    {
        return float3(1.0);
    }
    const float3 offset = position - light.position;
    const float3 up = cross(light.cookieRight, light.direction);
    float2 uv = float2(dot(offset, light.cookieRight), -dot(offset, up));
    // Texels of the cookie in a metre, across where the position is.
    float texelsPerMetre = kLightCookieSize * length(light.cookieRight);
    if (lightType(light) == kLightSpot)
    {
        const float depth = dot(offset, light.direction);
        if (depth <= 0.0)
        {
            return float3(0.0);
        }
        uv = 0.5 + 0.5 * uv / depth;
        texelsPerMetre *= 0.5 / depth;
    }
    const float level = log2(max(footprint * texelsPerMetre, 1.0e-6));
    return lightCookies.SampleLevel(cookieSampler, float3(uv, float(light.cookie)), level).rgb;
}

// The same for a pixel of the view that shows `position`.
float3 cookieFilter(Light light, float3 position)
{
    return cookieFilterAt(light, position, pixelFootprint(position));
}

// The light leaving a surface towards the viewer that comes from one light. Shadows
// are looked up last, and only for what would be lit: they are the costly part.
float3 shadeLight(Surface surface, float3 toViewer, Light light)
{
    if (lightType(light) == kLightDirectional)
    {
        const float3 toSun = -light.direction;
        if (dot(surface.normal, toSun) <= 0.0)
        {
            return float3(0.0);
        }
        const float reaching =
            sunVisibility(light.shadowView, light.shadowBias, light.shadowNormalOffset, surface, toSun);
        return shadeDirect(surface, toViewer, toSun,
                           light.color * reaching * cookieFilter(light, surface.position));
    }

    const float3 offset = light.position - surface.position;
    const float distanceSquared = dot(offset, offset);
    if (distanceSquared >= light.range * light.range)
    {
        return float3(0.0);
    }
    const float distance = sqrt(max(distanceSquared, 1.0e-8));
    const float3 toLight = offset / distance;
    float brightness = distanceFalloff(distanceSquared, light.range);
    const bool isSpot = lightType(light) == kLightSpot;
    if (isSpot)
    {
        brightness *= coneFalloff(light, toLight);
    }
    if (brightness <= 0.0 || dot(surface.normal, toLight) <= 0.0)
    {
        return float3(0.0);
    }
    brightness *= localLightVisibility(light.shadowView, !isSpot, light.position, light.direction, light.shadowBias,
                                       light.shadowNormalOffset, surface, toLight);
    // A point light has no cookie: its filter lets everything through.
    return shadeDirect(surface, toViewer, toLight, light.color * brightness * cookieFilter(light, surface.position));
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
