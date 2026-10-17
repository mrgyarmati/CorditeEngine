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
    // The level of detail the instance is drawn at: 0 for the mesh itself.
    nointerpolation uint lod : LODLEVEL;
#if defined(MOTION)
    // Where the point is in the clip space of the view, and where it was in the clip space
    // of the view as that was last drawn (common/Motion.slang). The first is not
    // SV_Position: interpolated alike, the two come out the same where nothing moved.
    float4 currentClipPosition : CURRENTPOSITION;
    float4 previousClipPosition : PREVIOUSPOSITION;
#endif
};
