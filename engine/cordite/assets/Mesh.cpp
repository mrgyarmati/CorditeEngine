#include "cordite/assets/Mesh.h"

#include "cordite/core/Assert.h"

#include <cmath>

namespace cordite {
namespace {

bool isFiniteVertex(const MeshVertex& vertex) {
    return isFinite(vertex.position) && isFinite(vertex.normal) && isFinite(Vec3(vertex.tangent)) &&
           std::isfinite(vertex.tangent.w) && std::isfinite(vertex.uv0.x) && std::isfinite(vertex.uv0.y) &&
           std::isfinite(vertex.uv1.x) && std::isfinite(vertex.uv1.y);
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
    CORDITE_ASSERT(indices.size() % 3 == 0, "{} indices do not make whole triangles", indices.size());
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
    return static_cast<uint32_t>(mesh.submeshes.size() - 1);
}

} // namespace cordite
