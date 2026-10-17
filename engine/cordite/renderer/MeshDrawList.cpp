#include "cordite/renderer/MeshDrawList.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace cordite {
namespace {

/// What differs between two draws of the same mesh: the rows of the 3x4 matrix that
/// places it in the world, its lighting channels, for a skinned mesh its pose, and where
/// it was before - placed and posed - for its motion. Read by the vertex shader as
/// per-instance attributes.
struct InstanceData {
    Vec4 row0;
    Vec4 row1;
    Vec4 row2;
    /// x: the lighting channels, as a number. y: where the joint matrices of the
    /// instance begin, as a number; zero for an instance that is not skinned. z: the
    /// level of detail it is drawn at, for the debug view of them. w: where the joint
    /// matrices of its pose before begin.
    Vec4 parameters;
    /// The rows of the matrix that placed it before.
    Vec4 previousRow0;
    Vec4 previousRow1;
    Vec4 previousRow2;
};

static_assert(sizeof(InstanceData) == kMeshInstanceStride, "must match the instance attributes of the mesh passes");

/// Room for a small scene before the instance buffer has to grow.
constexpr size_t kInitialInstanceCapacity = 1024;

/// Whether a draw is moved by joints: its mesh can be, and it has been given a pose.
bool isSkinned(const MeshDraw& draw) {
    return draw.mesh->isSkinned() && draw.palette != kNoPalette;
}

uint32_t variantOf(const MeshDraw& draw) {
    uint32_t variant = 0;
    if (isSkinned(draw)) {
        variant |= kMeshVariantSkinned;
    }
    if (draw.material->alphaMode == AlphaMode::Masked) {
        variant |= kMeshVariantMasked;
    } else if (draw.material->alphaMode == AlphaMode::Blended) {
        variant |= kMeshVariantBlended;
    }
    if (draw.material->doubleSided) {
        variant |= kMeshVariantDoubleSided;
    }
    if (glm::determinant(Mat3(draw.localToWorld)) < 0.0f) {
        variant |= kMeshVariantMirrored;
    }
    return variant;
}

/// The level of detail a draw is drawn at: the one it asks for, or the last its mesh has.
uint32_t levelOf(const MeshDraw& draw) {
    return std::min(draw.lod, draw.mesh->levelCount() - 1);
}

bool isDrawable(const MeshDraw& draw) {
    return draw.mesh != nullptr && draw.material != nullptr && draw.submesh < draw.mesh->submeshes.size() &&
           draw.mesh->rangeOf(draw.submesh, draw.lod).indexCount > 0 && draw.mesh->indexBuffer != nullptr;
}

/// A draw with what it is sorted and batched by.
struct SortedDraw {
    const MeshDraw* draw = nullptr;
    uint32_t variant = 0;
    /// Distance along the view direction, for blended surfaces.
    float depth = 0.0f;

    [[nodiscard]] bool isBlended() const { return (variant & kMeshVariantBlended) != 0; }

    /// Draws that agree in this can share a draw call. Serial numbers rather than
    /// addresses, so that two runs of a program draw in the same order.
    [[nodiscard]] auto batchKey() const {
        return std::make_tuple(variant, draw->material->serial, draw->mesh->serial, draw->submesh, levelOf(*draw));
    }
};

} // namespace

MeshDrawList::MeshDrawList(nvrhi::IDevice* device) : m_device(device) {
    CORDITE_ASSERT(device != nullptr, "a mesh draw list needs a device");
}

std::span<const MeshBatch> MeshDrawList::solidBatches() const {
    return std::span(m_batches).first(m_solidBatchCount);
}

std::span<const MeshBatch> MeshDrawList::blendedBatches() const {
    return std::span(m_batches).subspan(m_solidBatchCount);
}

void MeshDrawList::build(nvrhi::ICommandList* commandList, std::span<const MeshDraw> draws, const Vec3& cameraPosition,
                         const Vec3& cameraForward) {
    m_batches.clear();
    m_solidBatchCount = 0;
    CORDITE_ASSERT(commandList != nullptr, "build needs a command list");
    if (commandList == nullptr) {
        return;
    }

    std::vector<SortedDraw> sorted;
    sorted.reserve(draws.size());
    for (const MeshDraw& draw : draws) {
        if (isDrawable(draw)) {
            // A placement that is not a number must not reach the sort: an order that
            // contradicts itself is undefined behaviour there.
            const float depth = glm::dot(Vec3(draw.localToWorld[3]) - cameraPosition, cameraForward);
            sorted.push_back({&draw, variantOf(draw), std::isfinite(depth) ? depth : 0.0f});
        }
    }
    if (sorted.empty()) {
        return;
    }

    // Solid surfaces first, grouped so that equal draws are neighbours and can be
    // instanced; then blended ones from far to near. The sort is stable, so that draws
    // that compare equal keep the order they were given in - every run draws alike.
    std::ranges::stable_sort(sorted, [](const SortedDraw& a, const SortedDraw& b) {
        if (a.isBlended() != b.isBlended()) {
            return !a.isBlended();
        }
        return a.isBlended() ? a.depth > b.depth : a.batchKey() < b.batchKey();
    });

    std::vector<InstanceData> instances;
    instances.reserve(sorted.size());
    // glm stores columns; the shader wants rows.
    const auto rowOf = [](const Mat4& matrix, int row) {
        return Vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
    };
    for (const SortedDraw& entry : sorted) {
        const MeshDraw& draw = *entry.draw;
        const bool skinned = isSkinned(draw);
        const uint32_t previousPalette = draw.previousPalette != kNoPalette ? draw.previousPalette : draw.palette;
        const Mat4& previousToWorld = draw.previousToWorld.value_or(draw.localToWorld);
        instances.push_back(
            {.row0 = rowOf(draw.localToWorld, 0),
             .row1 = rowOf(draw.localToWorld, 1),
             .row2 = rowOf(draw.localToWorld, 2),
             .parameters =
                 Vec4(static_cast<float>(draw.lightChannels & 0xFFu), skinned ? static_cast<float>(draw.palette) : 0.0f,
                      static_cast<float>(levelOf(draw)), skinned ? static_cast<float>(previousPalette) : 0.0f),
             .previousRow0 = rowOf(previousToWorld, 0),
             .previousRow1 = rowOf(previousToWorld, 1),
             .previousRow2 = rowOf(previousToWorld, 2)});
    }

    const size_t capacity =
        m_instanceBuffer ? static_cast<size_t>(m_instanceBuffer->getDesc().byteSize) / sizeof(InstanceData) : 0;
    if (instances.size() > capacity) {
        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize =
            std::max({kInitialInstanceCapacity, instances.size(), capacity + capacity / 2}) * sizeof(InstanceData);
        bufferDesc.debugName = "Mesh instances";
        bufferDesc.isVertexBuffer = true;
        bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        bufferDesc.keepInitialState = true;
        // The previous buffer stays alive until the draws that used it have finished.
        m_instanceBuffer = m_device->createBuffer(bufferDesc);
        if (!m_instanceBuffer) {
            CORDITE_LOG_ERROR("Renderer", "cannot allocate a buffer for {} mesh instances", instances.size());
            return;
        }
    }
    commandList->writeBuffer(m_instanceBuffer, instances.data(), instances.size() * sizeof(InstanceData));

    // Equal draws that follow each other share a batch. Among solid surfaces that is
    // all equal draws; blended ones stay in their order of distance.
    for (size_t first = 0; first < sorted.size();) {
        const SortedDraw& entry = sorted[first];
        size_t end = first + 1;
        while (end < sorted.size() && sorted[end].batchKey() == entry.batchKey()) {
            ++end;
        }
        m_batches.push_back({.mesh = entry.draw->mesh,
                             .submesh = entry.draw->submesh,
                             .lod = levelOf(*entry.draw),
                             .material = entry.draw->material,
                             .variant = entry.variant,
                             .firstInstance = static_cast<uint32_t>(first),
                             .instanceCount = static_cast<uint32_t>(end - first)});
        if (!entry.isBlended()) {
            ++m_solidBatchCount;
        }
        first = end;
    }
}

} // namespace cordite
