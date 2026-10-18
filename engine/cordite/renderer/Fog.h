#pragma once

/// @file
/// Fog: air that is not clear, and what it does to light on its way to a viewer.
///
/// Haze, mist, smoke that hangs low: the further away something is, the less of its
/// light arrives, and the more of the fog's own color is seen in its place. A fog here
/// is as dense as it gets at one height and everywhere below it, and thins out above,
/// with every metre by the same factor ("exponential height fog"; without that factor
/// it is the same fog everywhere). How much of such a fog lies on a line of sight has a
/// closed form, so it costs a pixel a few multiplications, whatever the distance.
///
/// @code
/// FogSettings fog;
/// fog.color = Vec3(0.5f, 0.55f, 0.6f);
/// fog.density = 0.02f;                 // a fiftieth of the light is lost with every metre
///
/// const float amount = fogAmount(fog, cameraPosition, surfacePosition);    // 0.18 ten metres away
/// const Vec3 seen = applyFog(fog, lightOfTheSurface, cameraPosition, surfacePosition);
/// const Vec3 sky = applyFogTowards(fog, lightOfTheSky, cameraPosition, directionOfThePixel);
/// @endcode
///
/// The arithmetic is written down a second time in shaders/common/Fog.slang, where the
/// lighting applies it; tests hold the pictures that makes against what the functions
/// here say. A scene gets its fog from a Fog component (RenderComponents.h), which
/// SceneLights turns into FogSettings.

#include "cordite/math/Math.h"

namespace cordite {

/// The most a fog's anisotropy is, either way: towards one the phase function of a fog
/// (henyeyGreenstein, VolumetricFog.h) grows without end.
inline constexpr float kMostFogAnisotropy = 0.9f;

/// A fog as the renderer works with it. All colors are linear; without density it is
/// clear air.
struct FogSettings {
    /// The color of the fog itself: what is seen where nothing shows through it.
    Vec3 color{0.0f};
    /// The part of the light that is lost with every metre where the fog is densest: at
    /// `height` and below. (An optical depth per metre: 0.02 leaves 82% of the light
    /// after ten metres, and 14% after a hundred.)
    float density = 0.0f;
    /// The height (world Y) up to which the fog is as dense as `density`, in metres.
    float height = 0.0f;
    /// How fast the fog thins out above `height`: its density falls to 1 / e, a little
    /// more than a third, every 1 / heightFalloff metres. 0 is the same fog at every
    /// height.
    float heightFalloff = 0.0f;
    /// The fog begins this far from the viewer, in metres.
    float startDistance = 0.0f;
    /// The most of the fog's color that ever takes the place of what is behind it: 1
    /// lets distant things vanish, less keeps them - and the sky - faintly there.
    float maxOpacity = 1.0f;
    /// The light of the sun that the fog throws towards a viewer who looks straight at
    /// the sun, added to `color` there; ...
    Vec3 sunColor{0.0f};
    /// ... the unit vector towards the sun; ...
    Vec3 towardsSun{0.0f, 1.0f, 0.0f};
    /// ... and how closely around the sun that light stays: it falls off with the
    /// cosine of the angle to the sun, to this power. At least 1.
    float sunExponent = 8.0f;
    /// Whether lights light the fog (VolumetricFog.h): then each light throws its
    /// GpuLight::fogColor into it, with its shadows and its cookie, besides the fog's own
    /// color.
    bool volumetric = false;
    /// Of the light the fog takes away, the share it throws on, in each color: white for
    /// mist, which takes nothing for itself, darker for smoke. From 0 to 1.
    Vec3 scattering{1.0f};
    /// Which way the fog throws light on - the g of the Henyey-Greenstein phase function:
    /// 0 alike in every direction; towards 0.9 mostly onwards, so that a light is brightest
    /// seen through the fog from in front of it; below 0 mostly back. From -0.9 to 0.9.
    float anisotropy = 0.2f;
    /// How far in front of the viewer lights light the fog, in metres: the depth of the
    /// grid that light is worked out for. Beyond it only directional lights do, as if
    /// nothing stood in their way. From 1 to 1000.
    float volumetricDistance = 50.0f;

    /// Whether there is anything of it to see.
    [[nodiscard]] bool isEnabled() const { return density > 0.0f && maxOpacity > 0.0f; }

    /// Whether there is anything of it to see, and lights light it.
    [[nodiscard]] bool isLitByLights() const { return volumetric && isEnabled(); }
};

/// `fog` with every number finite and in the range its comment gives. What is sent to a
/// GPU goes through this first; the functions below work with whatever it leaves.
[[nodiscard]] FogSettings sanitized(const FogSettings& fog);

/// How much fog lies on the straight way from `from` to `to`: the optical depth, of
/// which e to the minus that much of the light gets through. Never less than nothing.
[[nodiscard]] float fogOpticalDepth(const FogSettings& fog, const Vec3& from, const Vec3& to);

/// The same along a line of sight without end: from `from` on, along the unit vector
/// `direction`. Infinite unless the fog thins out upwards and the line climbs.
[[nodiscard]] float fogOpticalDepthTowards(const FogSettings& fog, const Vec3& from, const Vec3& direction);

/// What part of what is seen of `to` from `from` is fog: 0 none of it, and never more
/// than FogSettings::maxOpacity.
[[nodiscard]] float fogAmount(const FogSettings& fog, const Vec3& from, const Vec3& to);

/// The same for what is infinitely far away along `direction`: the sky.
[[nodiscard]] float fogAmountTowards(const FogSettings& fog, const Vec3& from, const Vec3& direction);

/// The color of the fog as seen along the unit vector `direction`, away from the viewer:
/// its own, and towards the sun what it throws on of the sun's light.
[[nodiscard]] Vec3 fogColorAlong(const FogSettings& fog, const Vec3& direction);

/// The light that arrives at `from` of the `light` that leaves `to` towards it: what
/// the fog lets through, and the fog's own in place of the rest.
[[nodiscard]] Vec3 applyFog(const FogSettings& fog, const Vec3& light, const Vec3& from, const Vec3& to);

/// The same for `light` that comes from infinitely far away, against the unit vector
/// `direction`: that of the sky.
[[nodiscard]] Vec3 applyFogTowards(const FogSettings& fog, const Vec3& light, const Vec3& from, const Vec3& direction);

} // namespace cordite
