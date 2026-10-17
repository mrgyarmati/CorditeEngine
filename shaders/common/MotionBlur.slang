// The arithmetic of motion blur: the reconstruction filter of M. McGuire, P. Hennessy,
// M. Bukowski and B. Osman ("A Reconstruction Filter for Plausible Motion Blur", 2012).
// Written down twice: engine/cordite/renderer/MotionBlur.h says what each function is
// for, and tests hold the pictures of passes/MotionBlur.slang against it.

#pragma once

// How far a pixel is blurred at the most, either way along its motion: one part in this
// many of the height of the picture.
static const uint kMotionBlurRadiusDivisor = 40;

// How much nearer one point has to be than another to be wholly in front of it, as a
// share of the depth of the nearer.
static const float kMotionBlurDepthSoftness = 0.02;

uint motionBlurRadius(uint height)
{
    return max(1u, (height + kMotionBlurRadiusDivisor - 1u) / kMotionBlurRadiusDivisor);
}

float2 blurOf(float2 motion, float2 size, float amount, float radius)
{
    const float2 blur = motion * size * (amount * 0.5);
    const float blurLength = length(blur);
    if (!(blurLength > 0.0) || !isfinite(blurLength))
    {
        return float2(0.0, 0.0);
    }
    return blurLength > radius ? blur * (radius / blurLength) : blur;
}

float inFrontOf(float depth, float otherDepth)
{
    const float softness = max(min(depth, otherDepth) * kMotionBlurDepthSoftness, 1.0e-4);
    return saturate(1.0 - (depth - otherDepth) / softness);
}

float motionCone(float distance, float speed)
{
    return saturate(1.0 - distance / speed);
}

float motionCylinder(float distance, float speed)
{
    return 1.0 - smoothstep(0.95 * speed, 1.05 * speed, distance);
}

// motionBlurWeight() with MotionBlurPoint spelt out: how far the sample is, and how long
// the blurs of the sample and of the pixel are and how far away each is.
float motionBlurWeight(float distance, float sampleSpeed, float sampleDepth, float pixelSpeed, float pixelDepth)
{
    return inFrontOf(sampleDepth, pixelDepth) * motionCone(distance, sampleSpeed) +
           inFrontOf(pixelDepth, sampleDepth) * motionCone(distance, pixelSpeed) +
           motionCylinder(distance, sampleSpeed) * motionCylinder(distance, pixelSpeed) * 2.0;
}

float motionBlurNoise(uint2 pixel)
{
    const float first = 0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y);
    const float second = 52.9829189 * frac(first);
    return frac(second);
}

float motionBlurSampleAt(uint index, uint count, uint2 pixel)
{
    const float jitter = motionBlurNoise(pixel) - 0.5;
    return lerp(-1.0, 1.0, (float(index) + jitter + 1.0) / (float(count) + 1.0));
}
