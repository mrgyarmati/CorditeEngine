// The first pass of the fog's light (VolumetricFogPass): what the lights throw towards the
// viewer at the middle of each box of the grid, per unit of the light the fog scatters
// there - with their shadows and their cookies - into the volume at u0 of space1. The
// view's own bindings are space0.
//
// What follows from it is VolumetricFog.h's F: the second pass (passes/FogIntegration.slang)
// weighs it by how much fog each box holds and adds it up along the columns.

#include "common/VolumetricFog.slang"

[format("rgba16f")] RWTexture3D<float4> fogScatteringOut : register(u0, space1);

// What a half float holds at the most.
static const float kMostFogLight = 65504.0;

[numthreads(4, 4, 4)]
void fogScatteringCompute(uint3 box : SV_DispatchThreadID)
{
    if (any(float3(box) >= fogVolumeSize))
    {
        return;
    }
    const float2 uv = fogCellMiddle(box.xy);
    const FogColumn column = fogColumnAt(uv);
    const float depth = fogSliceDepth((float(box.z) + 0.5) / fogVolumeSize.z);
    const float3 position = column.viewer + column.direction * (depth / column.cosine);
    const float3 toViewer = -column.direction;
    // The cookies are read at the level of how wide a box is there.
    const float footprint = pixelFootprint(position) * targetSize.x / fogVolumeSize.x;

    float3 light = float3(0.0);
    for (uint index = 0; index < directionalLightCount; ++index)
    {
        const Light sun = lights[index];
        const float3 reaching = lightInFog(sun, position, toViewer);
        if (any(reaching > 0.0))
        {
            light += reaching * sunVisibilityAt(sun.shadowView, position) * cookieFilterAt(sun, position, footprint);
        }
    }
    const ClusterRange range = clusterGrid[clusterIndexOf(uv, depth)];
    for (uint entry = 0; entry < range.count; ++entry)
    {
        const Light local = lights[lightIndices[range.offset + entry]];
        const float3 reaching = lightInFog(local, position, toViewer);
        if (any(reaching > 0.0))
        {
            // A rect light's shadows are those of a point light in its middle.
            const bool isPointLight = lightType(local) != kLightSpot;
            light += reaching * localLightVisibilityAt(local.shadowView, isPointLight, local.position, position) *
                     cookieFilterAt(local, position, footprint);
        }
    }
    fogScatteringOut[box] = float4(min(light, float3(kMostFogLight)), 1.0);
}
