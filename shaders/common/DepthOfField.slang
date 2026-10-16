// Depth of field: how blurred what a pixel shows is, and what the passes of
// passes/DepthOfField.slang are told.
//
// The arithmetic is written down twice: focusBlur() in
// engine/cordite/renderer/PostProcessSettings.cpp, and focusBlurAt() here. Tests hold the
// pictures the passes make against the first. The C++ side of the constants is
// DepthOfFieldConstants in engine/cordite/renderer/DepthOfFieldPass.cpp.

#pragma once

#include "common/GBuffer.slang"
#include "common/View.slang"

cbuffer DepthOfFieldConstants : register(b0, space1)
{
    // How far from the camera what is sharp is, how far in front of that and behind it
    // everything stays sharp, and over how many metres beyond that the blur grows to its
    // most - all in metres, along the way the camera looks.
    float focusDistance;
    float focusRange;
    float focusTransition;
    // The most blur in front of what is sharp, as a share of the most there is.
    float nearBlur;

    // The same behind it.
    float farBlur;
    // The radius of the most blur there is, in texels of the half-sized images the blur
    // is worked out in.
    float blurRadius;
    // How many texels around a texel of the blur it is made of.
    uint tapCount;
    // 1 to show where the picture is blurred instead of the picture (RenderDebugView::Focus).
    float showFocus;
};

// focusBlur() of PostProcessSettings.cpp: how blurred what is `depth` metres in front of
// the camera is, as a share of the most blur - negative in front of what is sharp.
float focusBlurAt(float depth)
{
    if (isnan(depth))
    {
        return 0.0;
    }
    const float offset = depth - focusDistance;
    const float beyond = abs(offset) - focusRange;
    if (!(beyond > 0.0))
    {
        return 0.0;
    }
    const float growth = focusTransition > 0.0 ? min(beyond / focusTransition, 1.0) : 1.0;
    return offset < 0.0 ? -growth * nearBlur : growth * farBlur;
}

// How blurred the pixel at `uv` is, whose depth in the depth buffer is `depth`. Where
// nothing was drawn - a depth of nothing, as far away as can be - it is behind at the
// most; worked out, that would divide by nothing.
float focusBlurOfPixel(float2 uv, float depth)
{
    if (!(depth > 0.0))
    {
        return farBlur;
    }
    return focusBlurAt(viewDepth(worldPositionFromDepth(uv, depth, inverseViewProjection)));
}
