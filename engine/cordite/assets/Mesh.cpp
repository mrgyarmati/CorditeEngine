#include "cordite/assets/Mesh.h"

#include "cordite/assets/Skeleton.h"
#include "cordite/core/Assert.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

bool isFiniteVertex(const MeshVertex& vertex) {
    return isFinite(vertex.position) && isFinite(vertex.normal) && isFinite(Vec3(vertex.tangent)) &&
           std::isfinite(vertex.tangent.w) && std::isfinite(vertex.uv0.x) && std::isfinite(vertex.uv0.y) &&
           std::isfinite(vertex.uv1.x) && std::isfinite(vertex.uv1.y);
}

/// How far the weights of a vertex may be from one in all.
constexpr float kWeightTolerance = 1.0e-3f;

/// What is wrong with how a vertex follows its joints, in a few words, or nothing.
std::string_view problemOf(const MeshSkinVertex& skin) {
    float sum = 0.0f;
    for (size_t slot = 0; slot < skin.weights.size(); ++slot) {
        const float weight = skin.weights[slot];
        if (!std::isfinite(weight) || weight < 0.0f) {
            return "has a weight that is negative or no number";
        }
        if (skin.joints[slot] >= kMaxSkeletonJoints) {
            return "follows a joint beyond the most a skeleton can have";
        }
        sum += weight;
    }
    return std::abs(sum - 1.0f) <= kWeightTolerance ? std::string_view() : "has weights that are not one in all";
}

/// What is wrong with the level of detail `number` - counted from one - of `mesh`,
/// which comes after a level whose error is `previousError`: nothing when it is as the
/// mesh needs it.
std::string problemOfLevel(const MeshData& mesh, const MeshLod& lod, size_t number, float previousError) {
    if (lod.indices.size() > kMaxMeshIndices) {
        return fmt::format("level of detail {} has {} indices, which exceed the limit of {}", number,
                           lod.indices.size(), kMaxMeshIndices);
    }
    if (lod.submeshes.size() != mesh.submeshes.size()) {
        return fmt::format("level of detail {} has {} submeshes, the mesh {}", number, lod.submeshes.size(),
                           mesh.submeshes.size());
    }
    size_t expected = 0;
    for (size_t index = 0; index < lod.submeshes.size(); ++index) {
        const IndexRange& range = lod.submeshes[index];
        if (range.firstIndex != expected || range.indexCount % 3 != 0) {
            return fmt::format("submesh {} of level of detail {} has indices {} to {}, expected whole "
                               "triangles from index {}",
                               index, number, range.firstIndex,
                               static_cast<size_t>(range.firstIndex) + range.indexCount, expected);
        }
        expected += range.indexCount;
    }
    if (expected != lod.indices.size()) {
        return fmt::format("the submeshes of level of detail {} cover {} indices, but it has {}", number, expected,
                           lod.indices.size());
    }
    for (size_t index = 0; index < lod.indices.size(); ++index) {
        if (lod.indices[index] >= mesh.vertices.size()) {
            return fmt::format("index {} of level of detail {} refers to vertex {}, but there are only {}", index,
                               number, lod.indices[index], mesh.vertices.size());
        }
    }
    if (!std::isfinite(lod.error) || lod.error < previousError) {
        return fmt::format("level of detail {} has an error of {}, which is not a number at least as "
                           "large as the {} of the level before",
                           number, lod.error, previousError);
    }
    return {};
}

} // namespace

Status validateMesh(const MeshData& mesh) {
    const auto fail = [&mesh](std::string message) {
        return makeError(ErrorCode::InvalidArgument, "mesh '{}': {}", mesh.name, message);
    };

    if (mesh.vertices.empty()) {
        return fail("it has no vertices");
    }
    if (mesh.vertices.size() > kMaxMeshVertices) {
        return fail(fmt::format("{} vertices exceed the limit of {}", mesh.vertices.size(), kMaxMeshVertices));
    }
    if (mesh.indices.size() > kMaxMeshIndices) {
        return fail(fmt::format("{} indices exceed the limit of {}", mesh.indices.size(), kMaxMeshIndices));
    }
    if (mesh.indices.size() % 3 != 0) {
        return fail(fmt::format("{} indices do not make whole triangles", mesh.indices.size()));
    }
    if (mesh.submeshes.empty()) {
        return fail("it has no submeshes");
    }

    size_t expectedFirstIndex = 0;
    for (size_t index = 0; index < mesh.submeshes.size(); ++index) {
        const Submesh& submesh = mesh.submeshes[index];
        if (submesh.firstIndex != expectedFirstIndex) {
            return fail(fmt::format("submesh {} starts at index {}, expected {}", index, submesh.firstIndex,
                                    expectedFirstIndex));
        }
        if (submesh.indexCount % 3 != 0) {
            return fail(
                fmt::format("submesh {} has {} indices, which do not make whole triangles", index, submesh.indexCount));
        }
        expectedFirstIndex += submesh.indexCount;
    }
    if (expectedFirstIndex != mesh.indices.size()) {
        return fail(
            fmt::format("its submeshes cover {} indices, but it has {}", expectedFirstIndex, mesh.indices.size()));
    }

    for (size_t index = 0; index < mesh.indices.size(); ++index) {
        if (mesh.indices[index] >= mesh.vertices.size()) {
            return fail(fmt::format("index {} refers to vertex {}, but there are only {}", index, mesh.indices[index],
                                    mesh.vertices.size()));
        }
    }
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        if (!isFiniteVertex(mesh.vertices[index])) {
            return fail(fmt::format("vertex {} contains a number that is not finite", index));
        }
    }

    if (!mesh.skin.empty() && mesh.skin.size() != mesh.vertices.size()) {
        return fail(fmt::format("it says how {} vertices follow their joints, but has {}", mesh.skin.size(),
                                mesh.vertices.size()));
    }
    for (size_t index = 0; index < mesh.skin.size(); ++index) {
        if (const std::string_view problem = problemOf(mesh.skin[index]); !problem.empty()) {
            return fail(fmt::format("vertex {} {}", index, problem));
        }
    }

    if (mesh.lods.size() > kMaxMeshLods) {
        return fail(fmt::format("{} levels of detail exceed the limit of {}", mesh.lods.size(), kMaxMeshLods));
    }
    float previousError = 0.0f;
    for (size_t level = 0; level < mesh.lods.size(); ++level) {
        // Levels are counted from one: the mesh itself is level zero.
        if (std::string problem = problemOfLevel(mesh, mesh.lods[level], level + 1, previousError); !problem.empty()) {
            return fail(std::move(problem));
        }
        previousError = mesh.lods[level].error;
    }
    return {};
}

void updateBounds(MeshData& mesh) {
    mesh.bounds = Aabb();
    for (Submesh& submesh : mesh.submeshes) {
        submesh.bounds = Aabb();
        const size_t end = static_cast<size_t>(submesh.firstIndex) + submesh.indexCount;
        CORDITE_ASSERT(end <= mesh.indices.size(), "submesh range {}..{} is outside the {} indices of '{}'",
                       submesh.firstIndex, end, mesh.indices.size(), mesh.name);
        for (size_t index = submesh.firstIndex; index < end && index < mesh.indices.size(); ++index) {
            const uint32_t vertex = mesh.indices[index];
            if (vertex < mesh.vertices.size()) {
                submesh.bounds.expand(mesh.vertices[vertex].position);
            }
        }
        if (!submesh.bounds.isEmpty()) {
            mesh.bounds.expand(submesh.bounds);
        }
    }
}

uint32_t appendSubmesh(MeshData& mesh, std::span<const MeshVertex> vertices, std::span<const uint32_t> indices) {
    return appendSubmesh(mesh, vertices, {}, indices);
}

uint32_t appendSubmesh(MeshData& mesh, std::span<const MeshVertex> vertices, std::span<const MeshSkinVertex> skin,
                       std::span<const uint32_t> indices) {
    CORDITE_ASSERT(indices.size() % 3 == 0, "{} indices do not make whole triangles", indices.size());
    CORDITE_ASSERT(skin.empty() || skin.size() == vertices.size(),
                   "{} vertices cannot follow their joints as {} entries say", vertices.size(), skin.size());
    CORDITE_ASSERT(mesh.vertices.empty() || vertices.empty() || mesh.isSkinned() == !skin.empty(),
                   "mesh '{}' is skinned as a whole or not at all", mesh.name);
    // Where the assertions are compiled out, a mesh still has an entry for every vertex
    // or none: what is missing is made up of vertices that follow joint zero.
    const bool skinned = mesh.vertices.empty() ? !skin.empty() : mesh.isSkinned();
    if (skinned) {
        mesh.skin.resize(mesh.vertices.size());
        mesh.skin.insert(mesh.skin.end(), skin.begin(), skin.begin() + std::min(skin.size(), vertices.size()));
        mesh.skin.resize(mesh.vertices.size() + vertices.size());
    }
    const auto firstVertex = static_cast<uint32_t>(mesh.vertices.size());

    Submesh submesh;
    submesh.firstIndex = static_cast<uint32_t>(mesh.indices.size());
    submesh.indexCount = static_cast<uint32_t>(indices.size() - indices.size() % 3);

    mesh.vertices.insert(mesh.vertices.end(), vertices.begin(), vertices.end());
    mesh.indices.reserve(mesh.indices.size() + submesh.indexCount);
    for (size_t index = 0; index < submesh.indexCount; ++index) {
        CORDITE_ASSERT(indices[index] < vertices.size(), "index {} is outside the {} vertices given", indices[index],
                       vertices.size());
        mesh.indices.push_back(firstVertex + indices[index]);
        if (indices[index] < vertices.size()) {
            submesh.bounds.expand(vertices[indices[index]].position);
        }
    }
    if (!submesh.bounds.isEmpty()) {
        mesh.bounds.expand(submesh.bounds);
    }

    mesh.submeshes.push_back(submesh);
    // The levels of detail have a range for every submesh, and this one has none.
    mesh.lods.clear();
    return static_cast<uint32_t>(mesh.submeshes.size() - 1);
}

uint32_t jointsUsed(const MeshData& mesh) {
    uint32_t count = 0;
    for (const MeshSkinVertex& vertex : mesh.skin) {
        for (size_t slot = 0; slot < vertex.weights.size(); ++slot) {
            if (vertex.weights[slot] > 0.0f) {
                count = std::max(count, uint32_t{vertex.joints[slot]} + 1);
            }
        }
    }
    return count;
}

std::vector<Aabb> jointBounds(const MeshData& mesh) {
    if (!mesh.isSkinned()) {
        return {};
    }
    std::vector<Aabb> bounds(jointsUsed(mesh));
    for (size_t index = 0; index < mesh.skin.size(); ++index) {
        const MeshSkinVertex& skin = mesh.skin[index];
        for (size_t slot = 0; slot < skin.weights.size(); ++slot) {
            // jointsUsed() counted every joint with a weight, so the index is inside.
            if (skin.weights[slot] > 0.0f) {
                bounds[skin.joints[slot]].expand(mesh.vertices[index].position);
            }
        }
    }
    return bounds;
}

uint32_t normalizeSkinWeights(MeshData& mesh) {
    uint32_t withoutWeights = 0;
    for (MeshSkinVertex& vertex : mesh.skin) {
        for (float& weight : vertex.weights) {
            weight = std::isfinite(weight) ? std::max(weight, 0.0f) : 0.0f;
        }
        const float sum = std::accumulate(vertex.weights.begin(), vertex.weights.end(), 0.0f);
        if (!std::isfinite(sum) || sum <= 0.0f) {
            vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            ++withoutWeights;
        } else {
            for (float& weight : vertex.weights) {
                weight /= sum;
            }
        }
        for (size_t slot = 0; slot < vertex.weights.size(); ++slot) {
            if (vertex.weights[slot] <= 0.0f) {
                vertex.joints[slot] = 0;
            }
        }
    }
    return withoutWeights;
}

} // namespace cordite
