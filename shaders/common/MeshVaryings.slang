// What the vertex stage of the mesh passes hands to their pixel stages.
//
// Every pass that draws meshes shares one vertex shader (passes/MeshVertex.slang), so
// every pixel shader of those passes takes a MeshVaryings.

#pragma once

struct MeshVaryings
{
    float4 clipPosition : SV_Position;
    // In the world.
    float3 position : POSITION0;
    // Not unit length: interpolation shortens them.
    float3 normal : NORMAL0;
    // xyz along increasing u; w is +1 or -1 and gives the bitangent its direction.
    float4 tangent : TANGENT0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    // The lighting channels of the instance, one bit each; the same all over it.
    nointerpolation uint lightChannels : LIGHTCHANNELS;
};
