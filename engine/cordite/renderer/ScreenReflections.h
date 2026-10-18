#pragma once

/// @file
/// Reflections from the screen: what a shiny surface reflects, found in the picture that
/// has been drawn.
///
/// Without them a surface reflects pictures of everything around it - the environment,
/// taken from far away, or a probe, taken at a point of the scene (EnvironmentProbes.h):
/// a floor reflects the sky, or the walls of its room as they stood when the probe was
/// taken, and never the crate that stands on it. Reflections from the screen follow
/// each reflection through the depth buffer, and where it meets something that was
/// drawn, the light the lighting gave that pixel takes the place of what the
/// surroundings would have given (M. McGuire and M. Mara, "Efficient GPU Screen-Space
/// Ray Tracing", 2014, walked more simply).
///
/// A reflection is followed in even steps across the picture (traceScreenReflection()):
/// from a pixel's width above the surface towards where it leaves the picture, comes to
/// the near plane or has gone maxDistance. Its depth at each step is exact - the depth of
/// a point on a line, z over w, changes evenly across the picture. A step whose point lies
/// behind what was drawn there is the first sign of a meeting: the way from the step
/// before is halved four times, and what is found there is a meeting only when the point
/// lies no further behind it than `thickness` metres. Further behind, the reflection went
/// behind something thin, and goes on.
///
/// What is found is trusted less towards the borders of the picture (screenEdgeFade()):
/// a reflection that leaves the picture falls back on the surroundings, and must not
/// jump there. Likewise towards maxDistance (reflectionDistanceFade()), and the rougher
/// the surface (reflectionRoughnessFade()): what the screen gives is as sharp as a
/// mirror's reflection - it is not blurred for a rough surface, as the reflections of
/// the surroundings are - so past maxRoughness nothing is taken from it.
///
/// The functions here are the arithmetic of shaders/common/ScreenReflections.slang,
/// written down a second time: tests hold them against geometry with a known answer,
/// and the pictures of the shaders against them. Change both.
///
/// @code
/// const ScreenReflectionSettings settings = screenReflectionSettingsFor(look, ScreenEffectsQuality::Medium);
/// const ScreenReflectionHit hit = traceScreenReflection(view, UVec2(1280, 720), settings, origin, normal,
///                                                       direction, [&](Vec2 uv) { return depthOfPixelAt(uv); });
/// if (hit.found) {
///     // The light drawn at hit.uv, trusted hit.confidence, times reflectionRoughnessFade().
/// }
/// @endcode

#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/ScreenEffects.h"

#include <cstdint>
#include <functional>

namespace cordite {

/// What the reflections from the screen of a view are drawn with: the look of its scene
/// and the quality of the view.
struct ScreenReflectionSettings {
    /// How much of what the screen gives takes the place of the surroundings' reflections:
    /// 0 none - which costs nothing - to 1, all of it.
    float intensity = 0.0f;
    /// The roughest surface that takes anything from the screen: those up to half as rough
    /// take all that is found, rougher ones less, and this one nothing.
    float maxRoughness = 0.4f;
    /// How far a reflection is followed, in metres.
    float maxDistance = 20.0f;
    /// How far behind what was drawn a reflection may pass and still meet it, in metres.
    float thickness = 0.3f;
    /// The reflections are followed for one pixel in this many, each way: 1 for every pixel.
    uint32_t divisor = 2;
    /// In how many steps across the picture a reflection is followed.
    uint32_t steps = 32;
};

/// The settings of a view whose scene looks like `look` - with `look` within its ranges
/// (sanitized()) - drawn at `quality`. ScreenEffectsQuality::Off draws none.
[[nodiscard]] ScreenReflectionSettings screenReflectionSettingsFor(const PostProcessSettings& look,
                                                                   ScreenEffectsQuality quality);

/// The share of the picture, at each of its borders, over which what is found there fades.
inline constexpr float kScreenEdgeFade = 0.1f;

/// How much a reflection found at `uv` - from (0, 0) at the top left of the picture to
/// (1, 1) - is trusted for being found there: 1 inside, falling evenly to 0 over the last
/// kScreenEdgeFade of the picture at each border, and 0 outside it or for no number.
[[nodiscard]] float screenEdgeFade(Vec2 uv);

/// How much a reflection that went `travelled` metres is trusted, of `maxDistance`: 1 up
/// to three quarters of the way, falling evenly to 0 at maxDistance. 0 when maxDistance
/// is nothing, or for no number.
[[nodiscard]] float reflectionDistanceFade(float travelled, float maxDistance);

/// How much a surface of `roughness` takes from the screen: 1 up to half of
/// `maxRoughness`, falling evenly to 0 at it. 0 when maxRoughness is nothing, or for no
/// number.
[[nodiscard]] float reflectionRoughnessFade(float roughness, float maxRoughness);

/// Where a reflection met what was drawn.
struct ScreenReflectionHit {
    /// Whether it met anything in the picture, within maxDistance.
    bool found = false;
    /// Where in the picture: from (0, 0) at the top left to (1, 1).
    Vec2 uv{0.0f};
    /// How far it went from the surface, in metres.
    float travelled = 0.0f;
    /// How much what was found is trusted, for where it is in the picture and how far the
    /// reflection went (screenEdgeFade() times reflectionDistanceFade()); the surface's
    /// roughness is the surface's to add. 0 when nothing was found.
    float confidence = 0.0f;
};

/// The reversed depth of what was drawn at a place of the picture - from (0, 0) at the
/// top left to (1, 1) - as the pixel there holds it: 0 where nothing was drawn, and
/// outside the picture, of which nothing is known.
using DepthAt = std::function<float(Vec2 uv)>;

/// Follows the reflection that leaves the surface at `origin`, whose unit normal is
/// `normal`, along the unit vector `direction`, in a picture of `view` of `targetSize`
/// pixels whose depth `depthAt` reads. It sets out a pixel's width above the surface: the
/// depth buffer holds the surface as it is at the middles of its pixels, which a
/// reflection that grazes it would seem to pass behind. Nothing is found without steps,
/// without a maxDistance, and for a reflection that would cross less than a pixel of the
/// picture.
[[nodiscard]] ScreenReflectionHit traceScreenReflection(const CameraView& view, UVec2 targetSize,
                                                        const ScreenReflectionSettings& settings, const Vec3& origin,
                                                        const Vec3& normal, const Vec3& direction,
                                                        const DepthAt& depthAt);

} // namespace cordite
