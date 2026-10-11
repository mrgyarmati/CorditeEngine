// The engine's material: its constants, its textures, and how they become a Surface.
//
// Bound as the second binding set (space1) by every pass that draws meshes. The C++
// side is MaterialConstants in engine/cordite/renderer/RenderResources.cpp.

#pragma once

#include "common/Brdf.slang"
#include "common/Math.slang"

cbuffer MaterialConstants : register(b0, space1)
{
    float4 baseColor;
    // Color times intensity.
    float3 emissive;
    float alphaCutoff;
    float metallic;
    float roughness;
    float specular;
    float normalScale;
    float2 uvScale;
    float2 uvOffset;
    float occlusionStrength;
    // Which set of texture coordinates the textures use: 0 or 1.
    uint uvChannel;
    uint occlusionUvChannel;
    float materialPadding;
};

// Color textures are sRGB and arrive here linear; the others hold numbers.
Texture2D<float4> baseColorTexture : register(t0, space1);
// Roughness in green, metallic in blue.
Texture2D<float4> metallicRoughnessTexture : register(t1, space1);
// Tangent space, +Y up.
Texture2D<float4> normalTexture : register(t2, space1);
// Occlusion in red.
Texture2D<float4> occlusionTexture : register(t3, space1);
Texture2D<float4> emissiveTexture : register(t4, space1);
SamplerState materialSampler : register(s0, space1);

// What a pixel shader knows about the point it shades, before the material is applied.
struct SurfaceInput
{
    float3 position;
    // The geometric frame, as interpolated: not yet unit length.
    float3 normal;
    // xyz along increasing u; w is +1 or -1 and gives the bitangent its direction.
    float4 tangent;
    float2 uv0;
    float2 uv1;
    bool isFrontFace;
};

// Evaluates the material at a point. With ALPHA_TEST defined, pixels whose opacity is
// below the cutoff are discarded.
Surface evaluateSurface(SurfaceInput input)
{
    const float2 uv = (uvChannel == 0 ? input.uv0 : input.uv1) * uvScale + uvOffset;
    const float2 occlusionUv = occlusionUvChannel == 0 ? input.uv0 : input.uv1;

    Surface surface;
    surface.position = input.position;

    const float4 color = baseColorTexture.Sample(materialSampler, uv) * baseColor;
    surface.baseColor = color.rgb;
    surface.opacity = color.a;
#if defined(ALPHA_TEST)
    if (surface.opacity < alphaCutoff)
    {
        discard;
    }
#endif

    const float4 metallicRoughness = metallicRoughnessTexture.Sample(materialSampler, uv);
    surface.metallic = saturate(metallicRoughness.b * metallic);
    surface.roughness = saturate(metallicRoughness.g * roughness);
    surface.specular = specular;

    // The normal map bends the interpolated normal within the tangent frame. The
    // bitangent is rebuilt here rather than interpolated, as MikkTSpace prescribes.
    // Interpolation can leave a normal or a tangent without length where the frames of
    // a triangle's corners contradict each other; the surface normal then stands in.
    const float3 normal = normalizeOr(input.normal, float3(0.0, 1.0, 0.0));
    const float3 tangent = normalizeOr(input.tangent.xyz - normal * dot(normal, input.tangent.xyz), float3(0.0));
    const float3 bitangent = cross(normal, tangent) * input.tangent.w;
    float3 mapped = normalTexture.Sample(materialSampler, uv).xyz * 2.0 - 1.0;
    mapped.xy *= normalScale;
    surface.normal = normalizeOr(tangent * mapped.x + bitangent * mapped.y + normal * mapped.z, normal);
    // The back of a double-sided surface faces the other way.
    if (!input.isFrontFace)
    {
        surface.normal = -surface.normal;
    }

    const float occlusion = occlusionTexture.Sample(materialSampler, occlusionUv).r;
    surface.occlusion = lerp(1.0, occlusion, occlusionStrength);
    surface.emissive = emissiveTexture.Sample(materialSampler, uv).rgb * emissive;
    return surface;
}
