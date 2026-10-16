// Light shafts: what a pixel of the sky gives to them, and what the passes of
// passes/LightShafts.slang are told.
//
// The arithmetic is written down twice: lightShaftSource() in
// engine/cordite/renderer/LightShafts.cpp, and lightShaftSourceOf() here. Tests hold the
// pictures the passes make against the first. The C++ side of the constants is
// LightShaftConstants in engine/cordite/renderer/LightShaftPass.cpp.

#pragma once

cbuffer LightShaftConstants : register(b0, space1)
{
    // The sun's color as a person set it - the color of its light times its intensity -
    // and the light of the sky beyond which the sky feeds the shafts as well.
    float3 sunColor;
    float threshold;
    // Unit vector towards the sun, and how much light the shafts add: their intensity,
    // times how much of them the view has by where the sun is (lightShaftFade()).
    float3 towardsSun;
    float intensity;
    // Where the sun is in the picture, from (0, 0) at its top left to (1, 1) at its bottom
    // right - outside that when it is out of the picture.
    float2 sunPoint;
    // The share of the way to the sun between two taps of this pass of the blur.
    float stepShare;
    float lightShaftPadding;
};

// kLightShaftConeExponent and kMaxLightShaftSource of LightShafts.h.
static const float kLightShaftConeExponent = 32.0;
static const float kMaxLightShaftSource = 4.0;

// lightShaftSource() of LightShafts.cpp: what a pixel of the sky whose light is `skyLight`,
// looking `cosToSun` towards the sun, gives to the shafts.
float3 lightShaftSourceOf(float3 skyLight, float3 sun, float cosToSun, float sunThreshold)
{
    if (!(cosToSun > 0.0))
    {
        return float3(0.0);
    }
    const float weight = pow(min(cosToSun, 1.0), kLightShaftConeExponent);
    float3 source = (max(sun, float3(0.0)) + max(skyLight - sunThreshold, float3(0.0))) * weight;
    [unroll]
    for (int channel = 0; channel < 3; ++channel)
    {
        // What is no number gives nothing; what is too bright gives the most there is.
        source[channel] = isnan(source[channel]) ? 0.0 : min(source[channel], kMaxLightShaftSource);
    }
    return source;
}
