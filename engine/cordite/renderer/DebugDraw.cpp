#include "cordite/renderer/DebugDraw.h"

#include "cordite/math/Color.h"
#include "cordite/math/Frustum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

struct Edge {
    uint8_t from;
    uint8_t to;
};

/// The pairs of corners joined by an edge, for boxes whose corners are numbered so that
/// bit 0 selects X, bit 1 selects Y and bit 2 selects Z. Aabb::corners and
/// frustumCorners both number them that way.
constexpr std::array<Edge, 12> kBoxEdges = {{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7}, // along X
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7}, // along Y
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7}, // along Z
}};

/// Two unit vectors perpendicular to `normal` and to each other.
std::pair<Vec3, Vec3> perpendicularBasis(const Vec3& normal) {
    const Vec3 axis = normalizeOr(normal, kWorldUp);
    // Cross with whichever world axis is less aligned with the normal.
    const Vec3 helper = std::abs(axis.y) < 0.9f ? kWorldUp : kWorldRight;
    const Vec3 first = glm::normalize(glm::cross(helper, axis));
    return {first, glm::cross(axis, first)};
}

} // namespace

void DebugDraw::line(const Vec3& from, const Vec3& to, const Vec4& color) {
    const uint32_t packed = packRgba8(color);
    m_vertices.push_back({from, packed});
    m_vertices.push_back({to, packed});
}

void DebugDraw::box(const Aabb& box, const Mat4& transform, const Vec4& color) {
    if (box.isEmpty()) {
        return;
    }

    std::array<Vec3, 8> corners = box.corners();
    for (Vec3& corner : corners) {
        corner = Vec3(transform * Vec4(corner, 1.0f));
    }
    for (const auto& [from, to] : kBoxEdges) {
        line(corners[from], corners[to], color);
    }
}

void DebugDraw::axes(const Mat4& transform, float length) {
    const Vec3 origin(transform[3]);
    line(origin, origin + normalizeOr(Vec3(transform[0]), kWorldRight) * length, kRed);
    line(origin, origin + normalizeOr(Vec3(transform[1]), kWorldUp) * length, kGreen);
    line(origin, origin + normalizeOr(Vec3(transform[2]), -kWorldForward) * length, kBlue);
}

void DebugDraw::cross(const Vec3& center, float size, const Vec4& color) {
    const float half = 0.5f * size;
    line(center - Vec3(half, 0.0f, 0.0f), center + Vec3(half, 0.0f, 0.0f), color);
    line(center - Vec3(0.0f, half, 0.0f), center + Vec3(0.0f, half, 0.0f), color);
    line(center - Vec3(0.0f, 0.0f, half), center + Vec3(0.0f, 0.0f, half), color);
}

void DebugDraw::circle(const Vec3& center, const Vec3& normal, float radius, const Vec4& color, uint32_t segments) {
    constexpr uint32_t kMinimumSegments = 3;
    const uint32_t count = std::max(segments, kMinimumSegments);
    const auto [first, second] = perpendicularBasis(normal);

    Vec3 previous = center + first * radius;
    for (uint32_t index = 1; index <= count; ++index) {
        const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(count);
        const Vec3 current = center + (first * std::cos(angle) + second * std::sin(angle)) * radius;
        line(previous, current, color);
        previous = current;
    }
}

void DebugDraw::sphere(const Vec3& center, float radius, const Vec4& color, uint32_t segments) {
    circle(center, kWorldRight, radius, color, segments);
    circle(center, kWorldUp, radius, color, segments);
    circle(center, kWorldForward, radius, color, segments);
}

void DebugDraw::frustum(const Mat4& inverseViewProjection, const Vec4& color) {
    const std::array<Vec3, 8> corners = frustumCorners(inverseViewProjection);
    for (const auto& [from, to] : kBoxEdges) {
        line(corners[from], corners[to], color);
    }
}

void DebugDraw::grid(const DebugGridDesc& desc) {
    if (desc.halfCellCount == 0 || desc.cellSize <= 0.0f) {
        return;
    }

    const auto cells = static_cast<int32_t>(desc.halfCellCount);
    const float extent = static_cast<float>(cells) * desc.cellSize;
    for (int32_t index = -cells; index <= cells; ++index) {
        const bool isMajor = desc.majorLineEvery != 0 && index % static_cast<int32_t>(desc.majorLineEvery) == 0;
        const Vec4& color = isMajor ? desc.majorColor : desc.color;
        const float offset = static_cast<float>(index) * desc.cellSize;

        line(desc.center + Vec3(offset, 0.0f, -extent), desc.center + Vec3(offset, 0.0f, extent), color);
        line(desc.center + Vec3(-extent, 0.0f, offset), desc.center + Vec3(extent, 0.0f, offset), color);
    }
}

} // namespace cordite
