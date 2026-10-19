#pragma once

/// @file
/// Presentation of rendered frames to a window.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto swapchain, Swapchain::create(*graphics, *window, {.vsync = true}));
/// while (running) {
///     CORDITE_TRY_ASSIGN(const SwapchainFrame frame, swapchain->beginFrame());
///     if (frame.framebuffer != nullptr) {      // null while the window is minimized
///         renderInto(frame.framebuffer);
///         CORDITE_TRY(swapchain->present());
///     }
///     graphics->collectGarbage();
/// }
/// @endcode
///
/// The swapchain follows the window: when the framebuffer size changes it is recreated
/// on the next beginFrame(), so handles obtained from an earlier frame must not be kept.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/platform/Window.h"
#include "cordite/rhi/GraphicsDevice.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

namespace cordite {

struct SwapchainDesc {
    /// Synchronize presentation with the display refresh. Without it, frames are shown
    /// as soon as they are ready, which can tear.
    bool vsync = true;
};

/// The back buffer to render the current frame into.
struct SwapchainFrame {
    /// Null when there is nothing to render to (the window is minimized). Skip
    /// rendering and present() for that frame.
    nvrhi::ITexture* texture = nullptr;
    /// A framebuffer with `texture` as its only color attachment.
    nvrhi::IFramebuffer* framebuffer = nullptr;
    UVec2 size{0};
};

class Swapchain {
public:
    /// The device must have been created with the window system's presentation
    /// extensions. The window and the device must outlive the swapchain. Fails with
    /// Unsupported for a device of Direct3D 12, which draws offscreen only for now, and with
    /// InvalidState for a device made without presentation.
    [[nodiscard]] static Result<std::unique_ptr<Swapchain>> create(GraphicsDevice& device, const Window& window,
                                                                   const SwapchainDesc& desc = {});
    ~Swapchain();
    CORDITE_NON_MOVABLE(Swapchain);

    /// Acquires the next back buffer, first recreating the swapchain if the window was
    /// resized. Every successful call that returns a texture must be followed by present().
    [[nodiscard]] Result<SwapchainFrame> beginFrame();

    /// Shows the frame acquired by beginFrame(). Blocks briefly when the CPU is more
    /// than two frames ahead of the GPU.
    [[nodiscard]] Status present();

    /// Current size in pixels.
    [[nodiscard]] UVec2 size() const;

    /// Format of the back buffers. Always an 8-bit UNORM format: the final pass writes
    /// display-encoded (sRGB) values itself.
    [[nodiscard]] nvrhi::Format format() const;

    [[nodiscard]] bool vsync() const;
    /// Takes effect at the next beginFrame().
    void setVsync(bool enabled);

    /// Number of times the swapchain was recreated since creation, for tests and statistics.
    [[nodiscard]] uint32_t recreationCount() const;

private:
    struct Impl;

    explicit Swapchain(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
