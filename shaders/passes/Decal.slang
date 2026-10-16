// Decals: materials laid onto the solid surfaces inside a box, into the G-buffer, after the
// base pass and before anything reads it (engine/cordite/renderer/DecalPass.h).
//
// Each decal is its box, drawn from inside - its back faces, without the depth test, so
// that a camera inside the box sees it too. A pixel the box covers finds the surface the
// G-buffer holds there by its depth; where that is inside the box and faces back along the
// projection, the decal's material is laid on: its color, roughness, metal and specular by
// how much of it there is, its normal map bending the surface's normal, its light added.
// Everything else is discarded - for what is true of this one pixel, as a mesh pass does.

#include "common/Decals.slang"
#include "common/GBuffer.slang"
#include "common/Math.slang"
#include "common/View.slang"
#include "materials/Material.slang"

// The order of the members is the order of the vertex attributes: the position of a
// vertex of the cube, then what differs per decal.
struct DecalVertexInput
{
    float3 position : POSITION;
    // The rows of the matrix that places the box: the world matrix of the decal, mirrored
    // back where it mirrors, so that the far faces of the box still face away.
    float4 box0 : BOX0;
    float4 box1 : BOX1;
    float4 box2 : BOX2;
    // The rows of the inverse of the decal's world matrix: from the world into its box.
    float4 decal0 : DECAL0;
    float4 decal1 : DECAL1;
    float4 decal2 : DECAL2;
    // x: how much of the decal is laid on; y and z: how far a surface has to face back
    // along the projection to get all of it, and any, as cosines; w: the alpha mode of the
    // material, 0 opaque, 1 masked, 2 blended.
    float4 parameters : PARAMETERS;
};

struct DecalVaryings
{
    float4 position : SV_Position;
    nointerpolation float4 decal0 : DECAL0;
    nointerpolation float4 decal1 : DECAL1;
    nointerpolation float4 decal2 : DECAL2;
    nointerpolation float4 parameters : PARAMETERS;
};

[shader("vertex")]
DecalVaryings decalVertex(DecalVertexInput input)
{
    const float4 local = float4(input.position, 1.0);
    const float3 world = float3(dot(input.box0, local), dot(input.box1, local), dot(input.box2, local));
    DecalVaryings output;
    output.position = mul(viewProjection, float4(world, 1.0));
    output.decal0 = input.decal0;
    output.decal1 = input.decal1;
    output.decal2 = input.decal2;
    output.parameters = input.parameters;
    return output;
}

// The G-buffer's normals as the decals before this one left them: the target cannot be
// read while it is drawn into, and its octahedral numbers cannot be blended.
Texture2D<float2> normalsBefore : register(t0, space2);
// The depth buffer of the view: reversed, nothing where nothing was drawn.
Texture2D<float> depthBuffer : register(t1, space2);

struct DecalOutput
{
    // rgb laid over the base color by alpha; the alpha of the target - the lighting
    // channels - is kept by the blend.
    float4 baseColor : SV_Target0;
    // The normal, bent and blended here: written as it is, by a decal whose material has a
    // normal map, and not at all by another.
    float2 normal : SV_Target1;
    // Roughness, metallic and specular laid over by alpha; occlusion kept by the blend.
    float4 parameters : SV_Target2;
    // Added.
    float3 emissive : SV_Target3;
};

// How much of the material is there, by its alpha mode as a mesh takes it: an opaque one
// all of it, a masked one all or nothing by its cutoff, a blended one what alpha says.
float coverageOf(float opacity, float alphaMode)
{
    if (alphaMode < 0.5)
    {
        return 1.0;
    }
    if (alphaMode < 1.5)
    {
        return opacity >= alphaCutoff ? 1.0 : 0.0;
    }
    return opacity;
}

// How far the image moves from the pixel at `uv` to the one `step` further on the screen.
// It is measured where the line of sight of that pixel meets the plane of the image
// through `position`, the point this pixel shows: what the pixel beside shows may be
// something else altogether, and the image would jump there; and at a slant the plane is
// the way to see that a pixel spans much of the image. At most the whole image - which is
// also the answer where the line runs along the plane - so that the smallest level is read.
float2 decalGradient(float2 uv, float2 step, float3 position, float2 at, float3 projection, DecalVaryings decal)
{
    const float3 origin = viewerAt(uv + step);
    const float3 direction = viewDirectionAt(uv + step);
    const float along = dot(direction, projection);
    const float distance = dot(position - origin, projection) / along;
    const float3 met = origin + direction * distance;
    if (!(abs(along) > 1.0e-6) || !(distance > 0.0) || !all(isfinite(met)))
    {
        return float2(1.0, 0.0);
    }
    const float2 gradient = decalUv(decalLocal(decal.decal0, decal.decal1, decal.decal2, met)) - at;
    const float size = length(gradient);
    return size > 1.0 ? gradient / size : gradient;
}

[shader("pixel")]
DecalOutput decalPixel(DecalVaryings input)
{
    const int2 pixel = int2(input.position.xy);
    const float depth = depthBuffer.Load(int3(pixel, 0));
    // Where nothing was drawn there is nothing to lay a decal on, and no position to work
    // out.
    if (!(depth > 0.0))
    {
        discard;
    }
    const float2 uv = (float2(pixel) + 0.5) * inverseTargetSize;
    const float3 position = worldPositionFromDepth(uv, depth, inverseViewProjection);
    const float3 local = decalLocal(input.decal0, input.decal1, input.decal2, position);
    if (!isInsideDecal(local))
    {
        discard;
    }
    const float3 projection = decalProjection(input.decal2);
    const float3 normalBefore = decodeNormal(normalsBefore.Load(int3(pixel, 0)));
    const float fade = decalFadeOf(normalBefore, projection, input.parameters.y, input.parameters.z);
    if (!(fade > 0.0))
    {
        discard;
    }
    const float2 at = decalUv(local);
    const DecalSurface surface =
        evaluateDecal(at, decalGradient(uv, float2(inverseTargetSize.x, 0.0), position, at, projection, input),
                      decalGradient(uv, float2(0.0, inverseTargetSize.y), position, at, projection, input));
    const float amount = saturate(coverageOf(surface.opacity, input.parameters.w) * input.parameters.x * fade);
    if (!(amount > 0.0))
    {
        discard;
    }

    // The normal map bends the surface's normal in a frame around it: its first axis the
    // way the image runs, across the surface; its second the way the image is up, which
    // the order of the first two decides where the decal mirrors its image.
    const float3 across = normalizeOr(input.decal0.xyz, float3(1.0, 0.0, 0.0));
    const float3 up = normalizeOr(input.decal1.xyz, float3(0.0, 1.0, 0.0));
    const float3 tangent = normalizeOr(across - normalBefore * dot(normalBefore, across), float3(0.0));
    const float3 side = cross(normalBefore, tangent);
    const float3 bitangent = dot(side, up) < 0.0 ? -side : side;
    const float3 bent = normalizeOr(tangent * surface.normal.x + bitangent * surface.normal.y +
                                        normalBefore * surface.normal.z,
                                    normalBefore);

    DecalOutput output;
    output.baseColor = float4(surface.baseColor, amount);
    output.normal = encodeNormal(normalizeOr(lerp(normalBefore, bent, amount), normalBefore));
    output.parameters = float4(surface.roughness, surface.metallic, surface.specular, amount);
    output.emissive = surface.emissive * amount;
    return output;
}
