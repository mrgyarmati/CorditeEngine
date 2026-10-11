#pragma once

/// @file
/// Draws meshes fully shaded in one pass.
///
/// It handles every kind of material: opaque and masked surfaces are drawn first, with
/// depth written; blended ones afterwards, farthest first, over what is already there.
/// Draws that share a mesh, a submesh and a material become one instanced draw call.
///
/// @code
/// CORDITE_TRY_ASSIGN(MeshForwardPass pass, MeshForwardPass::create(device, shaders, resources.materialLayout()));
///
/// std::vector<MeshDraw> draws = collectVisible(world, resources);
/// resources.flushUploads();
/// const MeshPassStatistics drawn = pass.draw(commandList, targets.sceneFramebuffer(), draws, view);
/// @endcode
///
/// Conventions: front faces wind counter-clockwise, depth is reversed (nearer is
/// greater), and the output is linear light.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
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
};

/// The point of view and the light for a call to MeshForwardPass::draw().
struct MeshPassView {
    /// World space to clip space.
    Mat4 viewProjection{1.0f};
    Vec3 cameraPosition{0.0f};
    /// The direction the camera looks in; blended surfaces are sorted along it.
    Vec3 cameraForward = kWorldForward;
    /// The camera has no perspective: every surface is seen from the direction
    /// `cameraForward`, not from `cameraPosition`. Highlights depend on it.
    bool orthographic = false;

    /// The fixed lighting that stands in until the engine has lights: a key light, given
    /// by the unit vector from a surface towards it and by its color and strength, and
    /// ambient light from a sky above and a ground below.
    Vec3 keyLightDirection{0.0f, 1.0f, 0.0f};
    Vec3 keyLightColor{1.0f};
    Vec3 skyColor{0.0f};
    Vec3 groundColor{0.0f};
};

/// What one call to MeshForwardPass::draw() cost.
struct MeshPassStatistics {
    uint32_t drawCalls = 0;
    /// Submeshes drawn; more than `drawCalls` where instancing merged some.
    uint32_t instances = 0;
    uint64_t triangles = 0;
};

class MeshForwardPass {
public:
    /// Loads the shaders. `materialLayout` is RenderResources::materialLayout().
    [[nodiscard]] static Result<MeshForwardPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                        nvrhi::IBindingLayout* materialLayout);

    /// Draws `draws` into the framebuffer, which must have a depth buffer. The command
    /// list must be open. The order of `draws` does not matter, except between blended
    /// surfaces at the same distance, which are drawn in the order given. Entries
    /// without a mesh or a material, and submeshes without triangles, are skipped.
    MeshPassStatistics draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
                            std::span<const MeshDraw> draws, const MeshPassView& view);

private:
    MeshForwardPass() = default;

    struct Pipeline {
        nvrhi::FramebufferInfo framebufferInfo;
        uint32_t variant = 0;
        nvrhi::GraphicsPipelineHandle handle;
    };

    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo,
                                                        uint32_t variant);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_pixelShader;
    nvrhi::ShaderHandle m_alphaTestPixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_viewLayout;
    nvrhi::BindingLayoutHandle m_materialLayout;
    nvrhi::BindingSetHandle m_viewBindings;
    nvrhi::BufferHandle m_viewConstants;
    nvrhi::BufferHandle m_instanceBuffer;
    std::vector<Pipeline> m_pipelines;
};

} // namespace cordite
