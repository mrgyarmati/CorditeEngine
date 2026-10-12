// Ambient occlusion from the screen: what its two passes share.
//
// How much of the light from all around reaches a point is measured in slices: half
// planes that all contain the line from the point to the viewer. In each, the depth
// buffer is walked both ways from the point; the steepest thing met either way is the
// horizon of that side, and what comes in between the two horizons has a closed form
// (J. Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion",
// 2016). What is met is measured by how far it rises above the plane of the surface,
// in the slice it really is in: the middle of a pixel is seldom exactly in the slice
// that was walked. passes/AmbientOcclusion.slang does that for a share of the pixels;
// passes/AmbientOcclusionResolve.slang makes a value for every pixel of what it finds.
//
// The arithmetic of the slices is written down a second time in
// engine/cordite/renderer/AmbientOcclusion.cpp, which tests hold against cases with a
// known answer. All angles are in radians and measured in a slice from the direction to
// the viewer: positive towards the way the slice runs across the screen.

#pragma once

#include "common/Math.slang"

static const float kHalfPi = 0.5 * kPi;

// The depth written down for a pixel in which nothing is: further than anything.
static const float kNowhere = 65504.0;

// Bound as space1 by both passes, next to the view's space0.
cbuffer AmbientOcclusionConstants : register(b0, space1)
{
    // How far from a point things are looked for that stand in its light, in metres.
    float radius;
    // 0 is none, 1 as the geometry says, more than that exaggerated: a power.
    float intensity;
    // How far something has to rise above a surface before it counts.
    float bias;
    // A texel of what the first pass draws stands for this many pixels, each way.
    uint divisor;
    // In how many directions a pixel looks, and how often along each, either way.
    uint sliceCount;
    uint stepCount;
    float occlusionPadding0;
    float occlusionPadding1;
};

// A surface normal as a slice sees it: the angle of its shadow on the slice, and how
// long that shadow is - 1 when the normal lies in the slice, 0 at a right angle to it.
struct SliceNormal
{
    float angle;
    float length;
};

// Matches projectIntoSlice() in AmbientOcclusion.cpp. `sliceDirection` is the way the
// slice runs across the screen, in the world; it need not have unit length.
SliceNormal projectIntoSlice(float3 normal, float3 toViewer, float3 sliceDirection)
{
    const float3 axis = normalizeOr(cross(sliceDirection, toViewer), float3(0.0));
    const float3 across = sliceDirection - toViewer * dot(sliceDirection, toViewer);
    const float3 shadow = normal - axis * dot(normal, axis);
    SliceNormal seen;
    seen.length = length(shadow);
    seen.angle = 0.0;
    if (seen.length > 1.0e-6)
    {
        const float angle = acos(clamp(dot(shadow, toViewer) / seen.length, -1.0, 1.0));
        seen.angle = dot(across, shadow) < 0.0 ? -angle : angle;
    }
    else
    {
        seen.length = 0.0;
    }
    return seen;
}

// The light between the direction to the viewer and a horizon: each direction weighted
// by the cosine of its angle to the normal, and by how much of the half sphere a step of
// the angle sweeps there.
float sliceArc(float normalAngle, float horizon)
{
    return (cos(normalAngle) + 2.0 * horizon * sin(normalAngle) - cos(2.0 * horizon - normalAngle)) * 0.25;
}

// Matches sliceVisibility() in AmbientOcclusion.cpp: `horizonBefore` is zero or less,
// `horizonAfter` zero or more.
float sliceVisibility(float normalAngle, float horizonBefore, float horizonAfter)
{
    // Further round than a right angle from the normal, the surface itself is in the way.
    const float before = normalAngle + clamp(horizonBefore - normalAngle, -kHalfPi, kHalfPi);
    const float after = normalAngle + clamp(horizonAfter - normalAngle, -kHalfPi, kHalfPi);
    return sliceArc(normalAngle, before) + sliceArc(normalAngle, after);
}

// Matches openSliceVisibility(): what a slice with nothing in the way lets through.
float openSliceVisibility(float normalAngle)
{
    return cos(normalAngle) + normalAngle * sin(normalAngle);
}

// Matches riseAboveSurface(): how far something in the unit direction `towards` rises
// above the plane of the surface, as an angle in the slice that it and the direction
// to the viewer make. Nothing along the surface, less than nothing below it.
float riseAboveSurface(float3 towards, float3 normal, float3 toViewer)
{
    const float alongNormal = dot(towards, normal);
    const float alongView = dot(towards, toViewer);
    const float sideways = sqrt(max(1.0 - alongView * alongView, 0.0));
    const float sine = alongNormal * sideways;
    const float cosine = dot(normal, toViewer) - alongView * alongNormal;
    // The arc tangent of nothing over nothing is no number on some GPUs.
    return abs(sine) + abs(cosine) > 1.0e-12 ? atan2(sine, cosine) : 0.0;
}
