#include "cordite/platform/Window.h"

#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <filesystem>
#include <span>
#include <vector>

namespace cordite {
namespace {

std::atomic<bool> g_windowSystemExists{false};

/// Returns the description of the most recent GLFW error on this thread.
std::string lastGlfwError() {
    const char* description = nullptr;
    const int code = glfwGetError(&description);
    if (code == GLFW_NO_ERROR || description == nullptr) {
        return "unknown error";
    }
    return description;
}

void onGlfwError(int code, const char* description) {
    CORDITE_LOG_ERROR("Window", "GLFW error {:#x}: {}", code, description != nullptr ? description : "");
}

Key keyFromGlfw(int key) {
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        return static_cast<Key>(static_cast<int>(Key::Num0) + (key - GLFW_KEY_0));
    }
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (key - GLFW_KEY_A));
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (key - GLFW_KEY_F1));
    }
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (key - GLFW_KEY_KP_0));
    }

    switch (key) {
        case GLFW_KEY_SPACE:
            return Key::Space;
        case GLFW_KEY_APOSTROPHE:
            return Key::Apostrophe;
        case GLFW_KEY_COMMA:
            return Key::Comma;
        case GLFW_KEY_MINUS:
            return Key::Minus;
        case GLFW_KEY_PERIOD:
            return Key::Period;
        case GLFW_KEY_SLASH:
            return Key::Slash;
        case GLFW_KEY_SEMICOLON:
            return Key::Semicolon;
        case GLFW_KEY_EQUAL:
            return Key::Equal;
        case GLFW_KEY_LEFT_BRACKET:
            return Key::LeftBracket;
        case GLFW_KEY_BACKSLASH:
            return Key::Backslash;
        case GLFW_KEY_RIGHT_BRACKET:
            return Key::RightBracket;
        case GLFW_KEY_GRAVE_ACCENT:
            return Key::GraveAccent;
        case GLFW_KEY_ESCAPE:
            return Key::Escape;
        case GLFW_KEY_ENTER:
            return Key::Enter;
        case GLFW_KEY_TAB:
            return Key::Tab;
        case GLFW_KEY_BACKSPACE:
            return Key::Backspace;
        case GLFW_KEY_INSERT:
            return Key::Insert;
        case GLFW_KEY_DELETE:
            return Key::Delete;
        case GLFW_KEY_RIGHT:
            return Key::Right;
        case GLFW_KEY_LEFT:
            return Key::Left;
        case GLFW_KEY_DOWN:
            return Key::Down;
        case GLFW_KEY_UP:
            return Key::Up;
        case GLFW_KEY_PAGE_UP:
            return Key::PageUp;
        case GLFW_KEY_PAGE_DOWN:
            return Key::PageDown;
        case GLFW_KEY_HOME:
            return Key::Home;
        case GLFW_KEY_END:
            return Key::End;
        case GLFW_KEY_CAPS_LOCK:
            return Key::CapsLock;
        case GLFW_KEY_SCROLL_LOCK:
            return Key::ScrollLock;
        case GLFW_KEY_NUM_LOCK:
            return Key::NumLock;
        case GLFW_KEY_PRINT_SCREEN:
            return Key::PrintScreen;
        case GLFW_KEY_PAUSE:
            return Key::Pause;
        case GLFW_KEY_KP_DECIMAL:
            return Key::KeypadDecimal;
        case GLFW_KEY_KP_DIVIDE:
            return Key::KeypadDivide;
        case GLFW_KEY_KP_MULTIPLY:
            return Key::KeypadMultiply;
        case GLFW_KEY_KP_SUBTRACT:
            return Key::KeypadSubtract;
        case GLFW_KEY_KP_ADD:
            return Key::KeypadAdd;
        case GLFW_KEY_KP_ENTER:
            return Key::KeypadEnter;
        case GLFW_KEY_KP_EQUAL:
            return Key::KeypadEqual;
        case GLFW_KEY_LEFT_SHIFT:
            return Key::LeftShift;
        case GLFW_KEY_LEFT_CONTROL:
            return Key::LeftControl;
        case GLFW_KEY_LEFT_ALT:
            return Key::LeftAlt;
        case GLFW_KEY_LEFT_SUPER:
            return Key::LeftSuper;
        case GLFW_KEY_RIGHT_SHIFT:
            return Key::RightShift;
        case GLFW_KEY_RIGHT_CONTROL:
            return Key::RightControl;
        case GLFW_KEY_RIGHT_ALT:
            return Key::RightAlt;
        case GLFW_KEY_RIGHT_SUPER:
            return Key::RightSuper;
        case GLFW_KEY_MENU:
            return Key::Menu;
        default:
            return Key::Unknown;
    }
}

/// GLFW numbers gamepad buttons and axes in the same order as our enums.
constexpr std::array<int, static_cast<size_t>(GamepadButton::Count)> kGlfwGamepadButtons = {
    GLFW_GAMEPAD_BUTTON_A,          GLFW_GAMEPAD_BUTTON_B,           GLFW_GAMEPAD_BUTTON_X,
    GLFW_GAMEPAD_BUTTON_Y,          GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
    GLFW_GAMEPAD_BUTTON_BACK,       GLFW_GAMEPAD_BUTTON_START,       GLFW_GAMEPAD_BUTTON_GUIDE,
    GLFW_GAMEPAD_BUTTON_LEFT_THUMB, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, GLFW_GAMEPAD_BUTTON_DPAD_UP,
    GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, GLFW_GAMEPAD_BUTTON_DPAD_DOWN,   GLFW_GAMEPAD_BUTTON_DPAD_LEFT,
};

constexpr std::array<int, static_cast<size_t>(GamepadAxis::Count)> kGlfwGamepadAxes = {
    GLFW_GAMEPAD_AXIS_LEFT_X,  GLFW_GAMEPAD_AXIS_LEFT_Y,       GLFW_GAMEPAD_AXIS_RIGHT_X,
    GLFW_GAMEPAD_AXIS_RIGHT_Y, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

} // namespace

struct Window::Impl {
    WindowSystem* system = nullptr;
    GLFWwindow* handle = nullptr;
    Input input;
    CursorMode cursorMode = CursorMode::Normal;

    static Impl& from(GLFWwindow* window) { return *static_cast<Impl*>(glfwGetWindowUserPointer(window)); }

    static void onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
        const Key mapped = keyFromGlfw(key);
        if (mapped == Key::Unknown) {
            return;
        }
        from(window).input.injectKey(mapped, action != GLFW_RELEASE, action == GLFW_REPEAT);
    }

    static void onMouseButton(GLFWwindow* window, int button, int action, int /*mods*/) {
        if (button < 0 || button >= static_cast<int>(MouseButton::Count)) {
            return;
        }
        // GLFW orders buttons left, right, middle, 4, 5 - the same as MouseButton.
        from(window).input.injectMouseButton(static_cast<MouseButton>(button), action == GLFW_PRESS);
    }

    static void onCursorPosition(GLFWwindow* window, double x, double y) {
        from(window).input.injectMouseMove(Vec2(static_cast<float>(x), static_cast<float>(y)));
    }

    static void onScroll(GLFWwindow* window, double x, double y) {
        from(window).input.injectScroll(Vec2(static_cast<float>(x), static_cast<float>(y)));
    }

    static void onCharacter(GLFWwindow* window, unsigned int codepoint) { from(window).input.injectText(codepoint); }

    static void onDrop(GLFWwindow* window, int count, const char** paths) {
        // GLFW names the files in UTF-8 on every platform.
        std::vector<std::filesystem::path> files;
        for (const char* path : std::span(paths, static_cast<size_t>(std::max(count, 0)))) {
            files.push_back(pathFromUtf8(path));
        }
        from(window).input.injectDrop(files);
    }

    static void onFocus(GLFWwindow* window, int focused) {
        if (focused == GLFW_FALSE) {
            // Key-up events are not delivered to an unfocused window.
            from(window).input.releaseAll();
        }
    }
};

Window::Window(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {
    m_impl->system->m_windows.push_back(this);
}

Window::~Window() {
    std::erase(m_impl->system->m_windows, this);
    glfwDestroyWindow(m_impl->handle);
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(m_impl->handle) != GLFW_FALSE;
}

void Window::requestClose() {
    glfwSetWindowShouldClose(m_impl->handle, GLFW_TRUE);
}

UVec2 Window::framebufferSize() const {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_impl->handle, &width, &height);
    return {static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))};
}

UVec2 Window::logicalSize() const {
    int width = 0;
    int height = 0;
    glfwGetWindowSize(m_impl->handle, &width, &height);
    return {static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))};
}

void Window::setLogicalSize(UVec2 size) {
    CORDITE_ASSERT(size.x > 0 && size.y > 0, "window size must be positive, got {}x{}", size.x, size.y);
    glfwSetWindowSize(m_impl->handle, static_cast<int>(std::max(size.x, 1u)), static_cast<int>(std::max(size.y, 1u)));
}

float Window::contentScale() const {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    glfwGetWindowContentScale(m_impl->handle, &scaleX, &scaleY);
    return scaleX > 0.0f ? scaleX : 1.0f;
}

bool Window::isMinimized() const {
    return glfwGetWindowAttrib(m_impl->handle, GLFW_ICONIFIED) != GLFW_FALSE;
}

bool Window::isFocused() const {
    return glfwGetWindowAttrib(m_impl->handle, GLFW_FOCUSED) != GLFW_FALSE;
}

void Window::setTitle(std::string_view title) {
    const std::string terminated(title);
    glfwSetWindowTitle(m_impl->handle, terminated.c_str());
}

void Window::setVisible(bool visible) {
    if (visible) {
        glfwShowWindow(m_impl->handle);
    } else {
        glfwHideWindow(m_impl->handle);
    }
}

CursorMode Window::cursorMode() const {
    return m_impl->cursorMode;
}

void Window::setCursorMode(CursorMode mode) {
    if (mode == m_impl->cursorMode) {
        return;
    }

    int glfwMode = GLFW_CURSOR_NORMAL;
    if (mode == CursorMode::Hidden) {
        glfwMode = GLFW_CURSOR_HIDDEN;
    } else if (mode == CursorMode::Captured) {
        glfwMode = GLFW_CURSOR_DISABLED;
    }
    glfwSetInputMode(m_impl->handle, GLFW_CURSOR, glfwMode);

    // Unaccelerated motion gives camera control the same feel at any pointer speed setting.
    if (glfwRawMouseMotionSupported() != GLFW_FALSE) {
        glfwSetInputMode(m_impl->handle, GLFW_RAW_MOUSE_MOTION, mode == CursorMode::Captured ? GLFW_TRUE : GLFW_FALSE);
    }

    m_impl->cursorMode = mode;
    // Capturing or releasing the cursor teleports it; do not report that as movement.
    m_impl->input.resetMouseTracking();
}

Input& Window::input() {
    return m_impl->input;
}

const Input& Window::input() const {
    return m_impl->input;
}

Result<uint64_t> Window::createVulkanSurface(void* vulkanInstance) const {
    if (m_impl->system->isHeadless()) {
        return makeError(ErrorCode::Unsupported, "a headless window system cannot create Vulkan surfaces");
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result =
        glfwCreateWindowSurface(static_cast<VkInstance>(vulkanInstance), m_impl->handle, nullptr, &surface);
    if (result != VK_SUCCESS) {
        return makeError(ErrorCode::DeviceError, "cannot create a Vulkan surface (VkResult {}): {}",
                         static_cast<int>(result), lastGlfwError());
    }

    // VkSurfaceKHR is a 64-bit non-dispatchable handle on every platform we support;
    // bit_cast refuses to compile if that ever stops being true.
    return std::bit_cast<uint64_t>(surface);
}

void* Window::nativeHandle() const {
    return m_impl->handle;
}

Result<std::unique_ptr<WindowSystem>> WindowSystem::create(const WindowSystemDesc& desc) {
    if (g_windowSystemExists.exchange(true)) {
        return makeError(ErrorCode::InvalidState, "a WindowSystem already exists");
    }

    glfwSetErrorCallback(&onGlfwError);
    if (desc.headless) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
    } else {
        glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    }

    if (glfwInit() == GLFW_FALSE) {
        g_windowSystemExists = false;
        return makeError(ErrorCode::DeviceError, "cannot initialize the window system: {}", lastGlfwError());
    }

    return std::unique_ptr<WindowSystem>(new WindowSystem(desc.headless));
}

WindowSystem::WindowSystem(bool headless) : m_headless(headless) {}

WindowSystem::~WindowSystem() {
    CORDITE_ASSERT(m_windows.empty(), "{} window(s) outlived the WindowSystem", m_windows.size());
    glfwTerminate();
    g_windowSystemExists = false;
}

Result<std::unique_ptr<Window>> WindowSystem::createWindow(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return makeError(ErrorCode::InvalidArgument, "window size must be positive, got {}x{}", desc.width,
                         desc.height);
    }

    glfwDefaultWindowHints();
    // Rendering goes through Vulkan; no OpenGL context is wanted.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);

    int width = static_cast<int>(desc.width);
    int height = static_cast<int>(desc.height);
    if (desc.fullscreen) {
        // A borderless window the size of the monitor switches instantly and does not
        // change the display mode.
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        if (mode != nullptr) {
            width = mode->width;
            height = mode->height;
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);
        }
    }

    GLFWwindow* handle = glfwCreateWindow(width, height, desc.title.c_str(), nullptr, nullptr);
    if (handle == nullptr) {
        return makeError(ErrorCode::DeviceError, "cannot create window '{}': {}", desc.title, lastGlfwError());
    }
    if (desc.fullscreen) {
        glfwSetWindowPos(handle, 0, 0);
    }

    auto impl = std::make_unique<Window::Impl>();
    impl->system = this;
    impl->handle = handle;

    glfwSetWindowUserPointer(handle, impl.get());
    glfwSetKeyCallback(handle, &Window::Impl::onKey);
    glfwSetMouseButtonCallback(handle, &Window::Impl::onMouseButton);
    glfwSetCursorPosCallback(handle, &Window::Impl::onCursorPosition);
    glfwSetScrollCallback(handle, &Window::Impl::onScroll);
    glfwSetCharCallback(handle, &Window::Impl::onCharacter);
    glfwSetDropCallback(handle, &Window::Impl::onDrop);
    glfwSetWindowFocusCallback(handle, &Window::Impl::onFocus);

    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

void WindowSystem::pollEvents() {
    for (Window* window : m_windows) {
        window->input().beginFrame();
    }

    glfwPollEvents();

    // Gamepads belong to the machine rather than to a window; every window sees them.
    for (uint32_t index = 0; index < Input::kMaxGamepads; ++index) {
        GLFWgamepadstate glfwState;
        GamepadState state;
        const bool connected = glfwGetGamepadState(GLFW_JOYSTICK_1 + static_cast<int>(index), &glfwState) != GLFW_FALSE;
        if (connected) {
            for (size_t button = 0; button < kGlfwGamepadButtons.size(); ++button) {
                state.buttons.set(button, glfwState.buttons[kGlfwGamepadButtons[button]] == GLFW_PRESS);
            }
            for (size_t axis = 0; axis < kGlfwGamepadAxes.size(); ++axis) {
                state.axes[axis] = glfwState.axes[kGlfwGamepadAxes[axis]];
            }
            // GLFW reports triggers in [-1, 1]; we use [0, 1].
            for (const GamepadAxis trigger : {GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger}) {
                float& value = state.axes[static_cast<size_t>(trigger)];
                value = saturate(value * 0.5f + 0.5f);
            }
        }
        for (Window* window : m_windows) {
            window->input().injectGamepad(index, connected ? &state : nullptr);
        }
    }
}

std::vector<std::string> WindowSystem::vulkanInstanceExtensions() const {
    if (m_headless || glfwVulkanSupported() == GLFW_FALSE) {
        return {};
    }

    uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (names == nullptr) {
        return {};
    }
    return {names, names + count};
}

} // namespace cordite
