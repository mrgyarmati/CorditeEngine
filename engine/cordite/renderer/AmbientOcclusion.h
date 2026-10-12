#pragma once

/// @file
/// Ambient occlusion from the screen: how much of the light from all around reaches a
/// point, worked out from the depth of what is drawn around it.
///
/// Light from all around - a sky, an environment - reaches a point on a surface from
/// the half of the world it faces, less what stands in the way: the inside of a crease,
/// the foot of a wall, the ground under a crate get less of it. How much less is
/// measured the way of "ground truth ambient occlusion" (J. Jimenez et al., "Practical
/// Real-Time Strategies for Accurate Indirect Occlusion", 2016). The half of the world
/// above a point is cut into slices: half planes that all contain the line from the
/// point to the viewer, like the pages of an open book. In each slice the depth buffer
/// is walked both ways from the point, and the steepest thing met either way is the
/// horizon of that side. Between the two horizons the light comes in, and how much of
/// it that is, weighted by how squarely it falls on the surface, has a closed form:
/// sliceVisibility().
///
/// One thing is done differently here. What is met on the walk is at the middle of a
/// pixel, which is seldom exactly in the slice; taken as if it were, a floor seen at a
/// low angle finds its own pixels standing in its light. So what is met is not
/// measured from the direction to the viewer but from the surface: by how far it
/// rises above the plane of the surface, in the slice it really is in
/// (riseAboveSurface()). A horizon is the surface, less the steepest rise - and on a
/// flat surface that is nothing, wherever the pixels are.
///
/// @code
/// // A point on a floor at the foot of a wall, seen from straight above. In the slice
/// // that runs across the wall, one side is open and the other is all wall:
/// const float open = openSliceVisibility(0.0f);                    // 1
/// const float atTheWall = sliceVisibility(0.0f, -kHalfPi, 0.0f);   // 0.5
///
/// // Something a metre away and a metre up from a floor rises 45 degrees above it:
/// const float rise = riseAboveSurface(glm::normalize(Vec3(1.0f, 1.0f, 0.0f)), kWorldUp, kWorldUp);
///
/// // What the pass that draws it is told, from the look of a scene and the quality of a view:
/// const AmbientOcclusionSettings settings = ambientOcclusionSettingsFor(look, ScreenEffectsQuality::Medium);
/// @endcode
///
/// The functions here are the arithmetic of shaders/passes/AmbientOcclusion.slang,
/// written down a second time: tests hold them against cases with a known answer, and
/// the pictures the shader makes against those.
///
/// All angles are in radians and measured in a slice, from the direction to the viewer:
/// positive towards the way the slice runs across the screen, negative the other way.

#include "cordite/math/Math.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/ScreenEffects.h"

#include <cstdint>

namespace cordite {

/// A surface normal as a slice sees it.
struct SliceNormal {
    /// The angle of the normal's shadow on the slice, from the direction to the viewer.
    float angle = 0.0f;
    /// How long that shadow is: 1 when the normal lies in the slice, 0 when it stands
    /// at a right angle to it. A slice counts for as much as this.
    float length = 0.0f;
};

/// The normal of a surface as the slice sees it that contains `toViewer` and runs
/// across the screen along `sliceDirection`. `normal` and `toViewer` are unit vectors;
/// `sliceDirection` need not be one, nor at a right angle to `toViewer`, but must not be
/// along it.
[[nodiscard]] SliceNormal projectIntoSlice(const Vec3& normal, const Vec3& toViewer, const Vec3& sliceDirection);

/// How much light from all around reaches a surface through one slice, weighted by how
/// squarely it falls on the surface. `normalAngle` is SliceNormal::angle;
/// `horizonBefore` (zero or less) and `horizonAfter` (zero or more) are the angles at
/// which the light is cut off on either side. A horizon further round than a right
/// angle from the normal is the surface's own: it is taken for exactly that.
///
/// The numbers of different slices are only comparable through
/// openSliceVisibility(): a slice with nothing in the way gives that, not 1.
[[nodiscard]] float sliceVisibility(float normalAngle, float horizonBefore, float horizonAfter);

/// What sliceVisibility() gives for a slice with nothing in the way. Over all the slices
/// of a point, each times the length of its SliceNormal, this averages one.
[[nodiscard]] float openSliceVisibility(float normalAngle);

/// How far something in the direction `towards` from a point on a surface rises above
/// the surface there, as an angle in the slice that it and `toViewer` make: 0 along the
/// surface, half of pi along the shadow of the normal on that slice, up to pi as it
/// comes round to the surface on the other side; less than 0 below the surface. All
/// three are unit vectors. `towards` must not be along `toViewer`: then there is no
/// slice, and the result is 0.
[[nodiscard]] float riseAboveSurface(const Vec3& towards, const Vec3& normal, const Vec3& toViewer);

/// What the ambient occlusion of a view is worked out with: the look a scene asks for,
/// and what the quality of the view allows.
struct AmbientOcclusionSettings {
    /// How strongly what stands in the light darkens it: 0 not at all, 1 as the
    /// geometry says, more than that exaggerated.
    float intensity = 1.0f;
    /// How far from a point things are looked for that stand in its light, in metres.
    float radius = 0.5f;
    /// How far something has to rise above a surface before it counts, in radians.
    float bias = 0.0f;
    /// See ScreenEffectsQualitySettings.
    uint32_t divisor = 2;
    uint32_t slices = 2;
    uint32_t steps = 4;

    /// False when there is nothing to work out.
    [[nodiscard]] bool isEnabled() const { return intensity > 0.0f; }
};

/// The settings for a view of the given quality in a scene with the given look. `look`
/// is taken as it is: bring it into its ranges first (sanitized()). With
/// ScreenEffectsQuality::Off the result is not enabled.
[[nodiscard]] AmbientOcclusionSettings ambientOcclusionSettingsFor(const PostProcessSettings& look,
                                                                   ScreenEffectsQuality quality);

} // namespace cordite
