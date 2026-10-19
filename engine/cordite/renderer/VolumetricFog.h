#pragma once

/// @file
/// Lights that light the fog: what they throw towards a viewer from the air in between.
///
/// A fog (Fog.h) has a color of its own, which nothing lights. A fog lit by lights
/// (FogSettings::volumetric) gets, besides, what every light throws towards the viewer from
/// each point of the air that it reaches: the beam of a spot light in haze, the glow around
/// a lamp, the rays of the sun through a gap - with the shadows and the cookies of the
/// lights, which are what makes beams of them. How much a light gives the fog is its
/// GpuLight::fogColor: its color, times the `volumetricIntensity` of its Light component.
///
/// From the viewer to a point a distance s along a line of sight, that light is
///
///     scattering * (integral from 0 to s of F(t) dA(t))
///
/// where A(t) is how much of what is t away is fog (fogAmount(), with the fog's start
/// distance and its largest opacity), so that dA is the light the air between t and t + dt
/// takes away and scatters - and F(t) is what the lights throw at the point t away
/// towards the viewer, per unit of that: for each light its fogColor, as much of it as
/// reaches the point, times the phase function of the angle between the way the light
/// travels and the way to the viewer (henyeyGreenstein()).
///
/// The renderer works it out for a grid of boxes laid over the view (FogVolumeGrid), from
/// the camera to the fog's volumetricDistance, with the boxes deeper the further away they
/// are: F at the middle of each box (VolumetricFogPass's first pass), and along each column
/// the sum of F times how much the fog's amount grows across the box (its second pass) -
/// which is exact where F is the same all through a box, as it is for a sun nothing
/// shadows. What a pixel gets is read from that between the ends of the boxes. Beyond the
/// grid only directional lights light the fog, as if nothing stood in their way, which has
/// a closed form: F times how much the amount grows from the end of the grid to the point.
///
/// @code
/// const FogVolumeGrid grid = fogVolumeGridFor(UVec2(1920, 1080), ScreenEffectsQuality::Medium);
/// // grid.columns == 160, grid.rows == 90, grid.slices == 64
///
/// // What a pixel that looks along `direction` at what is 20 m away gets from the lights,
/// // nothing in their way:
/// const Vec3 light = fogLightAlong(fog, lights, {.viewer = cameraPosition, .direction = direction,
///                                                .forward = cameraForward, .distance = 20.0f, .slices = grid.slices});
/// @endcode
///
/// The arithmetic is written down twice: here, and in shaders/common/FogVolume.slang,
/// shaders/common/VolumetricFog.slang and the two passes of VolumetricFogPass. Tests hold
/// what the GPU draws against fogLightAlong(); change both.

#include "cordite/math/Math.h"
#include "cordite/renderer/Fog.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/ScreenEffects.h"

#include <cstdint>
#include <span>

namespace cordite {

/// The grid of boxes laid over a view that what lights throw into its fog is worked out
/// for: `columns` across the picture, `rows` down it, `slices` from the camera to how far
/// the fog is lit. The boxes of a slice cover the picture exactly.
struct FogVolumeGrid {
    uint32_t columns = 0;
    uint32_t rows = 0;
    uint32_t slices = 0;

    /// Whether there are no boxes, and so nothing to work out: what ScreenEffectsQuality::Off
    /// gives.
    [[nodiscard]] bool isEmpty() const { return columns == 0 || rows == 0 || slices == 0; }

    bool operator==(const FogVolumeGrid&) const = default;
};

/// The grid of a view of `targetSize` pixels, at `quality`: boxes about
/// ScreenEffectsQualitySettings::fogVolumeTile pixels across and down - fewer and larger
/// where the picture is so large that there would be more than fogVolumeMaxCells across or
/// down - and fogVolumeSlices deep. None for ScreenEffectsQuality::Off and for a picture
/// of no pixels.
[[nodiscard]] FogVolumeGrid fogVolumeGridFor(UVec2 targetSize, ScreenEffectsQuality quality);

/// How far in front of the camera, in metres, a share `share` of the way through the
/// slices of a grid that reaches `distance` is: `distance` times the square of the share,
/// so that slices are thin near the camera, where a metre is much of the picture. Shares
/// outside 0 to 1 count as those.
[[nodiscard]] float fogSliceDepth(float share, float distance);

/// The share of the way through the slices of a grid that reaches `distance` at which a
/// point `depth` metres in front of the camera is: the inverse of fogSliceDepth(), from 0
/// to 1. A depth beyond the grid is at its end; a grid that reaches nowhere ends at once.
[[nodiscard]] float fogSliceShare(float depth, float distance);

/// The phase function of a fog of the given anisotropy (FogSettings::anisotropy, held to
/// kMostFogAnisotropy either way): of the light that the fog scatters at a point, the
/// share that goes into one steradian at an angle whose cosine is `cosine` to the way the
/// light was going. That of Henyey and Greenstein: 1 / (4 pi) every way for an anisotropy
/// of 0, and towards 1 more and more of it onwards. Its integral over the sphere is one.
[[nodiscard]] float henyeyGreenstein(float cosine, float anisotropy);

/// What `light` throws towards a viewer from `position`, per unit of light the fog
/// scatters there: its fogColor, as much of it as reaches the position by its distance and
/// its cone (Lights.slang) - of a rect light, the fogColor over the area of its face times
/// how much of everything around the position the face fills (rectLightSolidAngle()), within
/// its range window - times the phase function of the angle between the way its light
/// travels from its middle and `toViewer`, a unit vector. What stands in its way and its
/// cookie are left out. Nothing beyond a point, spot or rect light's range, nothing behind a
/// rect light, and nothing from a light whose fog color is black.
[[nodiscard]] Vec3 lightInFog(const GpuLight& light, const Vec3& position, const Vec3& toViewer, float anisotropy);

/// A line of sight, as fogLightAlong() follows it.
struct FogSight {
    /// Where it sets out: the camera, or with a camera without perspective the point of the
    /// plane the camera is in that it sets out from.
    Vec3 viewer{0.0f};
    /// The unit vector along which it goes.
    Vec3 direction{0.0f, 0.0f, -1.0f};
    /// The unit vector along which the camera looks: depths are measured along it.
    Vec3 forward{0.0f, 0.0f, -1.0f};
    /// How far along it what is seen is, in metres: infinity for the sky.
    float distance = 0.0f;
    /// How many slices deep the grid is (FogVolumeGrid::slices).
    uint32_t slices = 0;
};

/// What `lights` throw towards the viewer from the fog along `sight`, as the renderer works
/// it out: what is added to the light of a pixel that looks along it, when the fog is lit
/// by lights and nothing stands in their way, at the middle of a column of the grid. Each
/// light is asked once at the middle of each slice, from the viewer out, and the pixel
/// gets the sum up to the ends of the slices around what it sees, in proportion to its
/// share between them (fogSliceShare()); beyond the grid, the directional lights add
/// theirs in the closed form. Nothing for a fog that is not lit by lights, for a grid of no
/// slices, and for a line of sight that does not go forward.
[[nodiscard]] Vec3 fogLightAlong(const FogSettings& fog, std::span<const GpuLight> lights, const FogSight& sight);

} // namespace cordite
