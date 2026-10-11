#pragma once

/// @file
/// What the picture of a view is finished with, as the PostProcess components of a world
/// say.
///
/// @code
/// const PostProcessSettings look = resolvePostProcess(world, {.position = view.position, .camera = cameraId});
/// postProcess.draw(commandList, targets, look, frame);
/// @endcode
///
/// A view starts from the settings a PostProcessSettings has when nothing is said. Then
/// every enabled PostProcess component that applies to it is laid over that, in the
/// order of their priorities - and of the ids of their entities where priorities are
/// the same, so that the result does not depend on how components are stored:
///
/// - a global one applies everywhere, by its weight;
/// - one with a box applies where the view is seen from inside the box, by its weight,
///   and less and less up to its blend distance outside;
/// - one on a camera applies to the views seen through that camera and to no other, by
///   its weight, after all the others: what that camera sets, the scene does not undo.
///
/// "Laid over" is blend() of PostProcessSettings.h: numbers go part of the way, what
/// cannot be blended changes over half way.

#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/World.h"

namespace cordite {

/// The view a look is worked out for.
struct PostProcessView {
    /// Where the view is seen from, in the world.
    Vec3 position{0.0f};
    /// The entity whose camera the view is seen through; nil for a view that is not an
    /// entity's, such as an editor's.
    Uuid camera;
};

/// How much of a PostProcess component with a box applies at `position`: 1 inside the
/// box that `placement` - the world matrix of its entity - makes of a cube of one metre
/// around the origin, falling to 0 at `blendDistance` metres outside it. A blend
/// distance of nothing makes the box end sharply.
[[nodiscard]] float postProcessBoxWeight(const Mat4& placement, float blendDistance, const Vec3& position);

/// The settings the picture of `view` is finished with. The world transforms must be
/// up to date.
[[nodiscard]] PostProcessSettings resolvePostProcess(const World& world, const PostProcessView& view);

} // namespace cordite
