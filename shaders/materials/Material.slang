// The engine's material: its constants, its textures, and how they become a Surface.
//
// Bound as the second binding set (space1) by every pass that draws meshes. The C++
// side is MaterialConstants in engine/cordite/renderer/RenderResources.cpp.

#pragma once

#include "common/Brdf.slang"
#include "common/Math.slang"
#include "common/MeshVaryings.slang"

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
    // The lighting channels of what is being drawn; handed on to the surface.
    uint lightChannels;
};

// Where the material's textures are read: the set of coordinates it uses, scaled and
// shifted as it says.
float2 materialUv(float2 uv0, float2 uv1)
{
    return (uvChannel == 0 ? uv0 : uv1) * uvScale + uvOffset;
}

// The material's opacity at a point, which is all a pass that only writes depth has to
// know of it: a masked material is there where this is at least `alphaCutoff`.
float evaluateOpacity(float2 uv0, float2 uv1)
{
    return baseColorTexture.Sample(materialSampler, materialUv(uv0, uv1)).a * baseColor.a;
}

// Evaluates the material at a point.
Surface evaluateSurface(SurfaceInput input)
{
    const float2 uv = materialUv(input.uv0, input.uv1);
    // The occlusion texture is laid out like the others where it uses their set of
    // coordinates - it is usually packed into one image with roughness and metallic -
    // and used as it is where it has a set of its own.
    const float2 occlusionUv =
        occlusionUvChannel == uvChannel ? uv : (occlusionUvChannel == 0 ? input.uv0 : input.uv1);

    Surface surface;
    surface.position = input.position;

    const float4 color = baseColorTexture.Sample(materialSampler, uv) * baseColor;
    surface.baseColor = color.rgb;
    surface.opacity = color.a;

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
    // The triangle's own normal: at right angles to how the position changes from one
    // pixel to the next, on the side the interpolated normal is on. The two changes are
    // brought to unit length first: up close they are fractions of a millimetre, and
    // their cross product would be too short to tell from nothing.
    const float3 outward = input.isFrontFace ? normal : -normal;
    const float3 face = normalizeOr(cross(normalizeOr(ddx(input.position), float3(0.0)),
                                          normalizeOr(ddy(input.position), float3(0.0))),
                                    outward);
    surface.geometricNormal = dot(face, outward) < 0.0 ? -face : face;

    const float occlusion = occlusionTexture.Sample(materialSampler, occlusionUv).r;
    surface.occlusion = lerp(1.0, occlusion, occlusionStrength);
    surface.emissive = emissiveTexture.Sample(materialSampler, uv).rgb * emissive;
    surface.lightChannels = input.lightChannels;
    return surface;
}

// Evaluates the material at the point a mesh pass is shading.
Surface evaluateSurface(MeshVaryings input, bool isFrontFace)
{
    SurfaceInput surfaceInput;
    surfaceInput.position = input.position;
    surfaceInput.normal = input.normal;
    surfaceInput.tangent = input.tangent;
    surfaceInput.uv0 = input.uv0;
    surfaceInput.uv1 = input.uv1;
    surfaceInput.isFrontFace = isFrontFace;
    surfaceInput.lightChannels = input.lightChannels;
    return evaluateSurface(surfaceInput);
}
