// The G-buffer: what the base pass writes down about the surface visible in each pixel,
// and how the passes after it read that back.
//
// | Target | Format       | Holds                                                 |
// |--------|--------------|-------------------------------------------------------|
// | 0      | SRGBA8       | rgb: base color (stored sRGB-encoded, read as linear); |
// |        |              | a: the lighting channels, as a number out of 255       |
// | 1      | RG16_UNORM   | the world-space normal, octahedrally mapped            |
// | 2      | RGBA8_UNORM  | r: roughness, g: metallic, b: specular, a: occlusion   |
// | 3      | R11G11B10F   | emissive light, linear and not limited to one          |
// | depth  | D32          | reversed depth; the position follows from it           |
//
// The C++ side of this table is GBufferTarget in engine/cordite/renderer/SceneTargets.h.
// Everything the lighting needs of a Surface is here; opacity is not, because only solid
// surfaces are written to the G-buffer.

#pragma once

#include "common/Brdf.slang"
#include "common/Math.slang"

// What the base pass writes: one member per target.
struct GBufferOutput
{
    float4 baseColor : SV_Target0;
    float2 normal : SV_Target1;
    float4 parameters : SV_Target2;
    float3 emissive : SV_Target3;
};

// Maps a unit vector to a point in the unit square, and back: an octahedron unfolded.
// Every direction gets nearly the same precision, which no pair of angles or of
// components gives. (Cigolle et al., "A Survey of Efficient Representations for
// Independent Unit Vectors", 2014.)
float2 encodeNormal(float3 normal)
{
    const float3 projected = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    // sign() is zero at zero, which would collapse the fold; these never are.
    const float2 signs = float2(projected.x >= 0.0 ? 1.0 : -1.0, projected.y >= 0.0 ? 1.0 : -1.0);
    const float2 folded = projected.z >= 0.0 ? projected.xy : (1.0 - abs(projected.yx)) * signs;
    return folded * 0.5 + 0.5;
}

float3 decodeNormal(float2 encoded)
{
    const float2 folded = encoded * 2.0 - 1.0;
    float3 normal = float3(folded, 1.0 - abs(folded.x) - abs(folded.y));
    const float below = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -below : below;
    normal.y += normal.y >= 0.0 ? -below : below;
    return normalizeOr(normal, float3(0.0, 1.0, 0.0));
}

GBufferOutput encodeGBuffer(Surface surface)
{
    GBufferOutput output;
    // Alpha is not sRGB-encoded: eight bits hold the eight channels exactly.
    output.baseColor = float4(surface.baseColor, float(surface.lightChannels & 0xFF) / 255.0);
    output.normal = encodeNormal(surface.normal);
    output.parameters = float4(surface.roughness, surface.metallic, surface.specular, surface.occlusion);
    output.emissive = surface.emissive;
    return output;
}

// The surface a pixel of the G-buffer describes. `position` is where the pixel's depth
// puts it in the world.
Surface decodeGBuffer(float4 baseColor, float2 normal, float4 parameters, float3 emissive, float3 position)
{
    Surface surface;
    surface.position = position;
    surface.normal = decodeNormal(normal);
    // The G-buffer does not hold the surface's own normal; a pass that needs it works
    // it out from the depth around the pixel (passes/DeferredLighting.slang).
    surface.geometricNormal = surface.normal;
    surface.baseColor = baseColor.rgb;
    surface.opacity = 1.0;
    surface.roughness = parameters.r;
    surface.metallic = parameters.g;
    surface.specular = parameters.b;
    surface.occlusion = parameters.a;
    surface.emissive = emissive;
    surface.lightChannels = uint(baseColor.a * 255.0 + 0.5);
    return surface;
}

// Where a pixel is in the world: its position on the target, from (0, 0) at the top left
// to (1, 1), and its depth, taken back through the inverse of the view-projection matrix.
float3 worldPositionFromDepth(float2 uv, float depth, float4x4 inverseViewProjection)
{
    const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    const float4 world = mul(inverseViewProjection, clip);
    return world.xyz / world.w;
}
