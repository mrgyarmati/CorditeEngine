#pragma once

/// @file
/// Immediate-mode line drawing for tools and debugging.
///
/// A DebugDraw is a list of colored line segments in world space. Code anywhere adds to
/// it during the frame; DebugLinePass draws the list and the owner clears it for the
/// next frame.
///
/// @code
/// DebugDraw lines;
/// lines.grid({.cellSize = 1.0f, .halfCellCount = 20});
/// lines.axes(world.worldMatrix(entity), 0.5f);
/// lines.box(bounds, Mat4(1.0f), {1.0f, 0.8f, 0.2f, 1.0f});
///
/// linePass.draw(commandList, framebuffer, lines.vertices(), {.viewProjection = view.viewProjection});
/// lines.clear();
/// @endcode
///
/// Colors are display colors (sRGB-encoded, as a color picker shows them) with straight
/// alpha: the lines are drawn onto the finished image, after any tone mapping.

#include "cordite/math/Geometry.h"
#include "cordite/math/Math.h"

#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// One end of a line segment, in the layout the GPU reads.
struct DebugVertex {
    Vec3 position{0.0f};
    /// Bytes R, G, B, A in memory (see packRgba8).
    uint32_t color = 0xffffffffu;
};

struct DebugGridDesc {
    /// A point on the grid. The grid lies in the plane through it parallel to XZ.
    Vec3 center{0.0f};
    /// Distance between neighbouring lines, in metres.
    float cellSize = 1.0f;
    /// Number of cells from the center to each edge.
    uint32_t halfCellCount = 20;
    Vec4 color{0.35f, 0.36f, 0.40f, 1.0f};
    /// Every n-th line is drawn in `majorColor`. Zero draws none that way.
    uint32_t majorLineEvery = 10;
    Vec4 majorColor{0.55f, 0.56f, 0.60f, 1.0f};
};

class DebugDraw {
public:
    static constexpr Vec4 kRed{0.90f, 0.25f, 0.25f, 1.0f};
    static constexpr Vec4 kGreen{0.35f, 0.80f, 0.30f, 1.0f};
    static constexpr Vec4 kBlue{0.30f, 0.50f, 0.95f, 1.0f};

    void line(const Vec3& from, const Vec3& to, const Vec4& color);

    /// The twelve edges of a box given in the space that `transform` maps to the world.
    void box(const Aabb& box, const Mat4& transform, const Vec4& color);

    /// Three lines from the origin of `transform` along its X (red), Y (green) and Z
    /// (blue) axes, each `length` long regardless of any scale in the matrix.
    void axes(const Mat4& transform, float length);

    /// Three short lines crossing at a point, one along each world axis.
    void cross(const Vec3& center, float size, const Vec4& color);

    /// A circle around `center` in the plane perpendicular to `normal`.
    void circle(const Vec3& center, const Vec3& normal, float radius, const Vec4& color, uint32_t segments = 32);

    /// Three perpendicular circles: the outline of a sphere from any side.
    void sphere(const Vec3& center, float radius, const Vec4& color, uint32_t segments = 32);

    /// The edges of the volume a camera sees, from the inverse of its view-projection.
    void frustum(const Mat4& inverseViewProjection, const Vec4& color);

    /// A square grid of lines parallel to the X and Z axes.
    void grid(const DebugGridDesc& desc = {});

    /// Two vertices per line segment.
    [[nodiscard]] std::span<const DebugVertex> vertices() const { return m_vertices; }

    [[nodiscard]] size_t lineCount() const { return m_vertices.size() / 2; }

    [[nodiscard]] bool isEmpty() const { return m_vertices.empty(); }

    /// Removes every line but keeps the memory for the next frame.
    void clear() { m_vertices.clear(); }

private:
    std::vector<DebugVertex> m_vertices;
};

} // namespace cordite
