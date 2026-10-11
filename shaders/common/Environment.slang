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

// The nine spherical harmonics at a unit direction. Matches sphericalHarmonicsBasis()
// in engine/cordite/math/SphericalHarmonics.cpp.
void sphericalHarmonicsBasis(float3 direction, out float basis[9])
{
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;
    basis[0] = 0.282095;
    basis[1] = 0.488603 * y;
    basis[2] = 0.488603 * z;
    basis[3] = 0.488603 * x;
    basis[4] = 1.092548 * x * y;
    basis[5] = 1.092548 * y * z;
    basis[6] = 0.315392 * (3.0 * z * z - 1.0);
    basis[7] = 1.092548 * x * z;
    basis[8] = 0.546274 * (x * x - y * y);
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

// The light leaving a surface towards the viewer that comes from the environment:
// scattered inside it, and reflected off it.
float3 shadeEnvironment(Surface surface, float3 toViewer)
{
    if (environmentIntensity <= 0.0)
    {
        return float3(0.0);
    }
    const float nDotV = max(dot(surface.normal, toViewer), 1.0e-4);
    const float roughness = saturate(surface.roughness);
    const float3 reflectance = environmentReflectance(specularColor(surface), roughness, nDotV);
    const float3 diffuse = environmentDiffuseLight(surface.normal) * diffuseColor(surface);
    const float3 reflected =
        environmentReflectedLight(reflectionDirection(surface.normal, toViewer, roughness), roughness);
    // Like the light of a sky and a ground (shadeHemisphere): both parts are dimmed
    // where the material says less of the surroundings is seen, in its crevices.
    return (diffuse + reflected * reflectance) * surface.occlusion;
}
