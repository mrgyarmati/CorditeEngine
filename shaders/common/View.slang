// What every pass that draws a scene knows about the view it draws: the camera, the
// target, and how the view's lights and shadows are laid out.
//
// Bound as the first binding set (space0) of those passes, together with the buffers
// of the lights (common/Lights.slang) and what their shadows need
// (common/Shadows.slang). The C++ side is engine/cordite/renderer/ViewConstants.h.

#pragma once

#include "common/Math.slang"

cbuffer ViewConstants : register(b0, space0)
{
    // The world to clip space, and back: passes that work from the depth buffer find
    // out with the second where a pixel is in the world.
    float4x4 viewProjection;
    float4x4 inverseViewProjection;
    float3 cameraPosition;
    // 1 for a camera without perspective, which sees everything from the same direction.
    float orthographic;
    // Unit vector along which the camera looks.
    float3 cameraForward;
    // How many directional lights are at the front of the light buffer.
    uint directionalLightCount;
    // Ambient light arriving from above and from below.
    float3 skyColor;
    // How a depth becomes a slice of the cluster grid: log2(depth) * scale + bias with
    // perspective, depth * scale + bias without.
    float clusterDepthScale;
    float3 groundColor;
    float clusterDepthBias;
    // The size of the target in pixels, and one divided by it.
    float2 targetSize;
    float2 inverseTargetSize;
    // The shadow of the sun (common/Shadows.slang): where each of its cascades ends,
    // in metres from the camera, how many there are, and where the shadow starts to
    // fade out and is gone.
    float4 cascadeEnds;
    uint cascadeCount;
    float shadowFadeStart;
    float shadowFadeEnd;
    // How many shadow maps the lights of the view have between them: zero when nothing
    // in it casts a shadow, and then nothing has to get ready to look one up.
    uint shadowViewCount;
    // How far the filter that softens the edges of shadows reaches, in texels.
    uint shadowFilterRadius;
    // The environment (common/Environment.slang): how bright it is - zero when there
    // is none -, the last level of its cube map, and 1 when its sky is drawn where
    // nothing else is.
    float environmentIntensity;
    float environmentLastLevel;
    float environmentVisible;
    // The sine and the cosine of the angle it is turned by about the vertical axis.
    float2 environmentTurn;
    // 1 when the ambient occlusion of the view was drawn this frame
    // (passes/AmbientOcclusion.slang) and the lighting is to read it.
    float screenOcclusion;
    float viewPadding0;
    // What it lights a matte surface with: nine spherical harmonics, in rgb.
    float4 environmentDiffuse[9];
    // The fog (common/Fog.slang): its own color, and the part of the light that is
    // lost with every metre where it is densest - zero when the view has no fog.
    float3 fogColor;
    float fogDensity;
    // What it adds of the sun's light where the view looks straight at the sun, and how
    // closely around the sun that stays: the power of the cosine of the angle to it.
    float3 fogSunColor;
    float fogSunExponent;
    // Unit vector towards that sun.
    float3 fogTowardsSun;
    // The height up to which the fog is at its densest, and how fast it thins out
    // above: by a factor of e every one divided by this many metres.
    float fogHeight;
    float fogHeightFalloff;
    // How far from the viewer the fog begins, and the most of a pixel it ever takes.
    float fogStartDistance;
    float fogMaxOpacity;
    float viewPadding1;
    // What the targets were cleared to: what is seen where nothing is drawn and the
    // environment is not shown as the sky.
    float3 backgroundColor;
    float viewPadding2;
};

// The unit vector from a point in the world towards the viewer. With perspective that
// is towards the camera; without, it is the same direction everywhere.
float3 directionToViewer(float3 position)
{
    return orthographic > 0.5 ? -cameraForward : normalizeOr(cameraPosition - position, -cameraForward);
}

// The unit vector along which the pixel at `uv` - (0, 0) at the top left of the picture,
// (1, 1) at its bottom right - looks out into the world. With perspective that is away
// from the camera, through the pixel; without, it is the same direction everywhere.
float3 viewDirectionAt(float2 uv)
{
    if (orthographic > 0.5)
    {
        return cameraForward;
    }
    // The far end of what the pixel shows, as a point that may be infinitely far away:
    // its fourth number is zero then, and the other three are the direction. Taking
    // the camera away like this is exact for both, and stays so far from the origin,
    // where the difference of two points near the camera would not.
    const float4 farPoint = mul(inverseViewProjection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0));
    return normalizeOr(farPoint.xyz - cameraPosition * farPoint.w, cameraForward);
}

// How far a point is from the camera, measured along the direction it looks in.
float viewDepth(float3 position)
{
    return dot(position - cameraPosition, cameraForward);
}

// Where the line of sight to a point in the world sets out. With perspective that is
// the camera. Without, every line of sight has a start of its own, in the plane the
// camera is in: the one straight in front of the point.
float3 viewerOf(float3 position)
{
    return orthographic > 0.5 ? position - cameraForward * viewDepth(position) : cameraPosition;
}

// The same for the pixel at `uv`, whatever it shows: where viewDirectionAt(uv) sets out.
float3 viewerAt(float2 uv)
{
    if (orthographic < 0.5)
    {
        return cameraPosition;
    }
    // Any point the pixel shows will do; this one is on the near plane, where depth is one.
    const float4 shown = mul(inverseViewProjection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0));
    return viewerOf(shown.xyz / shown.w);
}
