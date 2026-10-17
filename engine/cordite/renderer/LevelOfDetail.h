#pragma once

/// @file
/// Levels of detail: which version of a mesh a view draws.
///
/// @code
/// const LodView lodView = lodViewOf(view, targets.size().y, settings.lodErrorPixels);
/// const uint32_t lod = selectLod(mesh.lodErrors, worldBounds, scaleOf(localToWorld), lodView);
/// commandList->drawIndexed(mesh.rangeOf(submesh, lod) ...);
/// @endcode
///
/// A mesh comes with coarser versions of itself (MeshData::lods), each with an error: how
/// far its surface is from the mesh's own, at the most. A view draws the coarsest version
/// whose error looks no larger than `errorPixels` on its screen, measured where the mesh
/// is nearest to the camera - so that what changes when a mesh moves from one version to
/// the next is less than a pixel. A camera inside a mesh's bounds sees it at its finest.

#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"

#include <cstdint>
#include <span>

namespace cordite {

/// What choosing a level of detail needs to know of a view.
struct LodView {
    Vec3 cameraPosition{0.0f};
    bool orthographic = false;
    /// How many pixels high a metre looks: one metre in front of the camera with
    /// perspective, anywhere without.
    float pixelsPerMetre = 0.0f;
    /// Nearer than this a mesh is always drawn at its finest, in metres.
    float nearPlane = 0.1f;
    /// How large the error of a level may look, in pixels. Zero, less or no number: every
    /// mesh at its finest.
    float errorPixels = 1.0f;
};

/// The LodView of `view`, drawn into a target `targetHeight` pixels high, allowing errors
/// of `errorPixels`.
[[nodiscard]] LodView lodViewOf(const CameraView& view, uint32_t targetHeight, float errorPixels);

/// How much a matrix enlarges what it places, at the most: the length of the longest of
/// its first three columns.
[[nodiscard]] float scaleOf(const Mat4& localToWorld);

/// The level of a mesh to draw: 0 for the mesh itself, `n` for the n-th of `errors` - the
/// errors of its coarser levels, in the units of the mesh's space, in order - for the
/// coarsest whose error, enlarged by `worldScale`, looks no larger than the view allows
/// where the sphere around `worldBounds` is nearest to the camera. 0 when none does, for
/// a camera that is nearer than the near plane - inside the bounds, say - and when a
/// number is none.
[[nodiscard]] uint32_t selectLod(std::span<const float> errors, const Aabb& worldBounds, float worldScale,
                                 const LodView& view);

} // namespace cordite
