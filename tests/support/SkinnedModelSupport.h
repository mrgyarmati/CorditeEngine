#pragma once

/// @file
/// A small skinned model for tests: a bar on three joints, and a clip that bends it.
///
/// @code
/// const GltfDocument document = test::makeSkinnedBar();
/// REQUIRE(exportGltf(document, directory.path() / "bar.gltf").has_value());
/// @endcode
///
/// The bar is two metres tall and stands on the origin, a square of 0.2 metres across.
/// Its joints are one above the other, a metre apart - "Root" at the bottom, "Middle",
/// "Tip" - and each ring of its vertices follows the joints below and above it by how
/// near they are. The clip "Bend" turns the middle joint a quarter turn about +Z in one
/// second, which tips the upper half of the bar over to -X; "Sway" moves the root a
/// metre along +X and back, stepped.

#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"

#include "support/TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace cordite::test {

/// How tall the bar is, how wide, and where its rings of vertices are.
inline constexpr float kBarHeight = 2.0f;
inline constexpr float kBarHalfWidth = 0.1f;
inline constexpr std::array<float, 5> kBarRings = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};

/// The nodes of the document makeSkinnedBar() returns.
inline constexpr uint32_t kBarMeshNode = 0;
inline constexpr uint32_t kBarRootNode = 1;
inline constexpr uint32_t kBarMiddleNode = 2;
inline constexpr uint32_t kBarTipNode = 3;

/// How a point of the bar at a height follows the three joints.
inline MeshSkinVertex barSkinAt(float height) {
    MeshSkinVertex skin;
    // Between the joint below and the joint above, by how near each is.
    const float along = std::clamp(height, 0.0f, kBarHeight);
    const auto below = static_cast<uint16_t>(std::min(along, 1.999f));
    const float share = along - static_cast<float>(below);
    // As normalizeSkinWeights() leaves it: a joint without weight is joint zero.
    if (share <= 0.0f) {
        skin.joints = {below, 0, 0, 0};
    } else if (share >= 1.0f) {
        skin.joints = {static_cast<uint16_t>(below + 1), 0, 0, 0};
    } else {
        skin.joints = {below, static_cast<uint16_t>(below + 1), 0, 0};
        skin.weights = {1.0f - share, share, 0.0f, 0.0f};
    }
    return skin;
}

/// The mesh of the bar: four sides, no caps, a ring of vertices at each of kBarRings.
inline MeshData makeBarMesh() {
    // The corners of a ring, counter-clockwise seen from above.
    const std::array<Vec2, 4> corners = {Vec2(1.0f, 1.0f), Vec2(-1.0f, 1.0f), Vec2(-1.0f, -1.0f), Vec2(1.0f, -1.0f)};
    std::vector<MeshVertex> vertices;
    std::vector<MeshSkinVertex> skin;
    std::vector<uint32_t> indices;
    // Each side has vertices of its own, so that its normal is the side's.
    for (size_t side = 0; side < corners.size(); ++side) {
        const Vec2 from = corners[side];
        const Vec2 to = corners[(side + 1) % corners.size()];
        const Vec3 normal = glm::normalize(Vec3(from.x + to.x, 0.0f, from.y + to.y));
        const auto first = static_cast<uint32_t>(vertices.size());
        for (size_t ring = 0; ring < kBarRings.size(); ++ring) {
            for (const Vec2& corner : {from, to}) {
                MeshVertex vertex;
                vertex.position = Vec3(corner.x * kBarHalfWidth, kBarRings[ring], corner.y * kBarHalfWidth);
                vertex.normal = normal;
                vertex.tangent = Vec4(0.0f, 1.0f, 0.0f, 1.0f);
                vertex.uv0 = Vec2(static_cast<float>(side) * 0.25f + (corner == to ? 0.25f : 0.0f),
                                  1.0f - kBarRings[ring] / kBarHeight);
                vertices.push_back(vertex);
                skin.push_back(barSkinAt(kBarRings[ring]));
            }
            if (ring > 0) {
                const uint32_t lower = first + static_cast<uint32_t>(ring - 1) * 2;
                const uint32_t upper = lower + 2;
                // Counter-clockwise seen from outside.
                for (const uint32_t index : {lower, upper, lower + 1, lower + 1, upper, upper + 1}) {
                    indices.push_back(index);
                }
            }
        }
    }

    MeshData mesh;
    mesh.name = "Bar";
    appendSubmesh(mesh, vertices, skin, indices);
    // As an importer leaves a mesh: what comes back from a file is this again.
    repairTangentFrames(mesh);
    optimizeMesh(mesh);
    return mesh;
}

/// The whole model: the mesh at node 0, its three joints at nodes 1 to 3, a skin and
/// two animations.
inline GltfDocument makeSkinnedBar() {
    GltfDocument document;
    document.meshes.push_back({.mesh = makeBarMesh(), .materials = {std::nullopt}});

    document.nodes.resize(4);
    document.nodes[kBarMeshNode] = {.name = "Bar", .mesh = 0, .skin = 0};
    document.nodes[kBarRootNode] = {.name = "Root", .children = {kBarMiddleNode}};
    document.nodes[kBarMiddleNode] = {.name = "Middle",
                                      .transform = {.position = Vec3(0.0f, 1.0f, 0.0f)},
                                      .parent = kBarRootNode,
                                      .children = {kBarTipNode}};
    document.nodes[kBarTipNode] = {
        .name = "Tip", .transform = {.position = Vec3(0.0f, 1.0f, 0.0f)}, .parent = kBarMiddleNode};
    document.roots = {kBarMeshNode, kBarRootNode};

    // Each joint is bound where it stands: one, and two, metres up.
    const std::vector<uint32_t> joints = {kBarRootNode, kBarMiddleNode, kBarTipNode};
    std::vector<Mat4> inverseBind(3, Mat4(1.0f));
    inverseBind[1][3] = Vec4(0.0f, -1.0f, 0.0f, 1.0f);
    inverseBind[2][3] = Vec4(0.0f, -2.0f, 0.0f, 1.0f);
    document.skins.push_back(
        {.skeleton = unwrap(skeletonOf(document, joints, inverseBind, "Bar skin")), .joints = joints});

    GltfAnimation bend;
    bend.animation.name = "Bend";
    bend.animation.duration = 1.0f;
    AnimationTrack middle;
    middle.joint = "Middle";
    for (const float time : {0.0f, 0.5f, 1.0f}) {
        const Quat turn = glm::angleAxis(time * kHalfPi, Vec3(0.0f, 0.0f, 1.0f));
        middle.rotation.addKey(time, Vec4(turn.x, turn.y, turn.z, turn.w));
    }
    bend.animation.tracks.push_back(middle);
    bend.nodes = {kBarMiddleNode};
    document.animations.push_back(bend);

    GltfAnimation sway;
    sway.animation.name = "Sway";
    sway.animation.duration = 2.0f;
    AnimationTrack root;
    root.joint = "Root";
    root.translation.addKey(0.0f, Vec3(0.0f), Interpolation::Step);
    root.translation.addKey(1.0f, Vec3(1.0f, 0.0f, 0.0f), Interpolation::Step);
    root.translation.addKey(2.0f, Vec3(0.0f), Interpolation::Step);
    root.scale.addKey(0.0f, Vec3(1.0f));
    root.scale.addKey(2.0f, Vec3(1.0f, 2.0f, 1.0f));
    sway.animation.tracks.push_back(root);
    sway.nodes = {kBarRootNode};
    document.animations.push_back(sway);
    return document;
}

/// The corners of every triangle of a skinned mesh - where each is and how it follows its
/// joints - as rows of numbers, sorted. Two meshes that are the same skinned surface
/// give the same list however their vertices and triangles are ordered.
inline std::vector<std::array<float, 11>> skinnedCorners(const MeshData& mesh) {
    std::vector<std::array<float, 11>> corners;
    for (const uint32_t index : mesh.indices) {
        const MeshVertex& vertex = mesh.vertices[index];
        const MeshSkinVertex skin = index < mesh.skin.size() ? mesh.skin[index] : MeshSkinVertex();
        // The joints a vertex follows in the order of their numbers: which of the four
        // places a joint is written in does not matter.
        std::array<std::pair<uint16_t, float>, 4> influences{};
        for (size_t slot = 0; slot < influences.size(); ++slot) {
            influences[slot] = {skin.weights[slot] > 0.0f ? skin.joints[slot] : uint16_t{0}, skin.weights[slot]};
        }
        std::ranges::sort(influences, [](const auto& a, const auto& b) {
            return std::tie(a.second, a.first) > std::tie(b.second, b.first);
        });
        std::array<float, 11> row{vertex.position.x, vertex.position.y, vertex.position.z};
        for (size_t slot = 0; slot < influences.size(); ++slot) {
            row[3 + slot * 2] = static_cast<float>(influences[slot].first);
            row[4 + slot * 2] = influences[slot].second;
        }
        corners.push_back(row);
    }
    std::ranges::sort(corners);
    return corners;
}

} // namespace cordite::test
