#pragma once

/// @file
/// What an image of everything around - a sky, a horizon, a ground - lights matte
/// surfaces with.
///
/// The image is equirectangular (cordite/math/Equirectangular.h says which direction is
/// where in it). A matte surface adds up the light of the whole half of the world it
/// faces, which is so smooth a function of the way it faces that nine numbers per color
/// hold it (cordite/math/SphericalHarmonics.h). This is where an image becomes those
/// nine numbers; the renderer does it once for an environment, when it is first used
/// (EnvironmentMaps.h).
///
/// @code
/// CORDITE_TRY_ASSIGN(const Image sky, Image::load(projectDirectory / "assets/sky.hdr"));
/// const SphericalHarmonics9 light = projectEnvironment(sky);
/// const Vec3 onAFloor = light.irradiance(kWorldUp) / kPi;   // what a white, matte floor gives off
/// @endcode

#include "cordite/assets/Image.h"
#include "cordite/math/SphericalHarmonics.h"

namespace cordite {

/// The light of an equirectangular image as spherical harmonics: what it lights matte
/// surfaces with. Every pixel counts, by the part of the sphere it covers, whatever the
/// size of the image. An image of 8-bit colors is taken as sRGB; pixels that hold what
/// is no number count as no light, and so do values less than nothing. An empty image
/// gives no light.
[[nodiscard]] SphericalHarmonics9 projectEnvironment(const Image& image);

} // namespace cordite
