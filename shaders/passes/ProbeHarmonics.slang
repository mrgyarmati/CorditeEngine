// Projects the picture of an environment probe - a cube map, read as the array of its six
// faces - onto nine spherical harmonics (EnvironmentProbes::project): the light of each
// texel times the basis in its direction, times the part of the sphere the texel covers,
// all added up - and scaled as SphericalHarmonics9::diffuseCoefficients() scales them, so
// that the light of a matte surface is one sum. The C++ side of the arithmetic is
// SphericalHarmonics9::add() and the solid angle of a texel of a cube map.
//
// One group of 256 threads does it all: each adds up every 256th texel, then the group
// adds up what its threads found, one harmonic at a time.

#include "common/CubeMap.slang"
#include "common/SphericalHarmonics.slang"

cbuffer ProbeHarmonicsConstants : register(b0, space0)
{
    // Where the nine go: from slot * 9 on.
    uint slot;
    // How many texels a side each face has.
    uint faceSize;
    uint harmonicsPadding0;
    uint harmonicsPadding1;
};

Texture2DArray<float4> picture : register(t0, space0);
RWStructuredBuffer<float4> harmonics : register(u0, space0);

static const uint kThreads = 256;

groupshared float3 partial[kThreads];

[numthreads(256, 1, 1)]
void probeHarmonicsCompute(uint thread : SV_GroupIndex)
{
    float3 sums[9];
    for (uint index = 0; index < kSphericalHarmonicsCount; ++index)
    {
        sums[index] = float3(0.0);
    }
    const uint texelsPerFace = faceSize * faceSize;
    const float step = 2.0 / float(faceSize);
    for (uint texel = thread; texel < texelsPerFace * kCubeFaceCount; texel += kThreads)
    {
        const uint face = texel / texelsPerFace;
        const uint within = texel - face * texelsPerFace;
        const uint2 place = uint2(within % faceSize, within / faceSize);
        const float2 uv = (float2(place) + 0.5) / float(faceSize);
        // The part of the sphere a texel covers: its area on the face, seen from the middle
        // of the cube at its distance and slant.
        const float2 onFace = uv * 2.0 - 1.0;
        const float solidAngle = step * step / pow(1.0 + dot(onFace, onFace), 1.5);
        const float3 light = picture.Load(int4(int2(place), int(face), 0)).rgb;
        float basis[9];
        sphericalHarmonicsBasis(faceDirection(face, uv), basis);
        for (uint index = 0; index < kSphericalHarmonicsCount; ++index)
        {
            sums[index] += light * (basis[index] * solidAngle);
        }
    }

    for (uint index = 0; index < kSphericalHarmonicsCount; ++index)
    {
        partial[thread] = sums[index];
        GroupMemoryBarrierWithGroupSync();
        for (uint stride = kThreads / 2; stride > 0; stride /= 2)
        {
            if (thread < stride)
            {
                partial[thread] += partial[thread + stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (thread == 0)
        {
            harmonics[slot * kSphericalHarmonicsCount + index] = float4(partial[0] * diffuseBandScale(index), 0.0);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
