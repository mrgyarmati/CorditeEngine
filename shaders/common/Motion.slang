// Motion vectors: how far what a pixel shows went across the picture since the view was
// last drawn, and which way - in parts of the picture, (1, 1) being its width and height,
// x to the right and y down, from where it was to where it is
// (engine/cordite/renderer/MotionVectors.h).
//
// The base pass writes them for the surfaces it draws (passes/MeshGBuffer.slang), from
// where each vertex was and is (passes/MeshVertex.slang, compiled with MOTION). Where
// nothing was drawn the sky is seen, and motionOfSky() works its motion out.

#pragma once

#include "common/View.slang"

// Where a point of clip space is on the picture: (0, 0) at its top left, (1, 1) at its
// bottom right.
float2 pictureUvOf(float4 clip)
{
    return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}

// From where a point was - in the clip space of the view then - to where it is - in the
// clip space of the view now. A point that was behind the camera then, or is now, did
// not move as far as the picture can tell; nor did what is no number. Further than a
// whole picture is as far as anything goes.
float2 motionBetween(float4 before, float4 now)
{
    if (before.w <= 0.0 || now.w <= 0.0)
    {
        return float2(0.0, 0.0);
    }
    const float2 motion = pictureUvOf(now) - pictureUvOf(before);
    return all(isfinite(motion)) ? clamp(motion, -1.0, 1.0) : float2(0.0, 0.0);
}

// How the sky seen at `uv` moved: as the camera turned, and not as it went, for the sky is
// infinitely far away. A direction is projected as a point that far: its fourth number
// is zero, which leaves out where the camera is. Without perspective everything is seen
// from the same direction, and the sky does not move.
float2 motionOfSky(float2 uv)
{
    if (orthographic > 0.5)
    {
        return float2(0.0, 0.0);
    }
    // Where it is now is projected as well, rather than taken from `uv`: a camera that
    // did not turn then sees the sky exactly where it was.
    const float4 direction = float4(viewDirectionAt(uv), 0.0);
    return motionBetween(mul(previousViewProjection, direction), mul(viewProjection, direction));
}
