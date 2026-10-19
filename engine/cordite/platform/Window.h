#pragma once

/// @file
/// Operating system windows.
///
/// A WindowSystem owns the connection to the desktop; Windows are created from it and
/// must be destroyed before it. Everything here must be used from the main thread.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto windowSystem, WindowSystem::create());
/// CORDITE_TRY_ASSIGN(auto window, windowSystem->createWindow({.title = "My Game"}));
/// while (!window->shouldClose()) {
///     windowSystem->pollEvents();
///     if (window->input().wasKeyPressed(Key::Escape)) {
///         window->requestClose();
///     }
///     renderFrame(window->framebufferSize());
/// }
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/platform/Input.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

enum class CursorMode : uint8_t {
    /// Visible and free to leave the window.
    Normal,
    /// Invisible while over the window.
    Hidden,
    /// Invisible and locked to the window, reporting unbounded movement. For camera control.
    Captured,
};

struct WindowDesc {
    std::string title = "Cordite";
    /// Client area size in screen coordinates.
    uint32_t width = 1280;
    uint32_t height = 720;
    bool resizable = true;
    /// Hidden windows still render; tests and offscreen tools use them.
    bool visible = true;
    bool maximized = false;
    /// Borderless window covering the primary monitor.
    bool fullscreen = false;
};

/// The window of the system that a Window is: what a dialog of the system - one that asks
/// for a file - is shown over, and holds back while it is open. Of no kind on Linux: there
/// it would take the headers of X11, whose names (`Status`, `None`) clash with the engine's,
/// and a dialog is shown without it, as a window of its own.
struct SystemWindow {
    enum class Kind : uint8_t {
        None,
        /// The handle is an HWND.
        Windows,
        /// The handle is an NSWindow*.
        Cocoa,
    };

    Kind kind = Kind::None;
    void* handle = nullptr;
};

class Window {
public:
    ~Window();
    CORDITE_NON_MOVABLE(Window);

    /// True once the user or requestClose() asked the window to close.
    [[nodiscard]] bool shouldClose() const;
    void requestClose();

    /// Size of the drawable area in pixels. Zero while the window is minimized.
    [[nodiscard]] UVec2 framebufferSize() const;

    /// Size of the client area in screen coordinates, which differ from pixels on
    /// high-DPI displays.
    [[nodiscard]] UVec2 logicalSize() const;
    void setLogicalSize(UVec2 size);

    /// Ratio of pixels to screen coordinates (1.0 on a standard-DPI display).
    [[nodiscard]] float contentScale() const;

    [[nodiscard]] bool isMinimized() const;
    [[nodiscard]] bool isFocused() const;

    void setTitle(std::string_view title);
    void setVisible(bool visible);

    [[nodiscard]] CursorMode cursorMode() const;
    void setCursorMode(CursorMode mode);

    [[nodiscard]] Input& input();
    [[nodiscard]] const Input& input() const;

    /// Creates a Vulkan surface for this window. `vulkanInstance` is a VkInstance created
    /// with WindowSystem::vulkanInstanceExtensions(); the result is a VkSurfaceKHR that
    /// the caller owns. The types are erased to keep Vulkan out of this header.
    [[nodiscard]] Result<uint64_t> createVulkanSurface(void* vulkanInstance) const;

    /// The underlying GLFWwindow*, for libraries that integrate with GLFW directly.
    [[nodiscard]] void* nativeHandle() const;

    /// The window of the system this is, for what the system shows over it.
    [[nodiscard]] SystemWindow systemWindow() const;

private:
    friend class WindowSystem;
    struct Impl;

    explicit Window(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

struct WindowSystemDesc {
    /// Run without a desktop connection. Windows can be created and receive injected
    /// input, but have no Vulkan surface. For servers, CI and tools.
    bool headless = false;
};

class WindowSystem {
public:
    /// Connects to the desktop. Only one WindowSystem may exist at a time.
    [[nodiscard]] static Result<std::unique_ptr<WindowSystem>> create(const WindowSystemDesc& desc = {});
    ~WindowSystem();
    CORDITE_NON_MOVABLE(WindowSystem);

    [[nodiscard]] Result<std::unique_ptr<Window>> createWindow(const WindowDesc& desc);

    /// Starts a new input frame on every window, then delivers pending operating system
    /// events and refreshes gamepad state. Call once per frame.
    void pollEvents();

    /// Vulkan instance extensions required to present to a window. Empty when headless
    /// or when the machine has no Vulkan support.
    [[nodiscard]] std::vector<std::string> vulkanInstanceExtensions() const;

    [[nodiscard]] bool isHeadless() const { return m_headless; }

private:
    friend class Window;

    explicit WindowSystem(bool headless);

    bool m_headless = false;
    // Non-owning: windows register and unregister themselves.
    std::vector<Window*> m_windows;
};

} // namespace cordite
