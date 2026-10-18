// Makes the cube map of an environment: the light arriving from each direction, as
// surfaces of different roughness see it.
//
// A mirror reflects the light of one direction; a rough surface reflects a blur of the
// light around that direction, the wider the rougher it is. Blurring when a pixel is
// shaded would take hundreds of reads, so it is done once, when the environment is
// first used: each level of the cube map holds the image blurred for one roughness,
// from none in level 0 to all of it in the last (B. Karis, "Real Shading in Unreal
// Engine 4", 2013).
//
// One face of one level is drawn at a time, from the image of everything around
// (equirectUv() in common/Math.slang says which direction is where in it). Compiled a
// second time with CUBE_SOURCE, for the pictures of environment probes
// (EnvironmentMaps::prefilterCube): from a cube map of everything around rather than an
// image.

#include "common/Brdf.slang"
#include "common/CubeMap.slang"
#include "common/Fullscreen.slang"
#include "common/Math.slang"

cbuffer EnvironmentPrefilterConstants : register(b0, space0)
{
    // Which face of the cube is drawn: +X, -X, +Y, -Y, +Z, -Z.
    uint face;
    // The roughness this level is for: 0 to 1.
    float roughness;
    // How much smaller a level of the image is read than its own: the logarithm, to the
    // base two, of how many of its pixels fall into one texel of the face being drawn.
    float sourceLevel;
    // How many pixels the image has for each part of the sphere, on average: its number
    // of pixels divided by four pi.
    float sourceDensity;
    // How often the image is read for one texel of a blurred level.
    uint sampleCount;
    float prefilterPadding0;
    float prefilterPadding1;
    float prefilterPadding2;
};

#if defined(CUBE_SOURCE)
// A cube map of everything around, with all its smaller levels.
TextureCube<float4> source : register(t0, space0);
#else
// The image of everything around, with all its smaller levels.
Texture2D<float4> source : register(t0, space0);
#endif
SamplerState sourceSampler : register(s0, space0);

// The light of everything around from `direction`, from level `level` of the source.
float3 sourceLight(float3 direction, float level)
{
#if defined(CUBE_SOURCE)
    return source.SampleLevel(sourceSampler, direction, level).rgb;
#else
    return source.SampleLevel(sourceSampler, equirectUv(direction), level).rgb;
#endif
}

// The `index`-th of `count` points spread evenly over a square (Hammersley).
float2 pointInSquare(uint index, uint count)
{
    return float2(float(index) / float(count), float(reversebits(index)) * (1.0 / 4294967296.0));
}

// A direction around `normal`, picked so that directions are as frequent as a surface
// of roughness `alpha` (squared already) has facets facing them.
float3 facetDirection(float2 point, float3 normal, float alpha)
{
    const float around = 2.0 * kPi * point.x;
    const float cosine = sqrt((1.0 - point.y) / (1.0 + (alpha * alpha - 1.0) * point.y));
    const float sine = sqrt(saturate(1.0 - cosine * cosine));
    // Around +Z, then turned to stand around the normal.
    const float3 local = float3(sine * cos(around), sine * sin(around), cosine);
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

[shader("pixel")]
float4 environmentPrefilterPixel(FullscreenVaryings input) : SV_Target0
{
    const float3 normal = faceDirection(face, input.uv);
    if (roughness <= 0.0)
    {
        // A mirror: the light of the one direction, from the level of the image whose
        // pixels are as large as this texel.
        return float4(sourceLight(normal, max(sourceLevel, 0.0)), 1.0);
    }

    // Seen and lit along the normal: what the blur looks like from straight on, which
    // is used for every angle.
    const float alpha = max(roughness * roughness, 1.0e-3);
    float3 light = float3(0.0);
    float weights = 0.0;
    for (uint index = 0; index < sampleCount; ++index)
    {
        const float3 facet = facetDirection(pointInSquare(index, sampleCount), normal, alpha);
        const float3 towards = 2.0 * dot(normal, facet) * facet - normal;
        const float nDotL = dot(normal, towards);
        if (nDotL <= 0.0)
        {
            continue;
        }
        // Each read stands for a part of the sphere - a large one where facets are
        // rare - and is taken from the level of the image whose pixels are that large:
        // a few reads then do what would take thousands (J. Krivanek and M. Colbert,
        // "Real-time Shading with Filtered Importance Sampling", 2008). The price is
        // a blur on top of the one asked for, the wider the fewer reads there are.
        const float nDotH = saturate(dot(normal, facet));
        const float density = distributionGgx(nDotH, alpha) * 0.25;
        const float covered = 1.0 / (float(sampleCount) * max(density, 1.0e-6));
        const float level = max(0.5 * log2(covered * sourceDensity) + 1.0, 0.0);
        light += sourceLight(towards, level) * nDotL;
        weights += nDotL;
    }
    return float4(light / max(weights, 1.0e-5), 1.0);
}
