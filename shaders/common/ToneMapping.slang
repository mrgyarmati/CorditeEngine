// Tone mapping and grading: from light without an upper limit to the colors of a display.
//
// The curves and the arithmetic are written down a second time in
// engine/cordite/renderer/PostProcessSettings.cpp, which is what tests hold the
// pictures against. Change both.

#pragma once

#include "common/Color.slang"

// Match ToneMapper in PostProcessSettings.h.
static const uint kToneMapperFilmic = 0;
static const uint kToneMapperAces = 1;
static const uint kToneMapperLinear = 2;

// The grey a tone mapper leaves where it is, and contrast turns about.
static const float kMidGrey = 0.18;

// Light is multiplied by these before it goes through the curves, so that mid-grey
// comes out as mid-grey.
static const float kFilmicScale = 2.9275;
static const float kAcesScale = 0.7232;
// What the filmic curve takes for white, after the scale.
static const float kFilmicWhite = 11.2;

// J. Hable's curve ("Filmic Tonemapping Operators", 2010), before it is scaled to end
// at white.
float3 hable(float3 x)
{
    const float shoulder = 0.15;
    const float linearPart = 0.50;
    const float linearAngle = 0.10;
    const float toe = 0.20;
    const float toeNumerator = 0.02;
    const float toeDenominator = 0.30;
    return (x * (shoulder * x + linearAngle * linearPart) + toe * toeNumerator) /
               (x * (shoulder * x + linearPart) + toe * toeDenominator) -
           toeNumerator / toeDenominator;
}

float3 toneMapFilmic(float3 light)
{
    return hable(light * kFilmicScale) / hable(float3(kFilmicWhite)).x;
}

// K. Narkowicz's fit of the ACES reference rendering and output transforms ("ACES
// Filmic Tone Mapping Curve", 2016).
float3 toneMapAces(float3 light)
{
    const float3 x = light * kAcesScale;
    return (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
}

// What a tone mapper makes of light: linear, between 0 and 1.
float3 applyToneMapper(uint toneMapper, float3 light)
{
    const float3 positive = max(light, 0.0);
    float3 shown = positive;
    if (toneMapper == kToneMapperFilmic)
    {
        shown = toneMapFilmic(positive);
    }
    else if (toneMapper == kToneMapperAces)
    {
        shown = toneMapAces(positive);
    }
    return saturate(shown);
}

// Saturation, then contrast about mid-grey, then lift, gamma and gain, on a color of
// the tone-mapped picture: linear, between 0 and 1. `inverseGamma` is one divided by the
// gamma of each color.
float3 applyGrading(float3 color, float saturation, float contrast, float3 lift, float3 inverseGamma, float3 gain)
{
    float3 graded = max(color, 0.0);
    graded = max(lerp(float3(luminance(graded)), graded, saturation), 0.0);
    // Powers of what is not negative, to exponents that are positive: those are defined,
    // zero included.
    graded = kMidGrey * pow(graded / kMidGrey, contrast);
    graded = pow(max(gain * (graded + lift * (1.0 - graded)), 0.0), inverseGamma);
    return saturate(graded);
}

// How much of the light a vignette leaves at a place of the picture: `fromCentre` is -1
// to 1 across it in both directions.
float vignetteScale(float vignette, float2 fromCentre)
{
    return 1.0 - vignette * min(dot(fromCentre, fromCentre) * 0.5, 1.0);
}
