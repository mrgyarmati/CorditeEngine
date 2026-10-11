#pragma once

/// @file
/// Draws the line segments collected in a DebugDraw.
///
/// @code
/// CORDITE_TRY_ASSIGN(DebugLinePass linePass, DebugLinePass::create(device, shaders));
///
/// linePass.draw(commandList, target.framebuffer(), lines.vertices(),
///               {.viewProjection = view.viewProjection,
///                .fadeOrigin = view.position,
///                .fadeStart = 30.0f,
///                .fadeEnd = 60.0f});
/// @endcode
///
/// Lines are one pixel wide and alpha-blended. When the framebuffer has a depth buffer
/// they are hidden by nearer geometry unless `depthTest` is turned off; they never write
/// depth themselves.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/DebugDraw.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <span>
#include <vector>

namespace cordite {

struct DebugLineParameters {
    /// World space to clip space.
    Mat4 viewProjection{1.0f};
    /// Test against the framebuffer's depth buffer, if it has one.
    bool depthTest = true;
    /// Brings the lines nearer to the viewer by this fraction of their distance when
    /// they are tested against the depth buffer: 0.001 is a centimetre at ten metres.
    /// Lines drawn onto a surface (a grid on a floor) need a little of it, or the
    /// surface hides them in places.
    float depthBias = 0.0f;
    /// Lines fade out between `fadeStart` and `fadeEnd` metres from `fadeOrigin`. The
    /// defaults are far beyond any scene, so nothing fades.
    Vec3 fadeOrigin{0.0f};
    float fadeStart = 1.0e30f;
    float fadeEnd = 2.0e30f;
};

class DebugLinePass {
public:
    /// Loads the shaders. Fails when they cannot be compiled.
    [[nodiscard]] static Result<DebugLinePass> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Draws `vertices`, two per line, over the framebuffer. The command list must be
    /// open. May be called several times per frame with different lines and parameters.
    void draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, std::span<const DebugVertex> vertices,
              const DebugLineParameters& parameters);

private:
    DebugLinePass() = default;

    struct Pipeline {
        nvrhi::FramebufferInfo framebufferInfo;
        bool depthTest = false;
        nvrhi::GraphicsPipelineHandle handle;
    };

    [[nodiscard]] nvrhi::IGraphicsPipeline* pipelineFor(const nvrhi::FramebufferInfo& framebufferInfo, bool depthTest);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::ShaderHandle m_vertexShader;
    nvrhi::ShaderHandle m_pixelShader;
    nvrhi::InputLayoutHandle m_inputLayout;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::BufferHandle m_vertexBuffer;
    std::vector<Pipeline> m_pipelines;
};

} // namespace cordite
