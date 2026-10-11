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
    float viewPadding0;
    float viewPadding1;
    float viewPadding2;
};

// The unit vector from a point in the world towards the viewer. With perspective that
// is towards the camera; without, it is the same direction everywhere.
float3 directionToViewer(float3 position)
{
    return orthographic > 0.5 ? -cameraForward : normalizeOr(cameraPosition - position, -cameraForward);
}

// How far a point is from the camera, measured along the direction it looks in.
float viewDepth(float3 position)
{
    return dot(position - cameraPosition, cameraForward);
}
