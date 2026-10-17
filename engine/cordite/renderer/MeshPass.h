#pragma once

/// @file
/// A pass that draws meshes.
///
/// The depth pre-pass, the base pass that fills the G-buffer, the forward pass and the
/// debug views that draw geometry all draw the same batches of meshes with the same
/// vertex stage. What they share is this class; what sets them apart is a MeshPassDesc.
///
/// @code
/// MeshPassDesc desc;
/// desc.debugName = "Base pass";
/// desc.pixelShader = {.path = "passes/MeshGBuffer.slang", .entryPoint = "meshGBufferPixel",
///                     .stage = ShaderStage::Pixel};
/// desc.depth = MeshPassDepth::Equal;
/// CORDITE_TRY_ASSIGN(MeshPass basePass,
///                    MeshPass::create(device, shaders,
///                                     {.view = view.layout(),
///                                      .material = resources.materialLayout(),
///                                      .joints = palettes.layout()},
///                                     desc));
///
/// basePass.draw(commandList, targets.gbufferFramebuffer(), drawList.solidBatches(),
///               {.view = view.bindings(), .instances = drawList.instanceBuffer(), .joints = palettes.bindings()});
/// @endcode
///
/// Conventions: front faces wind counter-clockwise, depth is reversed (nearer is
/// greater), binding set 0 is the view's and set 1 the material's.
///
/// A batch of skinned meshes (kMeshVariantSkinned) is drawn by pipelines of its own: the
/// one vertex shader compiled with SKINNED, which reads how each vertex follows its
/// joints from a third vertex buffer (GpuMesh::skinBuffer) and the joint matrices from a
/// third binding set (SkinPalettes). Everything else about the pass is the same.
///
/// A pass that is to know where its vertices were when the view was last drawn
/// (MeshPassDesc::motion) compiles all of its shaders with MOTION, and reads where each
/// instance was from three more rows of its entry in the instance buffer.

#include "cordite/core/Result.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace cordite {

/// What a mesh pass does with the depth buffer.
enum class MeshPassDepth : uint8_t {
    /// Draws what is not farther than what is there, and writes its depth.
    TestAndWrite,
    /// Draws what is not hidden by what is there, and leaves the depth as it is.
    TestOnly,
    /// Draws only where the depth is exactly the one already there: the very surfaces
    /// a depth pre-pass drew.
    Equal,
    /// Ignores the depth buffer.
    None,
};

/// What a mesh pass does with the color already in the target.
enum class MeshPassBlend : uint8_t {
    /// Replaces it.
    None,
    /// Blends over it by the alpha the pixel shader returns.
    Alpha,
    /// Adds to it.
    Additive,
};

struct MeshPassDesc {
    /// Shown in error messages.
    std::string debugName = "Mesh pass";
    /// The pixel shader. Leave the path empty for a pass that writes depth only.
    ShaderRequest pixelShader;
    /// The pixel shader for masked materials, where they need one of their own: a
    /// depth-only pass still has to cut their holes. Empty uses `pixelShader`.
    ShaderRequest maskedPixelShader;
    MeshPassDepth depth = MeshPassDepth::TestAndWrite;
    MeshPassBlend blend = MeshPassBlend::None;
    /// Draw the edges of the triangles instead of filling them, front and back. Needs
    /// an adapter that can (AdapterInfo::supportsWireframe).
    bool wireframe = false;
    /// Depth as a shadow map holds it: 0 at the nearest and 1 at the farthest, where
    /// the views of a scene have it the other way round. The depth test passes what
    /// is nearer either way.
    bool conventionalDepth = false;
    /// Also work out where each vertex was when the view was last drawn - the vertex
    /// shader compiled with MOTION, and the pixel shaders too, which then get it
    /// (MotionVectors.h). For the base pass, which writes motion vectors, and for the
    /// depth pre-pass, whose positions the base pass has to match to the bit: the two
    /// must run the same code.
    bool motion = false;
};

/// What a call to MeshPass::draw() cost.
struct MeshPassStatistics {
    uint32_t drawCalls = 0;
    /// Submeshes drawn; more than `drawCalls` where instancing merged some.
    uint32_t instances = 0;
    uint64_t triangles = 0;

    MeshPassStatistics& operator+=(const MeshPassStatistics& other) {
        drawCalls += other.drawCalls;
        instances += other.instances;
        triangles += other.triangles;
        return *this;
    }

    bool operator==(const MeshPassStatistics&) const = default;
};

/// The layouts of the binding sets a mesh pass draws with.
struct MeshPassLayouts {
    /// Set 0: ViewBindings::layout(), or the layout of a view the pass has of its own.
    nvrhi::IBindingLayout* view = nullptr;
    /// Set 1: RenderResources::materialLayout().
    nvrhi::IBindingLayout* material = nullptr;
    /// Set 2, of the pipelines that skin: SkinPalettes::layout().
    nvrhi::IBindingLayout* joints = nullptr;
};

/// What the batches of one draw are drawn with.
struct MeshPassBindings {
    /// Set 0: the view.
    nvrhi::IBindingSet* view = nullptr;
    /// MeshDrawList::instanceBuffer() of the list the batches come from.
    nvrhi::IBuffer* instances = nullptr;
    /// Set 2: SkinPalettes::bindings(), the joint matrices the palettes of the
    /// instances count into. May be null where no batch is skinned.
    nvrhi::IBindingSet* joints = nullptr;
};

class MeshPass {
public:
    /// Loads the shaders. The device and the three layouts must outlive the pass.
    [[nodiscard]] static Result<MeshPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                 const MeshPassLayouts& layouts, MeshPassDesc desc);

    /// Draws `batches` into the framebuffer, in the order given. The command list must
    /// be open. The framebuffer must have a depth buffer unless the pass ignores depth.
    /// A skinned batch for which there are no joint matrices is left out.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                            std::span<const MeshBatch> batches, const MeshPassBindings& bindings);

    /// The same, into the part of the framebuffer that `viewport` names, in pixels;
    /// nothing is drawn outside it.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                            std::span<const MeshBatch> batches, const MeshPassBindings& bindings,
                            const nvrhi::Viewport& viewport);

private:
    MeshPass() = default;

    struct Pipeline {
        nvrhi::FramebufferInfo framebufferInfo;
        uint32_t variant = 0;
        nvrhi::GraphicsPipelineHandle handle;
    };

    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo,
                                                        uint32_t variant);

    nvrhi::IDevice* m_device = nullptr;
    MeshPassDesc m_desc;
    nvrhi::ShaderHandle m_vertexShader;
    /// The same shader, compiled for meshes that joints move.
    nvrhi::ShaderHandle m_skinnedVertexShader;
    /// Null for a pass that writes depth only.
    nvrhi::ShaderHandle m_pixelShader;
    /// Null when masked materials use `m_pixelShader`.
    nvrhi::ShaderHandle m_maskedPixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::InputLayoutHandle m_skinnedInputLayout;
    nvrhi::BindingLayoutHandle m_viewLayout;
    nvrhi::BindingLayoutHandle m_materialLayout;
    nvrhi::BindingLayoutHandle m_jointLayout;
    std::vector<Pipeline> m_pipelines;
};

} // namespace cordite
