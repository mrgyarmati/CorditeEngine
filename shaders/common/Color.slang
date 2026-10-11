// Conversions between linear light and display-encoded color.
//
// Everything the renderer computes is linear. The conversion to sRGB happens once, in
// the pass that writes the image a display will show.

#pragma once

// The sRGB transfer function, exactly as IEC 61966-2-1 defines it.
float3 linearToSrgb(float3 color)
{
    const float3 low = color * 12.92;
    const float3 high = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
    return select(color <= 0.0031308, low, high);
}

float3 srgbToLinear(float3 color)
{
    const float3 low = color / 12.92;
    const float3 high = pow((max(color, 0.0) + 0.055) / 1.055, 2.4);
    return select(color <= 0.04045, low, high);
}

// How bright a linear color looks (Rec. 709 weights).
float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}
