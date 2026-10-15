#include "cordite/ui/UiSystem.h"

#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/ui/UiRenderer.h"

#include <backends/imgui_impl_glfw.h>

#include <algorithm>
#include <cmath>

namespace cordite {
namespace {

/// The one UiSystem there is (ImGui's context is the process's), for palette() and font().
UiSystem* g_currentSystem = nullptr;

} // namespace

float interfaceScale(float contentScale, UVec2 logicalSize, UVec2 framebufferSize) {
    constexpr float kMinimumScale = 0.5f;
    constexpr float kMaximumScale = 4.0f;

    const float requested = std::isfinite(contentScale) && contentScale > 0.0f ? contentScale : 1.0f;
    // A minimized window has no framebuffer; assume one pixel per unit then.
    const float provided = logicalSize.x > 0 && framebufferSize.x > 0
                               ? static_cast<float>(framebufferSize.x) / static_cast<float>(logicalSize.x)
                               : 1.0f;
    return std::clamp(requested / provided, kMinimumScale, kMaximumScale);
}

Result<std::unique_ptr<UiSystem>> UiSystem::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                   const UiSystemDesc& desc) {
    if (ImGui::GetCurrentContext() != nullptr) {
        return makeError(ErrorCode::InvalidState, "a UiSystem already exists; only one can exist at a time");
    }
    if (desc.fontSize < 6.0f || desc.fontSize > 96.0f) {
        return makeError(ErrorCode::InvalidArgument, "the interface font size must be between 6 and 96 pixels, got {}",
                         desc.fontSize);
    }

    // Created before anything that can fail, so that the destructor undoes whatever part
    // of the set-up has happened.
    std::unique_ptr<UiSystem> system(new UiSystem());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    system->m_contextCreated = true;
    g_currentSystem = system.get();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.LogFilename = nullptr;

    if (desc.settingsFile.empty()) {
        io.IniFilename = nullptr;
    } else {
        if (const std::filesystem::path directory = desc.settingsFile.parent_path(); !directory.empty()) {
            CORDITE_TRY(ensureDirectory(directory));
        }
        system->m_settingsFile = pathToUtf8(desc.settingsFile);
        io.IniFilename = system->m_settingsFile.c_str();
    }

    // Sizes are for 100% scaling; a high-DPI display gets them enlarged, and the text with
    // them.
    system->m_scale = desc.window != nullptr ? interfaceScale(desc.window->contentScale(), desc.window->logicalSize(),
                                                              desc.window->framebufferSize())
                                             : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = desc.fontSize;
    style.FontScaleDpi = system->m_scale;
    system->m_theme = desc.theme;
    applyTheme(style, desc.theme, system->m_scale);
    system->m_fonts = addInterfaceFonts(*io.Fonts, desc.fontSize);

    if (desc.window != nullptr) {
        // ImGui's GLFW backend handles the keyboard, the pointer and its shapes, the
        // clipboard and text input. It chains to the callbacks the window installed, so
        // the engine's own Input keeps working.
        const bool installCallbacks = true;
        if (!ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(desc.window->nativeHandle()), installCallbacks)) {
            return makeError(ErrorCode::Unsupported, "cannot connect the user interface to the window");
        }
        system->m_platformBackend = true;
    } else {
        io.BackendPlatformName = "cordite_headless";
    }

    CORDITE_TRY_ASSIGN(system->m_renderer, UiRenderer::create(device, shaders));
    return system;
}

UiSystem::~UiSystem() {
    if (g_currentSystem == this) {
        g_currentSystem = nullptr;
    }
    if (!m_contextCreated) {
        return;
    }
    if (m_frameOpen) {
        ImGui::EndFrame();
    }

    // In reverse order of creation; the renderer needs the context to release its textures.
    m_renderer.reset();
    if (m_platformBackend) {
        ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext();
}

void UiSystem::beginFrame(UVec2 displaySize, float deltaSeconds) {
    CORDITE_ASSERT(!m_frameOpen, "the previous interface frame was neither rendered nor discarded");
    if (m_frameOpen) {
        ImGui::EndFrame();
    }

    ImGuiIO& io = ImGui::GetIO();
    if (m_platformBackend) {
        // Sets the display size, the content scale and the pointer from the window.
        ImGui_ImplGlfw_NewFrame();
    } else {
        io.DisplaySize = ImVec2(static_cast<float>(displaySize.x), static_cast<float>(displaySize.y));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }

    // The engine's clock, not the wall clock, so that a fixed time step also makes the
    // interface deterministic. ImGui requires a positive value.
    constexpr float kMinimumDelta = 1.0e-5f;
    io.DeltaTime = std::max(deltaSeconds, kMinimumDelta);

    // A theme chosen during the last frame is put in before anything of this one is pushed.
    if (m_pendingTheme) {
        m_theme = *m_pendingTheme;
        m_pendingTheme.reset();
        applyTheme(ImGui::GetStyle(), m_theme, m_scale);
    }

    ImGui::NewFrame();
    m_frameOpen = true;
}

void UiSystem::render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer) {
    CORDITE_ASSERT(m_frameOpen, "render() needs a frame started with beginFrame()");
    if (!m_frameOpen) {
        return;
    }

    ImGui::Render();
    m_frameOpen = false;
    m_renderer->render(commandList, framebuffer, ImGui::GetDrawData());
}

void UiSystem::discardFrame() {
    if (m_frameOpen) {
        ImGui::EndFrame();
        m_frameOpen = false;
    }
}

// The pointer belongs to the interface this object owns, although ImGui keeps it in its
// global context rather than in a member.
// NOLINTNEXTLINE(readability-make-member-function-const)
void UiSystem::setPointerState(const UiPointerState& state) {
    CORDITE_ASSERT(!m_platformBackend, "a UiSystem with a window reads the pointer from it");
    if (m_platformBackend) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(state.position.x, state.position.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, state.leftDown);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, state.rightDown);
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, state.middleDown);
    if (state.wheel != 0.0f) {
        io.AddMouseWheelEvent(0.0f, state.wheel);
    }
}

// These two ask the interface this object owns; that ImGui stores the answer globally
// does not make them free functions.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool UiSystem::wantsPointer() const {
    return ImGui::GetIO().WantCaptureMouse;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool UiSystem::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

ImTextureID UiSystem::textureId(nvrhi::ITexture* texture) {
    return UiRenderer::textureId(texture);
}

void UiSystem::setTheme(UiTheme theme) {
    if (m_frameOpen) {
        m_pendingTheme = theme;
        return;
    }
    m_pendingTheme.reset();
    m_theme = theme;
    applyTheme(ImGui::GetStyle(), theme, m_scale);
}

const UiPalette& UiSystem::palette() {
    return paletteOf(g_currentSystem != nullptr ? g_currentSystem->m_theme : UiTheme::Dark);
}

ImFont* UiSystem::font(UiFont font) {
    const auto index = static_cast<size_t>(font);
    CORDITE_ASSERT(index < kUiFonts.size(), "there is no typeface {}", index);
    return g_currentSystem != nullptr && index < kUiFonts.size() ? g_currentSystem->m_fonts[index] : nullptr;
}

} // namespace cordite
