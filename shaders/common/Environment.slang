// The light of everything far away: a sky, and what it lights.
//
// An environment is an image of all there is around a scene, as light. It reaches the
// shaders in three forms, bound next to the view's constants (space0):
//
// - nine numbers per color in the constants (environmentDiffuse): what it lights a
//   matte surface with, as spherical harmonics;
// - a cube map at t5 with a sampler at s1 (environmentRadiance): the light arriving from
//   each direction, blurred more from level to level, for reflections on surfaces from
//   mirror-smooth to rough;
// - the image itself at t6 with a sampler at s2 (environmentSky), for where the sky is
//   seen.
//
// A view without an environment has none of its light: the numbers are zero, and the
// textures a black texel. The C++ side - where an image becomes these three - is
// engine/cordite/renderer/EnvironmentMaps.h; the mapping between directions and places
// in the image is equirectUv() in common/Math.slang.

#pragma once

#include "common/Brdf.slang"
#include "common/SphericalHarmonics.slang"
#include "common/View.slang"

TextureCube<float4> environmentRadiance : register(t5, space0);
Texture2D<float4> environmentSky : register(t6, space0);
SamplerState environmentSampler : register(s1, space0);
SamplerState environmentSkySampler : register(s2, space0);

// A direction in the world as one in the environment, which may be turned about the
// vertical axis: the direction turned back by as much.
float3 towardsEnvironment(float3 direction)
{
    const float sine = environmentTurn.x;
    const float cosine = environmentTurn.y;
    return float3(direction.x * cosine - direction.z * sine, direction.y, direction.z * cosine + direction.x * sine);
}

// What a white, matte surface that faces `normal` gives off under the environment.
float3 environmentDiffuseLight(float3 normal)
{
    float basis[9];
    sphericalHarmonicsBasis(towardsEnvironment(normal), basis);
    float3 light = float3(0.0);
    for (int index = 0; index < 9; ++index)
    {
        light += environmentDiffuse[index].rgb * basis[index];
    }
    // Nine numbers overshoot beside a small, bright light: never less than nothing.
    return max(light, 0.0) * environmentIntensity;
}

// The light the environment sends from around `direction`, as a surface of the given
// roughness sees it: sharp for a mirror, blurred for a rough one.
float3 environmentReflectedLight(float3 direction, float roughness)
{
    const float level = saturate(roughness) * environmentLastLevel;
    return environmentRadiance.SampleLevel(environmentSampler, towardsEnvironment(direction), level).rgb *
           environmentIntensity;
}

// The sky where it is seen: the image itself, in the direction looked in. `turn` is how
// far that direction turns from one pixel of the picture to the next, in radians.
float3 environmentSkyLight(float3 direction, float turn)
{
    const float2 uv = equirectUv(normalizeOr(towardsEnvironment(direction), float3(0.0, 0.0, -1.0)));
    // The level whose texels are as large as a pixel of the picture, so that a sky seen
    // small does not flicker. It is worked out from how the direction turns: the place
    // in the image jumps where its left and right edges meet, and a level chosen by how
    // fast the place changes would be the smallest one along that seam.
    uint width;
    uint height;
    uint levels;
    environmentSky.GetDimensions(0, width, height, levels);
    const float level = log2(max(turn * float(height) / kPi, 1.0));
    return environmentSky.SampleLevel(environmentSkySampler, uv, level).rgb * environmentIntensity;
}

// The direction most of the light comes from that a surface reflects towards a viewer:
// that of a mirror for a smooth surface, bent towards the normal the rougher it is
// (S. Lagarde and C. de Rousiers, "Moving Frostbite to Physically Based Rendering", 2014).
float3 reflectionDirection(float3 normal, float3 toViewer, float roughness)
{
    // Never more than one: the root below is of what is left of it.
    const float alpha = saturate(roughness * roughness);
    const float3 mirrored = reflect(-toViewer, normal);
    return normalizeOr(lerp(normal, mirrored, (1.0 - alpha) * (sqrt(1.0 - alpha) + alpha)), normal);
}

// The environment probes that light the view (renderer/EnvironmentProbes.h): pictures of
// everything around points of the scene, which stand in for the environment in their
// boxes. `probeCount` of them at t9, the highest priority first; their harmonics at t10,
// nine a probe; their reflections at t11, a cube a probe, read with the sampler of the
// environment's. Matches GpuEnvironmentProbe.
struct EnvironmentProbe
{
    // From the world to the box's own space, in which the box is the cube from -0.5 to 0.5.
    float4x4 worldToBox;
    // How many metres one unit of the box's own space is, along each of its axes.
    float3 boxSize;
    // How far inside its faces the probe fades to nothing, in metres.
    float blendDistance;
    // Where its picture was taken.
    float3 capturePosition;
    // Scales its light.
    float intensity;
    // Where its harmonics are - from slot * 9 on - and its cube of reflections.
    uint slot;
    // kProbeBoxProjection and kProbeReflects.
    uint flags;
    uint probePadding1;
    uint probePadding2;
};

StructuredBuffer<EnvironmentProbe> environmentProbes : register(t9, space0);
StructuredBuffer<float4> probeHarmonics : register(t10, space0);
TextureCubeArray<float4> probeRadiance : register(t11, space0);

// Match kProbeBoxProjection, kProbeReflects and kProbeLevels.
static const uint kProbeBoxProjection = 1;
static const uint kProbeReflects = 2;
static const float kProbeLastLevel = 5.0;

// How much of the light from all around `position` takes from `probe`, before the probes
// above it have taken theirs: all of it inside the box and at least the blend distance
// from each face, falling evenly to nothing at the faces; nothing outside. Matches
// probeWeight() in renderer/EnvironmentProbes.cpp.
float probeWeight(EnvironmentProbe probe, float3 position)
{
    const float3 local = mul(probe.worldToBox, float4(position, 1.0)).xyz;
    const float3 inside = (0.5 - abs(local)) * probe.boxSize;
    const float nearest = min(inside.x, min(inside.y, inside.z));
    // Also when it is no number.
    if (!(nearest >= 0.0))
    {
        return 0.0;
    }
    return probe.blendDistance > 0.0 ? saturate(nearest / probe.blendDistance) : 1.0;
}

// What a white, matte surface that faces `normal` gives off under the picture of `probe`.
float3 probeDiffuseLight(EnvironmentProbe probe, float3 normal)
{
    float basis[9];
    sphericalHarmonicsBasis(normal, basis);
    float3 light = float3(0.0);
    for (uint index = 0; index < kSphericalHarmonicsCount; ++index)
    {
        light += probeHarmonics[probe.slot * kSphericalHarmonicsCount + index].rgb * basis[index];
    }
    // Nine numbers overshoot beside a small, bright light: never less than nothing.
    return max(light, 0.0) * probe.intensity;
}

// Which way to look up what a surface at `position` reflects in the picture of `probe`,
// for a reflection that leaves it along `direction`: with box projection, towards where the
// reflection meets the box, seen from where the picture was taken - the wall the floor of a
// room reflects is where it is, not infinitely far away. The surface is inside the box.
float3 probeReflectionDirection(EnvironmentProbe probe, float3 position, float3 direction)
{
    if ((probe.flags & kProbeBoxProjection) == 0)
    {
        return direction;
    }
    // In the box's own space, where it is the cube from -0.5 to 0.5, the line leaves it
    // through the face it goes towards first. The space is the world moved, turned and
    // stretched, so how far along the line that is holds in the world as well.
    const float3 origin = mul(probe.worldToBox, float4(position, 1.0)).xyz;
    const float3 along = mul((float3x3)probe.worldToBox, direction);
    // A line that does not go along an axis never reaches its faces.
    const float3 safe = float3(abs(along.x) > 1.0e-6 ? along.x : 1.0e-6, abs(along.y) > 1.0e-6 ? along.y : 1.0e-6,
                               abs(along.z) > 1.0e-6 ? along.z : 1.0e-6);
    const float3 toFaces = (sign(safe) * 0.5 - origin) / safe;
    const float leaves = max(min(toFaces.x, min(toFaces.y, toFaces.z)), 0.0);
    return normalizeOr(position + direction * leaves - probe.capturePosition, direction);
}

// The light the picture of `probe` sends towards a surface at `position` from around
// `direction`, as a surface of the given roughness sees it.
float3 probeReflectedLight(EnvironmentProbe probe, float3 position, float3 direction, float roughness)
{
    const float3 towards = probeReflectionDirection(probe, position, direction);
    return probeRadiance.SampleLevel(environmentSampler, float4(towards, float(probe.slot)), roughness * kProbeLastLevel)
               .rgb *
           probe.intensity;
}

// For the debug view of probes (RenderDebugView::Probes): a color for each probe of the
// view, by its place among them, mixed by the shares of the probes that light `position`;
// grey for what the environment lights.
float3 probeSharesColor(float3 position)
{
    const float3 colors[8] = {float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0),
                              float3(1.0, 1.0, 0.2), float3(1.0, 0.2, 1.0), float3(0.2, 1.0, 1.0),
                              float3(1.0, 0.6, 0.2), float3(0.6, 0.2, 1.0)};
    float left = 1.0;
    float3 color = float3(0.0);
    for (uint index = 0; index < probeCount && left > 0.0; ++index)
    {
        const float share = probeWeight(environmentProbes[index], position) * left;
        color += colors[index % 8] * share;
        left -= share;
    }
    return color + float3(0.25) * left;
}

// The light leaving a surface towards the viewer that comes from all around it: scattered
// inside it, and reflected off it. Both are taken from the probes whose boxes it is in -
// each its share of what the probes above it left - and the rest from the environment.
float3 shadeEnvironment(Surface surface, float3 toViewer)
{
    const float roughness = saturate(surface.roughness);
    const float3 direction = reflectionDirection(surface.normal, toViewer, roughness);
    float left = 1.0;
    float3 diffuseLight = float3(0.0);
    float3 reflectedLight = float3(0.0);
    for (uint index = 0; index < probeCount && left > 0.0; ++index)
    {
        const EnvironmentProbe probe = environmentProbes[index];
        const float share = probeWeight(probe, surface.position) * left;
        if (share > 0.0)
        {
            diffuseLight += probeDiffuseLight(probe, surface.normal) * share;
            // One whose reflections could not be made reflects the environment.
            reflectedLight += ((probe.flags & kProbeReflects) != 0
                                   ? probeReflectedLight(probe, surface.position, direction, roughness)
                                   : environmentReflectedLight(direction, roughness)) *
                              share;
            left -= share;
        }
    }
    if (environmentIntensity > 0.0)
    {
        diffuseLight += environmentDiffuseLight(surface.normal) * left;
        reflectedLight += environmentReflectedLight(direction, roughness) * left;
    }
    const float nDotV = max(dot(surface.normal, toViewer), 1.0e-4);
    const float3 reflectance = environmentReflectance(specularColor(surface), roughness, nDotV);
    // Like the light of a sky and a ground (shadeHemisphere): both parts are dimmed
    // where the material says less of the surroundings is seen, in its crevices.
    return (diffuseLight * diffuseColor(surface) + reflectedLight * reflectance) * surface.occlusion;
}
