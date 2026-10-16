// Decals: where a point is in a decal, and how much of the decal a surface takes by the way
// it faces.
//
// The arithmetic is written down twice: decalPoint() and decalFade() in
// engine/cordite/renderer/Decals.cpp, and here. Tests hold the pictures the decal pass
// makes against the first.

#pragma once

#include "common/Math.slang"

// decalPoint() of Decals.cpp: where a point the rows `decal0` to `decal2` of the inverse of
// the decal's world matrix take into the decal's box is - (u, v) on its image, and whether
// it is inside the box at all.
float3 decalLocal(float4 decal0, float4 decal1, float4 decal2, float3 position)
{
    const float4 point = float4(position, 1.0);
    return float3(dot(decal0, point), dot(decal1, point), dot(decal2, point));
}

bool isInsideDecal(float3 local)
{
    // A point that is no number is no point inside.
    return abs(local.x) <= 0.5 && abs(local.y) <= 0.5 && abs(local.z) <= 0.5;
}

float2 decalUv(float3 local)
{
    return float2(local.x + 0.5, 0.5 - local.y);
}

// The `projection` of SceneDecal in Decals.cpp: the unit vector along which a decal is
// projected, the way its depth in the box grows - from the row `decal2` of the inverse of
// its world matrix, which is how much that depth changes in each direction of the world.
float3 decalProjection(float4 decal2)
{
    return -normalizeOr(decal2.xyz, float3(0.0, 0.0, 1.0));
}

// decalFade() of Decals.cpp, with the cosines of its two angles worked out once a decal:
// all of the decal where the surface faces back along the projection at least as much as
// `fullFacing`, none where it does no more than `noFacing`, a smooth step between.
float decalFadeOf(float3 surfaceNormal, float3 projection, float fullFacing, float noFacing)
{
    const float facing = -dot(surfaceNormal, projection);
    const float t = saturate((facing - noFacing) / (fullFacing - noFacing));
    return isnan(t) ? 0.0 : t * t * (3.0 - 2.0 * t);
}
