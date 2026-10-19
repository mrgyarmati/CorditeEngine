#include "cordite/core/Base.h"
#include "cordite/platform/Window.h"

// What GLFW says of the windows of a system takes the headers of that system: they are
// included here, in a source of their own, where they meet nothing else of the engine.
#if CORDITE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#elif CORDITE_PLATFORM_MACOS
#define GLFW_EXPOSE_NATIVE_COCOA
#endif

#include <GLFW/glfw3.h>

#if CORDITE_PLATFORM_WINDOWS || CORDITE_PLATFORM_MACOS
#include <GLFW/glfw3native.h>
#endif

namespace cordite {

SystemWindow Window::systemWindow() const {
    auto* window = static_cast<GLFWwindow*>(nativeHandle());
    if (window == nullptr) {
        return {};
    }
#if CORDITE_PLATFORM_WINDOWS
    return {.kind = SystemWindow::Kind::Windows, .handle = glfwGetWin32Window(window)};
#elif CORDITE_PLATFORM_MACOS
    // An `id` outside Objective-C is a pointer like any other.
    return {.kind = SystemWindow::Kind::Cocoa, .handle = glfwGetCocoaWindow(window)};
#else
    return {};
#endif
}

} // namespace cordite
