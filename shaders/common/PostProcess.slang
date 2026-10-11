// What the passes that finish the picture of a view are told: the numbers of a
// PostProcessSettings, made ready for the shaders.
//
// One buffer for all of them - bloom, exposure and the final pass - written once for
// every view. The C++ side is PostProcessConstants in
// engine/cordite/renderer/PostProcessPass.cpp.

#pragma once

cbuffer PostProcessConstants : register(b0, space0)
{
    // What the light of the scene is multiplied by: two to the power of the exposure in
    // stops. With automatic exposure the stops in the exposure texture come on top.
    float exposureScale;
    // 1 with automatic exposure, 0 without.
    float autoExposure;
    // How much of the bloom is added to the picture.
    float bloomIntensity;
    // A kToneMapper constant of common/ToneMapping.slang.
    uint toneMapper;

    // What white balance multiplies red, green and blue by.
    float3 whiteBalance;
    float contrast;

    float3 lift;
    float saturation;

    // One divided by the gamma of each color.
    float3 inverseGamma;
    // How dark the corners get: 0 not at all, 1 black.
    float vignette;

    float3 gain;
    // How far red and blue part in the corners, as a share of the picture's size.
    float chromaticAberration;

    // How much the grain changes a color at the most.
    float filmGrain;
    // Changes with time, so that the grain does.
    uint grainSeed;
    // How much of the lookup table's color is taken, and how many slices the table has:
    // 0 when there is none.
    float colorLutIntensity;
    float colorLutSize;

    // Light brighter than this blooms.
    float bloomThreshold;
    // How much of each coarser level of the bloom goes into the finer one.
    float bloomScatter;
    // 1 to break up the steps between the 256 levels of a display with noise.
    float dither;
    // 1 when the view has no exposure of its own yet and takes what it measures.
    float instantAdaptation;

    // Automatic exposure: the fewest and the most stops, and the share of the way to a
    // new exposure that is covered in this frame - towards a brighter picture and
    // towards a darker one.
    float exposureMinimum;
    float exposureMaximum;
    float adaptationUp;
    float adaptationDown;
};

// The logarithm of a brightness, as the bloom chain carries it in alpha for the
// exposure to be measured from: never of nothing, which has none.
float logLuminance(float brightness)
{
    return log2(max(brightness, 1.0e-5));
}
