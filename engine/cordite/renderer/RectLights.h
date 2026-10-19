#pragma once

/// @file
/// What a rectangle that gives off light gives a surface: the arithmetic of rect lights.
///
/// What a matte surface gets of a rectangle is the integral of the cosine over the part of
/// it above the surface's horizon, which has a closed form - a sum over the edges of the
/// rectangle as the surface sees them. What the GGX lobe of a shiny surface reflects of it
/// has none. The lobe is stood in for by a cosine that a matrix has stretched, sheared and
/// turned into its shape, and the rectangle is taken through the inverse of that matrix,
/// where the closed form of the cosine applies again: linearly transformed cosines (Heitz,
/// Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed
/// Cosines", 2016). The matrices are the fit of its authors, one for each roughness and
/// angle of the view, compiled into the module from the pinned ltc_code (see
/// docs/third-party.md): fitted to the GGX of shaders/common/Brdf.slang, with Smith's
/// height-correlated visibility. What it reflects of a light in the lobe's way, seen at up
/// to sixty degrees from the normal, is within an eighth of what that GGX does, mostly
/// within a fiftieth. At grazing angles the cosine that stands in for the lobe reaches below
/// the horizon, where no light is: of a sky full of light the fit reflects a tenth less at
/// 75 degrees, a seventh at 85; and what the lobe has far from its middle it gives less of.
///
/// @code
/// // A panel half a metre by a quarter, three metres up, that shines down.
/// const RectLightShape panel{.center = Vec3(0.0f, 3.0f, 0.0f),
///                            .right = Vec3(0.25f, 0.0f, 0.0f),
///                            .up = Vec3(0.0f, 0.0f, -0.125f)};
/// const RectLightIrradiance matte = rectLightIrradiance(panel, point, normal);
/// const RectLightSpecular shine = rectLightSpecular(panel, point, normal, toViewer, roughness);
/// // What the panel's radiance becomes on its way to a viewer, by Schlick's reflectances.
/// const Vec3 diffuse = radiance * diffuseColor * matte.formFactor;
/// const Vec3 specular = radiance * (f0 * (shine.lobe - shine.grazing) + f90 * shine.grazing);
/// @endcode
///
/// The arithmetic is written down a second time, in shaders/common/RectLights.slang and
/// shadeRectLight of shaders/common/Lights.slang, which SceneLightingTests holds to these
/// functions: change both.

#include "cordite/math/Math.h"

#include <cstdint>
#include <span>

namespace cordite {

/// The least roughness a surface is lit with: matches kMinimumRoughness of
/// shaders/common/Brdf.slang, below which a light would be reflected into a point.
inline constexpr float kMinimumRoughness = 0.045f;

/// How many texels the tables of the fit have a side: roughness across, from 0 at the
/// middle of the first texel to 1 at the middle of the last, and sqrt(1 - n.v) - n.v the
/// cosine of the angle between the normal and the direction to the viewer - down, from 0
/// to 1 in the same way.
inline constexpr uint32_t kLtcTableSize = 64;

/// A rectangle that gives off light from one face. It shines towards cross(up, right):
/// for a rectangle along an entity's +X and +Y, the entity's -Z, the way every light of
/// the engine shines.
struct RectLightShape {
    /// Its middle, in the world.
    Vec3 center{0.0f};
    /// From the middle to the middle of its right edge: half its width, and along it.
    Vec3 right{0.5f, 0.0f, 0.0f};
    /// From the middle to the middle of its top edge, at right angles to `right`.
    Vec3 up{0.0f, 0.5f, 0.0f};
};

/// The fit at one roughness and one angle of the view: the texels of the two tables,
/// filtered between their neighbours as a GPU filters a texture.
struct LtcFit {
    /// The inverse of the matrix that makes the GGX lobe of a cosine, divided by its middle
    /// element - in the frame where the normal is +Z and the viewer is in the XZ plane, on
    /// the side of +X: (m00, m20, m02, m22) of [[m00, 0, m02], [0, 1, 0], [m20, 0, m22]].
    Vec4 matrix{0.0f};
    /// What the lobe reflects of all around with a reflectance of one; the part of that
    /// which Schlick's factor gives to the reflectance at grazing angles; nothing; and the
    /// form factor of a sphere, which the engine does not use.
    Vec4 magnitude{0.0f};
};

/// What a rectangle of light gives a matte surface.
struct RectLightIrradiance {
    /// The share of the light's radiance that a white, matte surface sends back: the
    /// integral of the cosine over what of the rectangle is above the surface's horizon,
    /// over pi. 0 for a surface behind the light, or one that sees nothing of it.
    float formFactor = 0.0f;
    /// The way the light arrives on average, weighed by the cosine: unit length, or nothing
    /// where nothing arrives.
    Vec3 direction{0.0f};
};

/// What the GGX lobe of a surface reflects of a rectangle of light, towards a viewer: with
/// Schlick's reflectances f0 and f90 the radiance of the light is reflected times
/// f0 * (lobe - grazing) + f90 * grazing, Schlick's factor integrated with the lobe.
struct RectLightSpecular {
    /// What the lobe reflects with a reflectance of one.
    float lobe = 0.0f;
    /// The part of `lobe` that Schlick's factor gives to the reflectance at grazing angles.
    float grazing = 0.0f;
};

/// The texels of the fit's tables, kLtcTableSize * kLtcTableSize of each, row by row - as
/// the GPU is given them: the matrices (LtcFit::matrix) and what the lobes reflect
/// (LtcFit::magnitude).
[[nodiscard]] std::span<const Vec4> ltcMatrixTexels();
[[nodiscard]] std::span<const Vec4> ltcMagnitudeTexels();

/// The fit at a roughness - perceptual, from 0 to 1 - and the cosine of the angle between
/// the normal and the direction to the viewer. Both are clamped to 0..1, as a GPU's
/// sampler clamps to the edges of a texture; what is no number counts as nothing.
[[nodiscard]] LtcFit ltcFit(float roughness, float nDotV);

/// What a matte surface at `position` with the unit `normal` gets of `light`: nothing
/// where it is behind the light, or in the plane of it.
[[nodiscard]] RectLightIrradiance rectLightIrradiance(const RectLightShape& light, const Vec3& position,
                                                      const Vec3& normal);

/// How much of everything around `position` the rectangle of `light` fills, as a point
/// there sees it - in steradians, 2 pi for a rectangle that fills half of it - and nothing
/// where the point is behind it or in its plane. What a fog there is lit with by the light
/// is the radiance of its face times this.
[[nodiscard]] float rectLightSolidAngle(const RectLightShape& light, const Vec3& position);

/// What the GGX lobe of a surface at `position`, with the unit `normal` and a `roughness`
/// (perceptual; kMinimumRoughness at the least), reflects of `light` along the unit
/// `toViewer`: nothing where the surface is behind the light, or in the plane of it.
[[nodiscard]] RectLightSpecular rectLightSpecular(const RectLightShape& light, const Vec3& position, const Vec3& normal,
                                                  const Vec3& toViewer, float roughness);

} // namespace cordite
