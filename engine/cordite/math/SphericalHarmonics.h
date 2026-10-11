#pragma once

/// @file
/// Light from all directions in nine numbers per color: spherical harmonics up to the
/// second band.
///
/// The light that arrives at a point from everything around it is a function of the
/// direction. A matte surface does not see that function sharply: it adds up the light
/// of the whole half of the world it faces, weighted by how squarely each part of it
/// faces the surface. That sum is so smooth, however rough the light itself is, that
/// nine numbers per color hold it to within a few percent (R. Ramamoorthi and P.
/// Hanrahan, "An Efficient Representation for Irradiance Environment Maps", 2001).
///
/// @code
/// SphericalHarmonics9 light;
/// for (const Sample& sample : samplesOfTheSky) {
///     light.add(sample.direction, sample.radiance, sample.solidAngle);
/// }
/// const Vec3 onAFloor = light.irradiance(kWorldUp) / kPi;   // what a white, matte floor gives off
/// @endcode
///
/// Directions are unit vectors that point away from the point being lit: towards where
/// the light comes from, or the way a surface faces.

#include "cordite/math/Math.h"

#include <array>
#include <cstddef>

namespace cordite {

inline constexpr size_t kSphericalHarmonicsCount = 9;

/// The nine basis functions at a unit direction: one that is the same everywhere, three
/// that follow y, z and x, and five of the second degree. Over the whole sphere each
/// has a mean square of one over four pi, and any two different ones a mean product of
/// nothing.
[[nodiscard]] std::array<float, kSphericalHarmonicsCount> sphericalHarmonicsBasis(const Vec3& direction);

/// Light from all around a point, as the weights of the nine basis functions.
struct SphericalHarmonics9 {
    std::array<Vec3, kSphericalHarmonicsCount> coefficients{};

    /// Adds light of `radiance` that arrives from around `direction`, over `solidAngle`
    /// steradians. Adding the whole sphere piece by piece gives its light.
    void add(const Vec3& direction, const Vec3& radiance, float solidAngle);

    /// The light arriving from `direction`, as far as nine numbers tell: a blurred
    /// picture of it, which can dip below nothing beside a small, bright light.
    [[nodiscard]] Vec3 radiance(const Vec3& direction) const;

    /// The light falling on a surface that faces `normal`, from all of the half of the
    /// world it sees: its irradiance. Divided by pi, it is what a white, matte surface
    /// gives off. Never less than nothing.
    [[nodiscard]] Vec3 irradiance(const Vec3& normal) const;

    /// The coefficients, each scaled so that the sum of their products with the basis at
    /// a normal is the irradiance there divided by pi: what a shader needs to light a
    /// matte surface with one sum.
    [[nodiscard]] std::array<Vec3, kSphericalHarmonicsCount> diffuseCoefficients() const;
};

} // namespace cordite
