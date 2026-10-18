#include "cordite/renderer/Decals.h"

#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// The smallest a box may be, as the volume its world matrix makes of a cube of one metre:
/// one that is flat has no inside to lay anything on.
constexpr float kMinBoxVolume = 1.0e-9f;

/// The way the depth of a point in a decal grows, from the inverse of its world matrix:
/// its third row is how much the box's Z changes along each axis of the world, and the
/// depth is minus that Z. decalProjection() of shaders/common/Decals.slang does the same.
Vec3 projectionOf(const Mat4& decalFromWorld) {
    const Vec3 zChange(decalFromWorld[0][2], decalFromWorld[1][2], decalFromWorld[2][2]);
    return -normalizeOr(zChange, Vec3(0.0f, 0.0f, 1.0f));
}

} // namespace

DecalPoint decalPoint(const Mat4& decalFromWorld, const Vec3& worldPosition) {
    const Vec3 local(decalFromWorld * Vec4(worldPosition, 1.0f));
    DecalPoint point;
    point.uv = Vec2(local.x + 0.5f, 0.5f - local.y);
    // Projected along -Z: it sets out from the face at +Z.
    point.depth = -local.z;
    // Comparisons with what is no number are false: such a point is outside.
    point.inside = std::abs(local.x) <= 0.5f && std::abs(local.y) <= 0.5f && std::abs(local.z) <= 0.5f;
    return point;
}

float decalFade(const Vec3& surfaceNormal, const Vec3& projection, float fadeAngle) {
    const float angle = std::isfinite(fadeAngle) ? std::clamp(fadeAngle, kDecalFadeWidth, 90.0f) : 60.0f;
    const float full = std::cos(glm::radians(angle - kDecalFadeWidth));
    const float none = std::cos(glm::radians(angle));
    const float facing = -glm::dot(surfaceNormal, projection);
    const float t = std::clamp((facing - none) / (full - none), 0.0f, 1.0f);
    // A normal that is no number gets nothing.
    return std::isnan(t) ? 0.0f : t * t * (3.0f - 2.0f * t);
}

void SceneDecals::collect(const World& world) {
    m_decals.clear();
    m_found.clear();
    m_dropped = 0;

    const auto decals = world.registry().view<const DecalComponent, const WorldTransform>();
    for (const auto [handle, decal, placement] : decals.each()) {
        const Entity entity{handle};
        if (!world.isEnabledInHierarchy(entity) || decal.material.isNil()) {
            continue;
        }
        const Mat4& worldFromDecal = placement.matrix;
        // A box that is flat, or not made of numbers, has nothing inside to lay on.
        const float volume = std::abs(glm::determinant(Mat3(worldFromDecal)));
        if (!isFinite(worldFromDecal) || !(volume > kMinBoxVolume)) {
            continue;
        }
        const float opacity = std::isfinite(decal.opacity) ? std::clamp(decal.opacity, 0.0f, 1.0f) : 0.0f;
        if (!(opacity > 0.0f)) {
            continue;
        }
        const Mat4 decalFromWorld = glm::inverse(worldFromDecal);
        const SceneDecal found{
            .worldFromDecal = worldFromDecal,
            .decalFromWorld = decalFromWorld,
            .projection = projectionOf(decalFromWorld),
            .bounds = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f)).transformed(worldFromDecal),
            .material = decal.material,
            .opacity = opacity,
            .fadeAngle = std::isfinite(decal.fadeAngle) ? std::clamp(decal.fadeAngle, kDecalFadeWidth, 90.0f) : 60.0f};
        m_found.emplace_back(std::pair(decal.order, world.id(entity)), found);
    }
    // By order, then by id: the same on every run, however the components are stored.
    std::ranges::sort(m_found, {}, &std::pair<std::pair<int32_t, Uuid>, SceneDecal>::first);
    for (const auto& [key, decal] : m_found) {
        if (m_decals.size() < kMaxDecals) {
            m_decals.push_back(decal);
        } else {
            ++m_dropped;
        }
    }
}

} // namespace cordite
