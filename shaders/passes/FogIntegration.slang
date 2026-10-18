// The second pass of the fog's light (VolumetricFogPass): along each column of the grid,
// from the camera out, the sum of what the boxes throw towards the viewer (from
// passes/FogScattering.slang, at t0 of space1), each weighed by how much the fog's amount
// grows across it - the share of what is behind that the fog in the box takes away, its
// start distance and its largest opacity included (common/Fog.slang). The sum at the end
// of each box goes to the volume at u0 of space1, which the lighting reads.
//
// Its bindings in space0 are the view's constants alone: the view's own set holds the
// volume this pass writes.

#include "common/Fog.slang"
#include "common/FogVolume.slang"

Texture3D<float4> fogScatteringIn : register(t0, space1);
[format("rgba16f")] RWTexture3D<float4> fogVolumeOut : register(u0, space1);

// What a half float holds at the most.
static const float kMostFogLight = 65504.0;

[numthreads(8, 8, 1)]
void fogIntegrationCompute(uint3 cell : SV_DispatchThreadID)
{
    if (any(float2(cell.xy) >= fogVolumeSize.xy))
    {
        return;
    }
    const FogColumn column = fogColumnAt(fogCellMiddle(cell.xy));
    const uint slices = uint(fogVolumeSize.z);
    float amountBefore = 0.0;
    float3 sum = float3(0.0);
    for (uint slice = 0; slice < slices; ++slice)
    {
        const float end = fogSliceDepth(float(slice + 1) / float(slices)) / column.cosine;
        const float amount = fogAmount(column.viewer, column.viewer + column.direction * end);
        const float3 thrown = fogScatteringIn.Load(int4(int2(cell.xy), int(slice), 0)).rgb;
        sum += fogScattering * thrown * max(amount - amountBefore, 0.0);
        amountBefore = amount;
        fogVolumeOut[uint3(cell.xy, slice)] = float4(min(sum, float3(kMostFogLight)), 1.0);
    }
}
