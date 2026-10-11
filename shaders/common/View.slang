// What every pass that draws a scene knows about the view it draws: the camera, the
// target, and how the view's lights are laid out.
//
// Bound as the first binding set (space0) of those passes, together with the buffers
// of the lights (common/Lights.slang). The C++ side is ViewConstants in
// engine/cordite/renderer/ViewBindings.cpp.

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
