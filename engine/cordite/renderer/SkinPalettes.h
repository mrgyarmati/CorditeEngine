#pragma once

/// @file
/// The joint matrices the skinned meshes of a view are drawn with, on the GPU.
///
/// Every pose that something is drawn with adds its matrices - a palette - and gets back
/// where they begin in the one buffer that holds them all. That number is what an
/// instance of a skinned mesh carries to the vertex shader (MeshDraw::palette), so any
/// number of figures, each in a pose of its own, are still drawn as instances of one
/// mesh.
///
/// @code
/// CORDITE_TRY_ASSIGN(SkinPalettes palettes, SkinPalettes::create(device));
///
/// palettes.clear();                                       // for every view
/// draw.palette = palettes.add(pose.skinning);             // for every pose something is drawn with
/// palettes.upload(commandList);                           // before the draws are recorded
/// pass.draw(commandList, framebuffer, batches,
///           {.view = viewBindings, .instances = instanceBuffer, .joints = palettes.bindings()});
/// @endcode
///
/// The buffer is a binding set of its own, register space 2 of the vertex stage, and
/// only pipelines that skin have it (see MeshPass).

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// The value of MeshDraw::palette for a draw that is not skinned, and what
/// SkinPalettes::add() returns when it has no room.
inline constexpr uint32_t kNoPalette = 0xFFFFFFFFu;

/// How many joint matrices a view can be drawn with, all poses together. The number an
/// instance carries is a float, which counts this far without skipping.
inline constexpr uint32_t kMaxPaletteJoints = 1u << 20;

class SkinPalettes {
public:
    /// The device must outlive the object.
    [[nodiscard]] static Result<SkinPalettes> create(nvrhi::IDevice* device);

    /// Forgets the palettes that were added. What is on the GPU stays until the next
    /// upload().
    void clear();

    /// Adds the matrices of one pose - SkeletonPoseComponent::skinning - and returns
    /// where the first of them is among all that were added. A matrix that is not made
    /// of numbers is replaced by one that leaves a vertex where it is. Returns
    /// kNoPalette, and adds nothing, for no matrices and when kMaxPaletteJoints would be
    /// exceeded; the latter is reported in the log, once.
    [[nodiscard]] uint32_t add(std::span<const Mat4> skinning);

    /// How many matrices have been added since the last clear().
    [[nodiscard]] uint32_t jointCount() const { return static_cast<uint32_t>(m_joints.size()); }

    /// Sends what was added to the GPU. The command list must be open. If the buffer
    /// cannot grow to hold it, nothing is sent and false is returned: nothing must then
    /// be drawn with the palettes of this view.
    [[nodiscard]] bool upload(nvrhi::ICommandList* commandList);

    /// The layout of bindings(): what a pipeline that skins has as its third set.
    [[nodiscard]] nvrhi::IBindingLayout* layout() const { return m_layout; }

    /// The matrices, as the vertex shader reads them. Never null after create().
    [[nodiscard]] nvrhi::IBindingSet* bindings() const { return m_bindings; }

    /// The bytes the buffer takes on the GPU.
    [[nodiscard]] uint64_t bufferBytes() const;

private:
    /// A joint matrix as the shader reads it: the three rows of a 3x4 matrix.
    struct GpuJoint {
        Vec4 row0;
        Vec4 row1;
        Vec4 row2;
    };

    SkinPalettes() = default;

    /// Makes the buffer hold at least `jointCount` matrices.
    [[nodiscard]] bool reserve(size_t jointCount);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::BindingLayoutHandle m_layout;
    nvrhi::BufferHandle m_buffer;
    nvrhi::BindingSetHandle m_bindings;
    std::vector<GpuJoint> m_joints;
    bool m_reportedFull = false;
};

} // namespace cordite
