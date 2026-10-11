#include "cordite/assets/MeshProcessing.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <meshoptimizer.h>
#include <mikktspace.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace cordite {
namespace {

/// Shorter than this, a vector is treated as having no direction.
constexpr float kMinimumLength = 1.0e-6f;

/// The tangent for a vertex: `candidate` made perpendicular to `normal` and unit length,
/// or any perpendicular when nothing of it is left.
Vec3 tangentFor(const Vec3& normal, const Vec3& candidate) {
    const Vec3 projected = candidate - normal * glm::dot(normal, candidate);
    const float length = glm::length(projected);
    if (!std::isfinite(length) || length < kMinimumLength) {
        return perpendicularTo(normal);
    }
    return projected / length;
}

/// Merges vertices that are bit-for-bit equal. The order of the indices is kept.
void weldVertices(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) {
        return;
    }
    // Negative zero compares equal to zero but has different bits. Adding zero turns it
    // into positive zero and changes nothing else.
    static_assert(sizeof(MeshVertex) == 14 * sizeof(float), "MeshVertex must consist of floats only");
    for (MeshVertex& vertex : vertices) {
        std::array<float, 14> values{};
        std::memcpy(values.data(), &vertex, sizeof(vertex));
        for (float& value : values) {
            value += 0.0f;
        }
        std::memcpy(&vertex, values.data(), sizeof(vertex));
    }

    std::vector<unsigned int> remap(vertices.size());
    const size_t uniqueCount = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(),
                                                           vertices.data(), vertices.size(), sizeof(MeshVertex));

    std::vector<MeshVertex> welded(uniqueCount);
    meshopt_remapVertexBuffer(welded.data(), vertices.data(), vertices.size(), sizeof(MeshVertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    vertices = std::move(welded);
}

/// What MikkTSpace reads the mesh through and writes its result to.
struct TangentContext {
    const MeshData* mesh = nullptr;
    /// One per index: tangents belong to the corners of triangles, not to vertices.
    std::vector<Vec4> cornerTangents;

    [[nodiscard]] const MeshVertex& corner(int face, int vertex) const {
        const size_t index = static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex);
        return mesh->vertices[mesh->indices[index]];
    }
};

TangentContext& contextOf(const SMikkTSpaceContext* context) {
    return *static_cast<TangentContext*>(context->m_pUserData);
}

int mikkFaceCount(const SMikkTSpaceContext* context) {
    return static_cast<int>(contextOf(context).mesh->indices.size() / 3);
}

int mikkVerticesOfFace(const SMikkTSpaceContext* /*context*/, const int /*face*/) {
    return 3;
}

void mikkPosition(const SMikkTSpaceContext* context, float* out, const int face, const int vertex) {
    const Vec3& position = contextOf(context).corner(face, vertex).position;
    out[0] = position.x;
    out[1] = position.y;
    out[2] = position.z;
}

void mikkNormal(const SMikkTSpaceContext* context, float* out, const int face, const int vertex) {
    const Vec3& normal = contextOf(context).corner(face, vertex).normal;
    out[0] = normal.x;
    out[1] = normal.y;
    out[2] = normal.z;
}

void mikkTexCoord(const SMikkTSpaceContext* context, float* out, const int face, const int vertex) {
    const Vec2& uv = contextOf(context).corner(face, vertex).uv0;
    // Normal maps are baked with v growing upwards; the engine's v grows downwards.
    // Flipping it here makes the handedness MikkTSpace reports the one glTF stores.
    out[0] = uv.x;
    out[1] = 1.0f - uv.y;
}

void mikkSetTangent(const SMikkTSpaceContext* context, const float* tangent, const float sign, const int face,
                    const int vertex) {
    TangentContext& target = contextOf(context);
    const size_t index = static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex);
    target.cornerTangents[index] = Vec4(tangent[0], tangent[1], tangent[2], sign);
}

} // namespace

Vec3 perpendicularTo(const Vec3& normal) {
    // "Building an Orthonormal Basis, Revisited" (Duff et al., 2017): exact for unit
    // vectors and free of the branch on the largest component.
    const float sign = std::copysign(1.0f, normal.z);
    const float a = -1.0f / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    return {1.0f + sign * normal.x * normal.x * a, sign * b, -sign * normal.x};
}

void repairTangentFrames(MeshData& mesh) {
    for (MeshVertex& vertex : mesh.vertices) {
        const float normalLength = glm::length(vertex.normal);
        vertex.normal =
            std::isfinite(normalLength) && normalLength >= kMinimumLength ? vertex.normal / normalLength : kWorldUp;
        const Vec3 tangent = tangentFor(vertex.normal, Vec3(vertex.tangent));
        vertex.tangent = Vec4(tangent, vertex.tangent.w < 0.0f ? -1.0f : 1.0f);
    }
}

void generateFlatNormals(MeshData& mesh) {
    std::vector<MeshVertex> vertices;
    vertices.reserve(mesh.indices.size());

    for (size_t first = 0; first + 2 < mesh.indices.size(); first += 3) {
        MeshVertex a = mesh.vertices[mesh.indices[first]];
        MeshVertex b = mesh.vertices[mesh.indices[first + 1]];
        MeshVertex c = mesh.vertices[mesh.indices[first + 2]];

        // A triangle without area has no plane; any normal is as good as another.
        const Vec3 normal = normalizeOr(glm::cross(b.position - a.position, c.position - a.position), kWorldUp);
        a.normal = normal;
        b.normal = normal;
        c.normal = normal;
        vertices.push_back(a);
        vertices.push_back(b);
        vertices.push_back(c);
    }

    mesh.vertices = std::move(vertices);
    for (size_t index = 0; index < mesh.indices.size(); ++index) {
        mesh.indices[index] = static_cast<uint32_t>(index);
    }
}

void generateTangents(MeshData& mesh) {
    if (mesh.indices.empty()) {
        return;
    }

    TangentContext target;
    target.mesh = &mesh;
    target.cornerTangents.assign(mesh.indices.size(), Vec4(0.0f, 0.0f, 0.0f, 1.0f));

    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = mikkFaceCount;
    callbacks.m_getNumVerticesOfFace = mikkVerticesOfFace;
    callbacks.m_getPosition = mikkPosition;
    callbacks.m_getNormal = mikkNormal;
    callbacks.m_getTexCoord = mikkTexCoord;
    callbacks.m_setTSpaceBasic = mikkSetTangent;

    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &target;
    if (genTangSpaceDefault(&context) == 0) {
        // Only runs out of memory. The zero tangents left behind become arbitrary
        // perpendiculars below, which is still a valid mesh.
        CORDITE_LOG_WARN("Assets", "cannot compute tangents for mesh '{}'; its normal maps will look wrong", mesh.name);
    }

    // One vertex per corner now, each with the tangent of its corner; welding below
    // brings back together the ones that agree.
    std::vector<MeshVertex> vertices;
    vertices.reserve(mesh.indices.size());
    for (size_t index = 0; index < mesh.indices.size(); ++index) {
        MeshVertex vertex = mesh.vertices[mesh.indices[index]];
        const Vec4& tangent = target.cornerTangents[index];
        vertex.tangent = Vec4(tangentFor(vertex.normal, Vec3(tangent)), tangent.w < 0.0f ? -1.0f : 1.0f);
        vertices.push_back(vertex);
        mesh.indices[index] = static_cast<uint32_t>(index);
    }
    mesh.vertices = std::move(vertices);
    weldVertices(mesh.vertices, mesh.indices);
}

void optimizeMesh(MeshData& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return;
    }
    weldVertices(mesh.vertices, mesh.indices);

    for (const Submesh& submesh : mesh.submeshes) {
        const size_t end = static_cast<size_t>(submesh.firstIndex) + submesh.indexCount;
        CORDITE_ASSERT(end <= mesh.indices.size(), "submesh range {}..{} is outside the {} indices of '{}'",
                       submesh.firstIndex, end, mesh.indices.size(), mesh.name);
        if (submesh.indexCount == 0 || end > mesh.indices.size()) {
            continue;
        }
        unsigned int* indices = mesh.indices.data() + submesh.firstIndex;
        meshopt_optimizeVertexCache(indices, indices, submesh.indexCount, mesh.vertices.size());
    }

    // Puts the vertices in the order the triangles use them, which also drops the ones
    // no triangle uses.
    std::vector<MeshVertex> ordered(mesh.vertices.size());
    const size_t usedCount =
        meshopt_optimizeVertexFetch(ordered.data(), mesh.indices.data(), mesh.indices.size(), mesh.vertices.data(),
                                    mesh.vertices.size(), sizeof(MeshVertex));
    ordered.resize(usedCount);
    mesh.vertices = std::move(ordered);

    updateBounds(mesh);
}

} // namespace cordite
