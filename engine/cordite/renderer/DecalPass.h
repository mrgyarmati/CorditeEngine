#pragma once

/// @file
/// Decals: the pass that lays them onto what the G-buffer holds, after the base pass and
/// before anything reads it - the ambient occlusion, the lighting, the debug views.
///
/// @code
/// CORDITE_TRY_ASSIGN(DecalPass decals, DecalPass::create(device, shaders, view.layout(), resources.materialLayout()));
///
/// SceneDecals found;
/// found.collect(world);
/// // After the base pass has filled targets.gbuffer(...):
/// const uint32_t drawn = decals.draw(commandList, targets, view.bindings(), cameraView, resources, found.decals());
/// @endcode
///
/// Each decal is its box, drawn from inside, with its own material (shaders/passes/Decal.slang,
/// with the arithmetic of Decals.h): a pixel the box covers lays the material onto the
/// surface the G-buffer holds there, where that is inside the box and faces back along the
/// projection - its color, roughness, metal and specular blended in by how much of it there
/// is, its normal map bending the surface's normal, its light added. How much there is goes
/// by the material's alpha mode, as for a mesh: all of an opaque one, all or nothing of a
/// masked one, and of a blended one what its alpha says. Its occlusion texture is not used.
///
/// The normals of the G-buffer are numbers that cannot be blended, so a decal bends them in
/// its shader, reading a copy of them (SceneTargets::prepareDecalNormals): made before the
/// first decal, and again before one whose box shares some of the world with that of a decal
/// since then that bent them - so each decal bends the normals the decals before it left.
/// A decal whose material has no normal map writes no normals at all.
///
/// What a decal lands on is everything solid in its box, moving or not; translucent
/// surfaces are drawn after, forward, and get none.

#include "cordite/core/Result.h"
#include "cordite/math/Geometry.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/Decals.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace cordite {

/// What DecalPass::draw() did.
struct DecalDrawStatistics {
    /// The decals drawn.
    uint32_t decals = 0;
    /// How often the normals of the G-buffer were copied for the decals to read: before
    /// the first, and again before each that comes to a part of the world a decal since
    /// the copy before bent them in.
    uint32_t normalCopies = 0;
};

class DecalPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout(), `materialLayout`
    /// RenderResources::materialLayout().
    [[nodiscard]] static Result<DecalPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                  nvrhi::IBindingLayout* viewLayout,
                                                  nvrhi::IBindingLayout* materialLayout);

    /// Asks `resources` for what drawing `decals` takes - their materials, and the cube
    /// their boxes are drawn with - so that it reaches the GPU with everything else the
    /// frame uploads (RenderResources::flushUploads()), ahead of what draw() records.
    static void prepare(RenderResources& resources, std::span<const SceneDecal> decals);

    /// Lays `decals` - the first kMaxDecals, in the order given - onto the G-buffer of
    /// `targets`, which were drawn for `view` with `viewBindings`; the materials are those
    /// of `resources`. A decal whose box is out of view is left out, and so is one whose
    /// material cannot be loaded, which `resources` reports. The command list must be open.
    ///
    /// Returns what was drawn: nothing, with the G-buffer left as it was, when there is
    /// nothing to draw or what the pass needs could not be made, which the log then says
    /// once.
    DecalDrawStatistics draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                             const CameraView& view, RenderResources& resources, std::span<const SceneDecal> decals);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::ShaderHandle vertexShader;
        nvrhi::ShaderHandle pixelShader;
        nvrhi::InputLayoutHandle inputLayout;
        nvrhi::BindingLayoutHandle viewLayout;
        nvrhi::BindingLayoutHandle materialLayout;
        nvrhi::BindingLayoutHandle layout;
        nvrhi::BufferHandle instances;
    };

    /// A decal that is drawn, with its material.
    struct DrawnDecal {
        const SceneDecal* decal;
        const GpuMaterial* material;
    };

    /// A pipeline for one framebuffer layout: one that writes normals and one that leaves
    /// them alone.
    struct Pipeline {
        nvrhi::FramebufferInfo framebuffer;
        bool writesNormals;
        nvrhi::GraphicsPipelineHandle pipeline;
    };

    explicit DecalPass(Parts parts);

    /// The pipeline for `framebuffer`, made the first time it is asked for; null when it
    /// cannot be made.
    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebuffer, bool writesNormals);

    nvrhi::IDevice* m_device;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_pixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_viewLayout;
    nvrhi::BindingLayoutHandle m_materialLayout;
    /// The normals as they were, and the depth: the decal's own set.
    nvrhi::BindingLayoutHandle m_layout;
    /// What differs per decal, kMaxDecals of them: written once for every view.
    nvrhi::BufferHandle m_instances;
    std::vector<Pipeline> m_pipelines;
    /// Working memory of draw(): what is drawn, and the boxes of the decals that bent
    /// normals since they were last copied.
    std::vector<DrawnDecal> m_drawn;
    std::vector<Aabb> m_bent;
    /// So that a failure to make what the pass needs is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
