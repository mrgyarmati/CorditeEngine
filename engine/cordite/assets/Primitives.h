#pragma once

/// @file
/// Generators for simple meshes: box, plane, sphere, cylinder, cone, capsule, torus.
///
/// They are what a scene is blocked out with before real models exist, what tests render
/// without needing files, and the shapes behind the engine's built-in mesh assets.
///
/// @code
/// const MeshData crate = makeBox(Vec3(1.0f, 0.8f, 1.0f));
/// const MeshData ball = makeSphere(0.25f, 48, 24);
/// @endcode
///
/// Every mesh is centred on the origin, has one submesh, smooth normals where the shape
/// is round, tangents along increasing `u`, and texture coordinates that cover the unit
/// square once (per face for the box, per cap for the cylinder and the cone). Sizes that
/// are not positive are raised to a millimetre and segment counts are kept between the
/// stated minimum and kMaxPrimitiveSegments, so every argument yields a valid mesh.

#include "cordite/assets/Mesh.h"

#include <cstdint>

namespace cordite {

/// Upper bound for every segment, ring and subdivision count below.
inline constexpr uint32_t kMaxPrimitiveSegments = 512;

/// A box with the given edge lengths. Each face has vertices of its own, so edges are hard.
[[nodiscard]] MeshData makeBox(const Vec3& size = Vec3(1.0f));

/// A rectangle in the XZ plane that faces +Y, `size.x` wide and `size.y` deep, split
/// into `subdivisions` cells along each side (at least 1).
[[nodiscard]] MeshData makePlane(const Vec2& size = Vec2(1.0f), UVec2 subdivisions = UVec2(1));

/// A sphere of latitude rings (at least 2) and longitude segments (at least 3). `u` runs
/// around the vertical axis and `v` from the top pole to the bottom one.
[[nodiscard]] MeshData makeSphere(float radius = 0.5f, uint32_t segments = 32, uint32_t rings = 16);

/// A cylinder around the Y axis with flat caps, `height` tall.
[[nodiscard]] MeshData makeCylinder(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);

/// A cone around the Y axis with its tip at the top and a flat base, `height` tall.
[[nodiscard]] MeshData makeCone(float radius = 0.5f, float height = 1.0f, uint32_t segments = 32);

/// A cylinder around the Y axis closed by half spheres. `height` is the whole height,
/// including both caps; less than a diameter gives a sphere. `rings` counts the latitude
/// rings of each half sphere (at least 1).
[[nodiscard]] MeshData makeCapsule(float radius = 0.5f, float height = 2.0f, uint32_t segments = 32,
                                   uint32_t rings = 8);

/// A ring around the Y axis: a tube of `minorRadius` whose centre line is a circle of
/// `majorRadius`.
[[nodiscard]] MeshData makeTorus(float majorRadius = 0.375f, float minorRadius = 0.125f, uint32_t majorSegments = 48,
                                 uint32_t minorSegments = 24);

} // namespace cordite
