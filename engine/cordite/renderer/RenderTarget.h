#pragma once

/// @file
/// An offscreen surface to render into: a color texture, optionally a depth buffer, and
/// the framebuffer that ties them together.
///
/// @code
/// CORDITE_TRY_ASSIGN(RenderTarget target,
///                    RenderTarget::create(device, {.size = {1280, 720}, .withDepth = true, .debugName = "Viewport"}));
///
/// target.clear(commandList);
/// pass.draw(commandList, target.framebuffer(), ...);
/// showOnScreen(target.color());          // the color texture can be sampled afterwards
///
/// CORDITE_TRY(target.resize(device, newSize));   // replaces the textures when the size changed
/// @endcode
///
/// Depth uses the engine's reversed-Z convention: the far plane is 0, and that is what
/// the depth buffer is cleared to.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"

#include <nvrhi/nvrhi.h>

#include <string>

namespace cordite {

struct RenderTargetDesc {
    /// In pixels. Both dimensions must be at least 1.
    UVec2 size{1, 1};
    nvrhi::Format colorFormat = nvrhi::Format::RGBA8_UNORM;
    /// Adds a 32-bit floating-point depth buffer.
    bool withDepth = false;
    /// What clear() fills the color texture with.
    Vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    /// Shown in graphics debuggers.
    std::string debugName = "Render target";
};

class RenderTarget {
public:
    /// An empty target: isValid() is false until it is assigned one made by create().
    RenderTarget() = default;

    [[nodiscard]] static Result<RenderTarget> create(nvrhi::IDevice* device, const RenderTargetDesc& desc);

    [[nodiscard]] bool isValid() const { return m_framebuffer != nullptr; }

    [[nodiscard]] UVec2 size() const { return m_desc.size; }

    [[nodiscard]] const RenderTargetDesc& desc() const { return m_desc; }

    [[nodiscard]] nvrhi::ITexture* color() const { return m_color; }

    /// Null when the target has no depth buffer.
    [[nodiscard]] nvrhi::ITexture* depth() const { return m_depth; }

    [[nodiscard]] nvrhi::IFramebuffer* framebuffer() const { return m_framebuffer; }

    /// Fills the color texture with the clear color and the depth buffer with "far". The
    /// command list must be open.
    void clear(nvrhi::ICommandList* commandList) const;

    /// Changes what clear() fills the color texture with.
    void setClearColor(const Vec4& color) { m_desc.clearColor = color; }

    /// Replaces the textures with ones of a new size, keeping everything else. Does
    /// nothing when the size is unchanged. The old textures stay alive until the GPU has
    /// finished with them; their content is not carried over.
    [[nodiscard]] Status resize(nvrhi::IDevice* device, UVec2 size);

private:
    RenderTargetDesc m_desc;
    nvrhi::TextureHandle m_color;
    nvrhi::TextureHandle m_depth;
    nvrhi::FramebufferHandle m_framebuffer;
};

} // namespace cordite
