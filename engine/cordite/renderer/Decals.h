#pragma once

/// @file
/// Decals: materials laid onto the solid surfaces inside a box - grime on a wall, a
/// crack in a floor, rust down a pipe - and the arithmetic of where they land.
/// shaders/common/Decals.slang has the same; DecalPass draws them into the G-buffer.
///
/// @code
/// SceneDecals decals;
/// decals.collect(world);                                  // in the order they are laid on
/// for (const SceneDecal& decal : decals.decals()) {
///     const DecalPoint point = decalPoint(decal.decalFromWorld, position);
///     if (point.inside) {
///         const float amount = decalFade(normal, decal.projection, decal.fadeAngle);
///     }
/// }
/// @endcode
///
/// A decal is its entity's box - a cube of one metre about its origin, scaled, turned and
/// placed by the entity's transform - projected along the entity's -Z axis, as a light
/// shines and a camera looks. Its image runs from the top-left corner of the box's +Z face
/// - u along +X, v down -Y - and is laid on everything solid inside the box that faces
/// back along the projection, less as it turns away.

#include "cordite/core/Uuid.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cordite {

/// How many decals a view lays on at the most; those after them, in the order they are
/// laid on, are left out and counted (SceneDecals::dropped()).
inline constexpr uint32_t kMaxDecals = 256;

/// Over how many degrees before its fade angle a decal fades out on surfaces that turn away
/// from its projection.
inline constexpr float kDecalFadeWidth = 10.0f;

/// Where a point of the world is in a decal.
struct DecalPoint {
    /// Where on its image: (0, 0) at the top left, (1, 1) at the bottom right.
    Vec2 uv{0.0f};
    /// How far along the projection, from -0.5 at the face it is projected from to 0.5 at
    /// the far one.
    float depth = 0.0f;
    /// Whether the point is inside the box.
    bool inside = false;
};

/// Where `worldPosition` is in the decal whose box is `decalFromWorld` - the inverse of the
/// world matrix of its entity. A point that is no number is outside.
[[nodiscard]] DecalPoint decalPoint(const Mat4& decalFromWorld, const Vec3& worldPosition);

/// How much of a decal lands on a surface whose unit normal is `surfaceNormal`, when it is
/// projected along the unit vector `projection`: all of it while the surface faces back
/// along the projection within `fadeAngle` minus ten degrees, nothing beyond `fadeAngle`,
/// and between the two by a smooth step. `fadeAngle` is in degrees, from 10 to 90.
[[nodiscard]] float decalFade(const Vec3& surfaceNormal, const Vec3& projection, float fadeAngle);

/// A decal of a world, as the renderer works with it.
struct SceneDecal {
    /// The world matrix of its entity: where its box is.
    Mat4 worldFromDecal{1.0f};
    /// Its inverse.
    Mat4 decalFromWorld{1.0f};
    /// The unit vector along which it is projected: the way its depth in the box grows,
    /// which is the entity's -Z axis wherever what it hangs under does not shear the box.
    Vec3 projection{0.0f, 0.0f, -1.0f};
    /// A box of the world around its box.
    Aabb bounds;
    Uuid material;
    float opacity = 1.0f;
    float fadeAngle = 60.0f;
};

/// The decals of a world, in the order they are laid on: by their `order`, then by the ids
/// of their entities - never by how components are stored. Keep one and collect into it
/// every frame: it reuses its memory.
class SceneDecals {
public:
    /// Gathers the decals of the enabled entities of `world` that have a material, whose
    /// box has a size and whose numbers are numbers - kMaxDecals at the most. The world
    /// transforms must be up to date.
    void collect(const World& world);

    [[nodiscard]] std::span<const SceneDecal> decals() const { return m_decals; }

    /// How many decals were left out beyond kMaxDecals.
    [[nodiscard]] uint32_t dropped() const { return m_dropped; }

private:
    std::vector<SceneDecal> m_decals;
    /// Working memory of collect().
    std::vector<std::pair<std::pair<int32_t, Uuid>, SceneDecal>> m_found;
    uint32_t m_dropped = 0;
};

} // namespace cordite
