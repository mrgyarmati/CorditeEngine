#include "cordite/assets/Primitives.h"

#include "cordite/assets/MeshProcessing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Sizes below this are raised to it, so that no primitive collapses into a point.
constexpr float kMinimumSize = 0.001f;

float atLeastMinimum(float size) {
    return std::isfinite(size) ? std::max(size, kMinimumSize) : kMinimumSize;
}

uint32_t clampSegments(uint32_t count, uint32_t minimum) {
    return std::clamp(count, minimum, kMaxPrimitiveSegments);
}

float fraction(uint32_t index, uint32_t count) {
    return static_cast<float>(index) / static_cast<float>(count);
}

/// The angle of step `index` of a full turn made in `count` steps. The last step is
/// the first one again, exactly: the sine of a rounded two pi is not zero, and a seam
/// that does not quite close shows as a crack.
float turn(uint32_t index, uint32_t count) {
    return kTwoPi * fraction(index % count, count);
}

/// Sine and cosine of the latitude at step `index` of the `count` steps from straight
/// up to straight down. Exact at both ends, so that every vertex of a pole is the
/// same point.
Vec2 poleToPole(uint32_t index, uint32_t count) {
    if (index == 0) {
        return {0.0f, 1.0f};
    }
    if (index >= count) {
        return {0.0f, -1.0f};
    }
    const float latitude = kPi * fraction(index, count);
    return {std::sin(latitude), std::cos(latitude)};
}

/// The horizontal direction at `angle` around the Y axis. Angle zero is +Z, and angles
/// grow towards +X, which is to the right for someone looking at the surface from outside.
Vec3 around(float angle) {
    return {std::sin(angle), 0.0f, std::cos(angle)};
}

/// The direction in which around() moves as the angle grows: the tangent of a surface
/// whose `u` runs around the Y axis.
Vec4 tangentAround(float angle) {
    return {std::cos(angle), 0.0f, -std::sin(angle), 1.0f};
}

/// Which rows of a grid are a single point (the pole of a sphere, the tip of a cone), so
/// that the triangles which would have no area there are left out.
struct CollapsedRows {
    bool top = false;
    bool bottom = false;
};

/// Collects the vertices and triangles of a primitive.
class PrimitiveBuilder {
public:
    /// Adds a grid of (columns + 1) x (rows + 1) vertices and the two triangles of each
    /// cell. `vertexAt(column, row)` supplies the vertices; as seen from the front of the
    /// surface, columns run to the right and rows downwards.
    template <typename VertexAt>
    void addGrid(uint32_t columns, uint32_t rows, VertexAt&& vertexAt, CollapsedRows collapsed = {}) {
        const auto first = static_cast<uint32_t>(m_vertices.size());
        for (uint32_t row = 0; row <= rows; ++row) {
            for (uint32_t column = 0; column <= columns; ++column) {
                m_vertices.push_back(vertexAt(column, row));
            }
        }

        const uint32_t stride = columns + 1;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t topLeft = first + row * stride + column;
                const uint32_t topRight = topLeft + 1;
                const uint32_t bottomLeft = topLeft + stride;
                const uint32_t bottomRight = bottomLeft + 1;
                if (!collapsed.bottom || row + 1 != rows) {
                    addTriangle(topLeft, bottomLeft, bottomRight);
                }
                if (!collapsed.top || row != 0) {
                    addTriangle(topLeft, bottomRight, topRight);
                }
            }
        }
    }

    /// Adds a flat disc around the Y axis at height `y`, facing up or down.
    void addDisc(float radius, float y, uint32_t segments, bool facingUp) {
        const float facing = facingUp ? 1.0f : -1.0f;
        const auto discVertex = [&](const Vec3& direction) {
            MeshVertex vertex;
            vertex.position = Vec3(radius * direction.x, y, radius * direction.z);
            vertex.normal = Vec3(0.0f, facing, 0.0f);
            vertex.tangent = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
            // Seen from the side it faces, +X is to the right in both cases; what is up
            // in the image is -Z from above and +Z from below.
            vertex.uv0 = Vec2(0.5f + 0.5f * direction.x, 0.5f + 0.5f * facing * direction.z);
            vertex.uv1 = vertex.uv0;
            return vertex;
        };

        const auto center = static_cast<uint32_t>(m_vertices.size());
        m_vertices.push_back(discVertex(Vec3(0.0f)));
        for (uint32_t segment = 0; segment < segments; ++segment) {
            m_vertices.push_back(discVertex(around(turn(segment, segments))));
        }
        for (uint32_t segment = 0; segment < segments; ++segment) {
            const uint32_t current = center + 1 + segment;
            const uint32_t next = center + 1 + (segment + 1) % segments;
            if (facingUp) {
                addTriangle(center, current, next);
            } else {
                addTriangle(center, next, current);
            }
        }
    }

    [[nodiscard]] MeshData build(std::string name) const {
        MeshData mesh;
        mesh.name = std::move(name);
        appendSubmesh(mesh, m_vertices, m_indices);
        // A grid leaves vertices behind where rows collapse into a point; optimizing
        // drops them, and puts the rest in an order the GPU likes.
        optimizeMesh(mesh);
        return mesh;
    }

private:
    void addTriangle(uint32_t a, uint32_t b, uint32_t c) {
        m_indices.push_back(a);
        m_indices.push_back(b);
        m_indices.push_back(c);
    }

    std::vector<MeshVertex> m_vertices;
    std::vector<uint32_t> m_indices;
};

} // namespace

MeshData makeBox(const Vec3& size) {
    const Vec3 half = 0.5f * Vec3(atLeastMinimum(size.x), atLeastMinimum(size.y), atLeastMinimum(size.z));

    struct Face {
        Vec3 normal;
        /// To the right, for someone looking at the face from outside.
        Vec3 right;
    };

    constexpr std::array<Face, 6> kFaces = {{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}},
    }};

    PrimitiveBuilder builder;
    for (const Face& face : kFaces) {
        const Vec3 up = glm::cross(face.normal, face.right);
        builder.addGrid(1, 1, [&](uint32_t column, uint32_t row) {
            const auto u = static_cast<float>(column);
            const auto v = static_cast<float>(row);
            MeshVertex vertex;
            vertex.position = half * (face.normal + face.right * (2.0f * u - 1.0f) + up * (1.0f - 2.0f * v));
            vertex.normal = face.normal;
            vertex.tangent = Vec4(face.right, 1.0f);
            vertex.uv0 = Vec2(u, v);
            vertex.uv1 = vertex.uv0;
            return vertex;
        });
    }
    return builder.build("Box");
}

MeshData makePlane(const Vec2& size, UVec2 subdivisions) {
    const Vec2 extent(atLeastMinimum(size.x), atLeastMinimum(size.y));
    const uint32_t columns = clampSegments(subdivisions.x, 1);
    const uint32_t rows = clampSegments(subdivisions.y, 1);

    PrimitiveBuilder builder;
    builder.addGrid(columns, rows, [&](uint32_t column, uint32_t row) {
        const Vec2 uv(fraction(column, columns), fraction(row, rows));
        MeshVertex vertex;
        vertex.position = Vec3((uv.x - 0.5f) * extent.x, 0.0f, (uv.y - 0.5f) * extent.y);
        vertex.normal = kWorldUp;
        vertex.tangent = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
        vertex.uv0 = uv;
        vertex.uv1 = uv;
        return vertex;
    });
    return builder.build("Plane");
}

MeshData makeSphere(float radius, uint32_t segments, uint32_t rings) {
    const float size = atLeastMinimum(radius);
    const uint32_t columns = clampSegments(segments, 3);
    const uint32_t rows = clampSegments(rings, 2);

    PrimitiveBuilder builder;
    builder.addGrid(columns, rows,
                    [&](uint32_t column, uint32_t row) {
                        const float longitude = turn(column, columns);
                        const Vec2 latitude = poleToPole(row, rows);
                        MeshVertex vertex;
                        vertex.normal = around(longitude) * latitude.x + kWorldUp * latitude.y;
                        vertex.position = vertex.normal * size;
                        vertex.tangent = tangentAround(longitude);
                        vertex.uv0 = Vec2(fraction(column, columns), fraction(row, rows));
                        vertex.uv1 = vertex.uv0;
                        return vertex;
                    },
                    {.top = true, .bottom = true});
    return builder.build("Sphere");
}

MeshData makeCylinder(float radius, float height, uint32_t segments) {
    const float size = atLeastMinimum(radius);
    const float halfHeight = 0.5f * atLeastMinimum(height);
    const uint32_t columns = clampSegments(segments, 3);

    PrimitiveBuilder builder;
    builder.addGrid(columns, 1, [&](uint32_t column, uint32_t row) {
        const float angle = turn(column, columns);
        MeshVertex vertex;
        vertex.normal = around(angle);
        vertex.position = vertex.normal * size + kWorldUp * (row == 0 ? halfHeight : -halfHeight);
        vertex.tangent = tangentAround(angle);
        vertex.uv0 = Vec2(fraction(column, columns), static_cast<float>(row));
        vertex.uv1 = vertex.uv0;
        return vertex;
    });
    builder.addDisc(size, halfHeight, columns, true);
    builder.addDisc(size, -halfHeight, columns, false);
    return builder.build("Cylinder");
}

MeshData makeCone(float radius, float height, uint32_t segments) {
    const float size = atLeastMinimum(radius);
    const float tall = atLeastMinimum(height);
    const uint32_t columns = clampSegments(segments, 3);
    // The side leans inwards by the same angle everywhere.
    const Vec2 slope = glm::normalize(Vec2(tall, size));

    PrimitiveBuilder builder;
    builder.addGrid(columns, 1,
                    [&](uint32_t column, uint32_t row) {
                        MeshVertex vertex;
                        if (row == 0) {
                            // The tip is one point per segment, each with the normal of the middle of
                            // its segment; one shared normal would shade the whole cone like a ball.
                            const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(columns);
                            vertex.normal = around(kTwoPi * u) * slope.x + kWorldUp * slope.y;
                            vertex.position = kWorldUp * (0.5f * tall);
                            vertex.tangent = tangentAround(kTwoPi * u);
                            vertex.uv0 = Vec2(u, 0.0f);
                        } else {
                            const float angle = turn(column, columns);
                            vertex.normal = around(angle) * slope.x + kWorldUp * slope.y;
                            vertex.position = around(angle) * size - kWorldUp * (0.5f * tall);
                            vertex.tangent = tangentAround(angle);
                            vertex.uv0 = Vec2(fraction(column, columns), 1.0f);
                        }
                        vertex.uv1 = vertex.uv0;
                        return vertex;
                    },
                    {.top = true});
    builder.addDisc(size, -0.5f * tall, columns, false);
    return builder.build("Cone");
}

MeshData makeCapsule(float radius, float height, uint32_t segments, uint32_t rings) {
    const float size = atLeastMinimum(radius);
    const float halfStraight = std::max(0.5f * atLeastMinimum(height) - size, 0.0f);
    const uint32_t columns = clampSegments(segments, 3);
    const uint32_t capRows = clampSegments(rings, 1);

    // Rows: the top half sphere from the pole to its rim, then the bottom one from its
    // rim to the pole. The straight part is the single row of cells between the rims.
    const uint32_t rows = 2 * capRows + 1;
    const float profileLength = kPi * size + 2.0f * halfStraight;

    PrimitiveBuilder builder;
    builder.addGrid(columns, rows,
                    [&](uint32_t column, uint32_t row) {
                        const bool isTop = row <= capRows;
                        // The step along the two half spheres taken together; both rims are step capRows.
                        const uint32_t step = isTop ? row : row - 1;
                        const Vec2 latitude = poleToPole(step, 2 * capRows);
                        const float longitude = turn(column, columns);

                        MeshVertex vertex;
                        vertex.normal = around(longitude) * latitude.x + kWorldUp * latitude.y;
                        vertex.position = vertex.normal * size + kWorldUp * (isTop ? halfStraight : -halfStraight);
                        vertex.tangent = tangentAround(longitude);
                        // v follows the distance along the outline, so a texture is not stretched
                        // over the straight part.
                        const float distance =
                            size * kPi * fraction(step, 2 * capRows) + (isTop ? 0.0f : 2.0f * halfStraight);
                        vertex.uv0 = Vec2(fraction(column, columns), distance / profileLength);
                        vertex.uv1 = vertex.uv0;
                        return vertex;
                    },
                    {.top = true, .bottom = true});
    return builder.build("Capsule");
}

MeshData makeTorus(float majorRadius, float minorRadius, uint32_t majorSegments, uint32_t minorSegments) {
    const float tube = atLeastMinimum(minorRadius);
    // The centre line is never closer to the axis than the tube is thick, or the surface
    // would pass through itself.
    const float ring = std::max(atLeastMinimum(majorRadius), tube);
    const uint32_t columns = clampSegments(majorSegments, 3);
    const uint32_t rows = clampSegments(minorSegments, 3);

    PrimitiveBuilder builder;
    builder.addGrid(columns, rows, [&](uint32_t column, uint32_t row) {
        const float aroundAxis = turn(column, columns);
        // Rows go downwards over the outside of the tube, starting at its outer edge.
        const float aroundTube = -turn(row, rows);
        const Vec3 outward = around(aroundAxis);

        MeshVertex vertex;
        vertex.normal = outward * std::cos(aroundTube) + kWorldUp * std::sin(aroundTube);
        vertex.position = outward * ring + vertex.normal * tube;
        vertex.tangent = tangentAround(aroundAxis);
        vertex.uv0 = Vec2(fraction(column, columns), fraction(row, rows));
        vertex.uv1 = vertex.uv0;
        return vertex;
    });
    return builder.build("Torus");
}

} // namespace cordite
