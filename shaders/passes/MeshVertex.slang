// The vertex stage of every pass that draws meshes.
//
// One shader for all of them, on purpose. The base pass redraws what the depth pre-pass
// drew and keeps only the pixels whose depth is exactly the one already there; that
// works when both passes compute positions with the very same code, and stops working
// the day two copies of it are optimized differently.

#include "common/MeshVaryings.slang"
#include "common/View.slang"

// The order of the members is the order of the vertex attributes: the mesh's own first,
// then what differs per instance.
struct MeshVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    // The rows of the matrix that takes the mesh into the world.
    float4 world0 : WORLD0;
    float4 world1 : WORLD1;
    float4 world2 : WORLD2;
};

[shader("vertex")]
MeshVaryings meshVertex(MeshVertexInput input)
{
    const float4 localPosition = float4(input.position, 1.0);
    const float3 row0 = input.world0.xyz;
    const float3 row1 = input.world1.xyz;
    const float3 row2 = input.world2.xyz;

    MeshVaryings output;
    output.position = float3(dot(input.world0, localPosition), dot(input.world1, localPosition),
                             dot(input.world2, localPosition));
    output.clipPosition = mul(viewProjection, float4(output.position, 1.0));

    // Normals transform with the inverse transpose, which is the matrix of cofactors
    // divided by the determinant. Only the determinant's sign matters for a direction;
    // it is negative for a mirrored instance, whose tangent frame changes handedness.
    const float3 cofactor0 = cross(row1, row2);
    const float3 cofactor1 = cross(row2, row0);
    const float3 cofactor2 = cross(row0, row1);
    const float mirrored = dot(row0, cofactor0) < 0.0 ? -1.0 : 1.0;
    output.normal = mirrored * float3(dot(cofactor0, input.normal), dot(cofactor1, input.normal),
                                      dot(cofactor2, input.normal));
    output.tangent = float4(dot(row0, input.tangent.xyz), dot(row1, input.tangent.xyz), dot(row2, input.tangent.xyz),
                            input.tangent.w * mirrored);
    output.uv0 = input.uv0;
    output.uv1 = input.uv1;
    return output;
}
