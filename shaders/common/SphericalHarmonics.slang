// Light from all directions in nine numbers per color: the basis of spherical harmonics up
// to the second band. The C++ side is engine/cordite/math/SphericalHarmonics.h: change
// both.
//
// Nothing here but arithmetic: passes of every binding layout include it.

#pragma once

static const uint kSphericalHarmonicsCount = 9;

// The nine spherical harmonics at a unit direction. Matches sphericalHarmonicsBasis()
// in engine/cordite/math/SphericalHarmonics.cpp.
void sphericalHarmonicsBasis(float3 direction, out float basis[9])
{
    const float x = direction.x;
    const float y = direction.y;
    const float z = direction.z;
    basis[0] = 0.282095;
    basis[1] = 0.488603 * y;
    basis[2] = 0.488603 * z;
    basis[3] = 0.488603 * x;
    basis[4] = 1.092548 * x * y;
    basis[5] = 1.092548 * y * z;
    basis[6] = 0.315392 * (3.0 * z * z - 1.0);
    basis[7] = 1.092548 * x * z;
    basis[8] = 0.546274 * (x * x - y * y);
}

// What each band of the harmonics of the light keeps of it in the light a matte surface
// gets, divided by pi: the scale from SphericalHarmonics9::coefficients to
// diffuseCoefficients(), so that a surface's light is one sum of products with the basis.
float diffuseBandScale(uint index)
{
    return index == 0 ? 1.0 : (index < 4 ? 2.0 / 3.0 : 0.25);
}
