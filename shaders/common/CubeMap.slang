// Which direction a place on a face of a cube map stands for. The C++ side is
// cubeFaceDirection() in engine/cordite/renderer/EnvironmentProbes.h: change both.
//
// Nothing here but arithmetic: passes of every binding layout include it.

#pragma once

// The faces of a cube map, in the order of its layers: +X, -X, +Y, -Y, +Z, -Z.
static const uint kCubeFaceCount = 6;

// The direction a place on a face of a cube map stands for: `uv` is (0, 0) at the top
// left of the face. This is how a GPU finds the face and the place for a direction,
// the other way round.
float3 faceDirection(uint faceIndex, float2 uv)
{
    const float2 place = uv * 2.0 - 1.0;
    float3 direction = float3(-place.x, -place.y, -1.0);
    if (faceIndex == 0)
    {
        direction = float3(1.0, -place.y, -place.x);
    }
    else if (faceIndex == 1)
    {
        direction = float3(-1.0, -place.y, place.x);
    }
    else if (faceIndex == 2)
    {
        direction = float3(place.x, 1.0, place.y);
    }
    else if (faceIndex == 3)
    {
        direction = float3(place.x, -1.0, -place.y);
    }
    else if (faceIndex == 4)
    {
        direction = float3(place.x, -place.y, 1.0);
    }
    return normalize(direction);
}
