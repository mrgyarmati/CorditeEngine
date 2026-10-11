#pragma once

/// @file
/// Helpers shared by the tests of the assets module.

#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Mesh.h"
#include "cordite/core/FileSystem.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cordite::test {

/// The three positions of every triangle of a mesh, each triangle rotated so that its
/// smallest corner comes first, and the list sorted. Two meshes that draw the same
/// surface give the same list however their vertices and triangles are ordered.
inline std::vector<std::array<Vec3, 3>> triangleSoup(const MeshData& mesh) {
    const auto less = [](const Vec3& a, const Vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); };

    std::vector<std::array<Vec3, 3>> triangles;
    for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
        std::array<Vec3, 3> corners = {mesh.vertices[mesh.indices[first]].position,
                                       mesh.vertices[mesh.indices[first + 1]].position,
                                       mesh.vertices[mesh.indices[first + 2]].position};
        // Rotating keeps the winding; sorting the corners would lose it.
        std::ranges::rotate(corners, std::ranges::min_element(corners, less));
        triangles.push_back(corners);
    }
    std::ranges::sort(triangles,
                      [&](const auto& a, const auto& b) { return std::ranges::lexicographical_compare(a, b, less); });
    return triangles;
}

/// True when two meshes draw the same triangles, facing the same way, within `tolerance`.
inline bool sameSurface(const MeshData& a, const MeshData& b, float tolerance = 1.0e-5f) {
    const auto first = triangleSoup(a);
    const auto second = triangleSoup(b);
    if (first.size() != second.size()) {
        return false;
    }
    for (size_t index = 0; index < first.size(); ++index) {
        for (size_t corner = 0; corner < 3; ++corner) {
            if (!nearlyEqual(first[index][corner], second[index][corner], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

/// Checks what every shader assumes of a vertex: a unit normal, and a unit tangent
/// perpendicular to it with a handedness of exactly one.
inline void checkTangentFrames(const MeshData& mesh) {
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        const MeshVertex& vertex = mesh.vertices[index];
        const Vec3 tangent(vertex.tangent);
        CAPTURE(index);
        REQUIRE(std::abs(glm::length(vertex.normal) - 1.0f) < 1.0e-4f);
        REQUIRE(std::abs(glm::length(tangent) - 1.0f) < 1.0e-4f);
        REQUIRE(std::abs(glm::dot(vertex.normal, tangent)) < 1.0e-4f);
        REQUIRE(std::abs(vertex.tangent.w) == 1.0f);
    }
}

/// Checks that tangents agree with the texture coordinates: on every triangle the tangent
/// points the way `u` grows, and the bitangent `cross(normal, tangent) * w` the way `v`
/// shrinks, which is up in the image. Also checks that every triangle faces the way its
/// normals point.
inline void checkTangentsFollowUvs(const MeshData& mesh) {
    for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[first]];
        const MeshVertex& b = mesh.vertices[mesh.indices[first + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[first + 2]];

        const Vec3 edge1 = b.position - a.position;
        const Vec3 edge2 = c.position - a.position;
        const Vec3 faceNormal = glm::cross(edge1, edge2);
        const Vec2 uv1 = b.uv0 - a.uv0;
        const Vec2 uv2 = c.uv0 - a.uv0;
        const float determinant = uv1.x * uv2.y - uv2.x * uv1.y;
        // Triangles without area, on the surface or in the texture, define no directions.
        if (glm::length(faceNormal) < 1.0e-7f || std::abs(determinant) < 1.0e-7f) {
            continue;
        }

        const Vec3 alongU = (edge1 * uv2.y - edge2 * uv1.y) / determinant;
        const Vec3 alongV = (edge2 * uv1.x - edge1 * uv2.x) / determinant;
        CAPTURE(first);
        for (const MeshVertex* vertex : {&a, &b, &c}) {
            const Vec3 tangent(vertex->tangent);
            const Vec3 bitangent = glm::cross(vertex->normal, tangent) * vertex->tangent.w;
            REQUIRE(glm::dot(faceNormal, vertex->normal) > 0.0f);
            REQUIRE(glm::dot(tangent, alongU) > 0.0f);
            REQUIRE(glm::dot(bitangent, -alongV) > 0.0f);
        }
    }
}

/// A PNG file of one color, as bytes.
inline std::vector<std::byte> encodeSolidPng(uint32_t width, uint32_t height, const Vec4& color) {
    Image image = unwrap(Image::create(width, height, ImageFormat::Rgba8));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            image.setPixel(x, y, color);
        }
    }
    return unwrap(image.encodePng());
}

inline void writeSolidPng(const std::filesystem::path& path, const Vec4& color, uint32_t size = 4) {
    REQUIRE(writeBinaryFile(path, encodeSolidPng(size, size, color)).has_value());
}

/// Base64, for the data URIs glTF files can carry their buffers and images in.
inline std::string encodeBase64(std::span<const std::byte> bytes) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (size_t index = 0; index < bytes.size(); index += 3) {
        uint32_t group = 0;
        const size_t available = std::min<size_t>(3, bytes.size() - index);
        for (size_t offset = 0; offset < 3; ++offset) {
            group = (group << 8) | (offset < available ? static_cast<uint32_t>(bytes[index + offset]) : 0u);
        }
        for (size_t digit = 0; digit < 4; ++digit) {
            text += digit <= available ? kAlphabet[(group >> (18 - 6 * digit)) & 0x3F] : '=';
        }
    }
    return text;
}

/// Appends the bytes of trivially copyable values to a buffer.
template <typename T>
void appendBytes(std::vector<std::byte>& buffer, std::initializer_list<T> values) {
    for (const T& value : values) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
    }
}

} // namespace cordite::test
