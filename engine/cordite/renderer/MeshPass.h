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
///                    MeshPass::create(device, shaders, view.layout(), resources.materialLayout(), desc));
///
/// basePass.draw(commandList, targets.gbufferFramebuffer(), drawList.solidBatches(), view.bindings(),
///               drawList.instanceBuffer());
/// @endcode
///
/// Conventions: front faces wind counter-clockwise, depth is reversed (nearer is
/// greater), binding set 0 is the view's and set 1 the material's.

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

class MeshPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout() and `materialLayout`
    /// RenderResources::materialLayout(); the device and both layouts must outlive the
    /// pass.
    [[nodiscard]] static Result<MeshPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                 nvrhi::IBindingLayout* viewLayout,
                                                 nvrhi::IBindingLayout* materialLayout, MeshPassDesc desc);

    /// Draws `batches` into the framebuffer, in the order given. The command list must
    /// be open; `instanceBuffer` is the one of the MeshDrawList the batches come from.
    /// The framebuffer must have a depth buffer unless the pass ignores depth.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                            std::span<const MeshBatch> batches, nvrhi::IBindingSet* viewBindings,
                            nvrhi::IBuffer* instanceBuffer);

    /// The same, into the part of the framebuffer that `viewport` names, in pixels;
    /// nothing is drawn outside it.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                            std::span<const MeshBatch> batches, nvrhi::IBindingSet* viewBindings,
                            nvrhi::IBuffer* instanceBuffer, const nvrhi::Viewport& viewport);

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
    /// Null for a pass that writes depth only.
    nvrhi::ShaderHandle m_pixelShader;
    /// Null when masked materials use `m_pixelShader`.
    nvrhi::ShaderHandle m_maskedPixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_viewLayout;
    nvrhi::BindingLayoutHandle m_materialLayout;
    std::vector<Pipeline> m_pipelines;
};

} // namespace cordite
