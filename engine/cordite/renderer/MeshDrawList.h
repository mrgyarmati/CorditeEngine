#pragma once

/// @file
/// The meshes of a view, put in the order they are drawn in.
///
/// Several passes draw the same meshes: the depth pre-pass, the base pass, the forward
/// pass. A MeshDrawList does once per view what all of them need: it sorts the draws,
/// groups equal ones into batches that become single instanced draw calls, and sends
/// the placements of all instances to the GPU.
///
/// @code
/// MeshDrawList drawList(device);
///
/// drawList.build(commandList, draws, view.position, view.forward);
/// const MeshPassBindings bindings{
///     .view = viewBindings, .instances = drawList.instanceBuffer(), .joints = palettes.bindings()};
/// depthPass.draw(commandList, targets.depthFramebuffer(), drawList.solidBatches(), bindings);
/// forwardPass.draw(commandList, targets.sceneFramebuffer(), drawList.blendedBatches(), bindings);
/// @endcode
///
/// Solid surfaces - opaque and masked - come grouped so that equal draws are neighbours.
/// Blended ones come from far to near, which is the order they have to be drawn in, and
/// are only grouped where that order allows it.
///
/// Skinned meshes are instanced like any other: the instances of a batch share the mesh
/// and differ in where their joint matrices are. A skinned mesh without a pose is not
/// in a batch with ones that have a pose; it is drawn as the rigid mesh it then is.

#include "cordite/math/Math.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SkinPalettes.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cordite {

/// One submesh of a mesh, drawn once.
struct MeshDraw {
    const GpuMesh* mesh = nullptr;
    /// Index into GpuMesh::submeshes.
    uint32_t submesh = 0;
    const GpuMaterial* material = nullptr;
    /// Places the mesh in the world.
    Mat4 localToWorld{1.0f};
    /// The lighting channels the surface belongs to, one bit each.
    uint32_t lightChannels = 1;
    /// For a skinned mesh in a pose: where the joint matrices of the pose begin among
    /// those of the view, as SkinPalettes::add() said. kNoPalette draws the mesh as it
    /// was modelled, as does any value for a mesh that is not skinned.
    uint32_t palette = kNoPalette;
    /// The level of detail to draw (GpuMesh::rangeOf()): 0 for the mesh itself.
    uint32_t lod = 0;
    /// Where the mesh was placed when the view was last drawn, for its motion vectors
    /// (MotionVectors.h). None takes `localToWorld` for it: what has no past did not move.
    std::optional<Mat4> previousToWorld;
    /// Where the joint matrices of the pose it was in then begin, as `palette` says where
    /// those of its pose now do: kNoPalette takes `palette` for it - a pose that has not
    /// changed, or has no past.
    uint32_t previousPalette = kNoPalette;
};

// What a batch needs of the pipeline that draws it: the bits of MeshBatch::variant.
/// The material cuts holes where its opacity is below its cutoff.
inline constexpr uint32_t kMeshVariantMasked = 1u << 0;
/// The material lets what is behind it show through.
inline constexpr uint32_t kMeshVariantBlended = 1u << 1;
/// Both sides of every triangle are drawn.
inline constexpr uint32_t kMeshVariantDoubleSided = 1u << 2;
/// The instances are mirrored, which turns their triangles the other way round.
inline constexpr uint32_t kMeshVariantMirrored = 1u << 3;
/// The vertices are moved by joints before the instance is placed: the mesh is skinned,
/// and every instance of the batch has a pose.
inline constexpr uint32_t kMeshVariantSkinned = 1u << 4;

/// Draws that share a mesh, a submesh, a material and a variant: one instanced draw call.
struct MeshBatch {
    const GpuMesh* mesh = nullptr;
    uint32_t submesh = 0;
    /// The level of detail its instances are drawn at.
    uint32_t lod = 0;
    const GpuMaterial* material = nullptr;
    /// A combination of the kMeshVariant bits.
    uint32_t variant = 0;
    /// The range of the batch in the instance buffer.
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
};

class MeshDrawList {
public:
    /// The device must outlive the list.
    explicit MeshDrawList(nvrhi::IDevice* device);

    /// Replaces the content of the list with `draws` as a camera at `cameraPosition`
    /// looking along `cameraForward` sees them, and records sending their placements to
    /// the GPU. The command list must be open. Entries without a mesh or a material and
    /// submeshes without triangles are left out. The order of `draws` does not matter,
    /// except between blended surfaces at the same distance, which keep the order given.
    void build(nvrhi::ICommandList* commandList, std::span<const MeshDraw> draws, const Vec3& cameraPosition,
               const Vec3& cameraForward);

    /// Opaque and masked surfaces, grouped.
    [[nodiscard]] std::span<const MeshBatch> solidBatches() const;

    /// Blended surfaces, the farthest first.
    [[nodiscard]] std::span<const MeshBatch> blendedBatches() const;

    /// Every batch: the solid ones, then the blended ones.
    [[nodiscard]] std::span<const MeshBatch> batches() const { return m_batches; }

    /// One entry per instance, kMeshInstanceStride bytes each: the three rows of the
    /// 3x4 matrix that places it, as three float4; a fourth float4 with what else
    /// differs between instances - its lighting channels in x, in y where its joint
    /// matrices begin (MeshDraw::palette; zero without), in z its level of detail, and in
    /// w where the joint matrices of its pose before begin (MeshDraw::previousPalette);
    /// and the three rows of the matrix that placed it before (MeshDraw::previousToWorld).
    /// Null while the list is empty.
    [[nodiscard]] nvrhi::IBuffer* instanceBuffer() const { return m_instanceBuffer; }

private:
    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_instanceBuffer;
    std::vector<MeshBatch> m_batches;
    /// How many of the batches are solid; they come first.
    size_t m_solidBatchCount = 0;
};

/// The stride of MeshDrawList::instanceBuffer() in bytes.
inline constexpr uint32_t kMeshInstanceStride = 112;

} // namespace cordite
