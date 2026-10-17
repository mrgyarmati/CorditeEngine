// The vertex stage of every pass that draws meshes.
//
// One shader for all of them, on purpose. The base pass redraws what the depth pre-pass
// drew and keeps only the pixels whose depth is exactly the one already there; that
// works when both passes compute positions with the very same code, and stops working
// the day two copies of it are optimized differently.
//
// Compiled a second time with SKINNED, for meshes that are moved by joints: the same
// code, with the matrix of the joints a vertex follows put before the matrix that places
// its instance. A pass draws a skinned mesh with that version in all its steps, so what
// was said above holds for it too.

#include "common/MeshVaryings.slang"
#include "common/View.slang"

// The order of the members is the order of the vertex attributes: the mesh's own first,
// then what differs per instance, then - for a skinned mesh - how a vertex follows the
// joints.
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
    // x: the lighting channels the instance belongs to, as a number.
    // y: where the joint matrices of the instance begin among those of the view.
    // z: the level of detail it is drawn at.
    float4 parameters : PARAMETERS;
#if defined(SKINNED)
    // The joints the vertex follows, counted within its skeleton, and how much it
    // follows each: the weights add up to one.
    uint4 joints : JOINTS;
    float4 weights : WEIGHTS;
#endif
};

#if defined(SKINNED)
// The rows of a matrix that takes a vertex from where its mesh was modelled to where a
// joint has it now.
struct JointMatrix
{
    float4 row0;
    float4 row1;
    float4 row2;
};

// The joint matrices of every skinned mesh of the view, one pose after another
// (SkinPalettes).
StructuredBuffer<JointMatrix> jointMatrices : register(t0, space2);

// A row of the product of two matrices whose last rows are (0, 0, 0, 1): `row` of the
// first, times the second, given by its rows.
float4 rowTimes(float4 row, float4 second0, float4 second1, float4 second2)
{
    return row.x * second0 + row.y * second1 + row.z * second2 + float4(0.0, 0.0, 0.0, row.w);
}
#endif

[shader("vertex")]
MeshVaryings meshVertex(MeshVertexInput input)
{
    const float4 localPosition = float4(input.position, 1.0);
    float4 world0 = input.world0;
    float4 world1 = input.world1;
    float4 world2 = input.world2;

#if defined(SKINNED)
    // The matrices of the joints the vertex follows, each by its weight, are one matrix;
    // the instance's matrix times that one takes the vertex into the world. Everything
    // below then treats it like the matrix of any instance - normals included.
    const uint palette = uint(input.parameters.y + 0.5);
    float4 skin0 = float4(0.0, 0.0, 0.0, 0.0);
    float4 skin1 = float4(0.0, 0.0, 0.0, 0.0);
    float4 skin2 = float4(0.0, 0.0, 0.0, 0.0);
    for (uint slot = 0; slot < 4; ++slot)
    {
        const JointMatrix joint = jointMatrices[palette + input.joints[slot]];
        const float weight = input.weights[slot];
        skin0 += joint.row0 * weight;
        skin1 += joint.row1 * weight;
        skin2 += joint.row2 * weight;
    }
    world0 = rowTimes(input.world0, skin0, skin1, skin2);
    world1 = rowTimes(input.world1, skin0, skin1, skin2);
    world2 = rowTimes(input.world2, skin0, skin1, skin2);
#endif

    const float3 row0 = world0.xyz;
    const float3 row1 = world1.xyz;
    const float3 row2 = world2.xyz;

    MeshVaryings output;
    output.position = float3(dot(world0, localPosition), dot(world1, localPosition), dot(world2, localPosition));
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
    output.lightChannels = uint(input.parameters.x + 0.5);
    output.lod = uint(input.parameters.z + 0.5);
    return output;
}
