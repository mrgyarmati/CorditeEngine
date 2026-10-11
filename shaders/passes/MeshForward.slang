// Draws meshes fully shaded in one pass: every material the engine has, opaque, masked
// or blended. Output is linear light.
//
// Until the engine has lights of its own, surfaces are lit by one fixed key light and by
// ambient light from a sky above and a ground below.
//
// Compiled twice: as is, and with ALPHA_TEST for masked materials. Keeping the discard
// out of the common variant lets the GPU reject hidden pixels before shading them.

#include "materials/Material.slang"

cbuffer ViewConstants : register(b0, space0)
{
    float4x4 viewProjection;
    float3 cameraPosition;
    // 1 for a camera without perspective, which sees everything from the same direction.
    float orthographic;
    // Unit vector along which the camera looks.
    float3 cameraForward;
    float viewPadding0;
    // Unit vector from a surface towards the key light.
    float3 keyLightDirection;
    float viewPadding1;
    float3 keyLightColor;
    float viewPadding2;
    float3 skyColor;
    float viewPadding3;
    float3 groundColor;
    float viewPadding4;
};

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

struct MeshVaryings
{
    float4 clipPosition : SV_Position;
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

[shader("vertex")]
MeshVaryings meshForwardVertex(MeshVertexInput input)
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

[shader("pixel")]
float4 meshForwardPixel(MeshVaryings input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    SurfaceInput surfaceInput;
    surfaceInput.position = input.position;
    surfaceInput.normal = input.normal;
    surfaceInput.tangent = input.tangent;
    surfaceInput.uv0 = input.uv0;
    surfaceInput.uv1 = input.uv1;
    surfaceInput.isFrontFace = isFrontFace;
    const Surface surface = evaluateSurface(surfaceInput);

    const float3 toViewer =
        orthographic > 0.5 ? -cameraForward : normalizeOr(cameraPosition - surface.position, -cameraForward);
    float3 color = shadeDirect(surface, toViewer, keyLightDirection, keyLightColor);
    color += shadeHemisphere(surface, toViewer, skyColor, groundColor);
    color += surface.emissive;
    return float4(color, surface.opacity);
}
