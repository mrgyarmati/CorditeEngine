#pragma once

/// @file
/// The immediate-mode user interface, built on Dear ImGui.
///
/// The editor is made of it, and games use it for debug overlays. A UiSystem owns the
/// ImGui context; between beginFrame() and render(), any code may call `ImGui::` functions
/// to describe what is on screen this frame.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto ui, UiSystem::create(device, shaders, {.window = window}));
///
/// ui->beginFrame(frameSize, deltaSeconds);
/// if (ImGui::Begin("Stats")) {
///     ImGui::Text("%.1f ms", deltaSeconds * 1000.0f);
///     ImGui::Image(UiSystem::textureId(sceneColor), ImVec2(320.0f, 180.0f));
/// }
/// ImGui::End();
/// ui->render(commandList, framebuffer);
/// @endcode
///
/// Without a window it runs headless: the interface is laid out and drawn the same way,
/// with pointer input supplied through setPointerState(). Tests and the editor's
/// automation mode use that.
///
/// Only one UiSystem can exist at a time, because ImGui's context is process-wide.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/platform/Window.h"
#include "cordite/renderer/ShaderManager.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <string>

namespace cordite {

class UiRenderer;

struct UiSystemDesc {
    /// The window that supplies input. Null runs headless.
    Window* window = nullptr;

    /// File in which window positions and the docking layout are remembered between
    /// sessions. Empty keeps nothing, so every run starts from the default layout.
    std::filesystem::path settingsFile;

    /// Text height in pixels at 100% display scaling.
    float fontSize = 15.0f;
};

/// Where the pointer is and which of its buttons are held, for a headless UiSystem.
struct UiPointerState {
    /// In pixels from the top-left corner of the interface.
    Vec2 position{0.0f};
    bool leftDown = false;
    bool rightDown = false;
    bool middleDown = false;
    /// Wheel movement since the previous frame, positive away from the user.
    float wheel = 0.0f;
};

/// How much larger than at 100% the interface has to be drawn in a window: the scale the
/// operating system asks for (Window::contentScale), less what the window's framebuffer
/// already provides. On Windows and X11 a window has exactly as many pixels as its size
/// says, and the whole scale is the interface's to apply. On macOS and Wayland a window
/// at "200%" already has twice the pixels, which ImGui accounts for by itself; scaling
/// again would make everything twice too large. The result is kept between 0.5 and 4.
[[nodiscard]] float interfaceScale(float contentScale, UVec2 logicalSize, UVec2 framebufferSize);

class UiSystem {
public:
    /// Creates the ImGui context. Fails with InvalidState when another UiSystem exists.
    [[nodiscard]] static Result<std::unique_ptr<UiSystem>> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                                  const UiSystemDesc& desc = {});
    ~UiSystem();
    CORDITE_NON_MOVABLE(UiSystem);

    /// Starts describing a frame. `displaySize` is the size in pixels of the target the
    /// interface will be drawn on; `deltaSeconds` drives animations and double-click
    /// timing. Every beginFrame() must be followed by render() or discardFrame().
    void beginFrame(UVec2 displaySize, float deltaSeconds);

    /// Finishes the frame and draws it over the framebuffer. The command list must be open.
    void render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer);

    /// Finishes the frame without drawing it, for frames that are not shown.
    void discardFrame();

    /// True between beginFrame() and render() or discardFrame().
    [[nodiscard]] bool isFrameOpen() const { return m_frameOpen; }

    /// Supplies the pointer for the next frame. Only for a UiSystem without a window; one
    /// with a window reads the real pointer.
    void setPointerState(const UiPointerState& state);

    /// True when the interface is using the pointer or the keyboard, so the game or the
    /// viewport should ignore them this frame.
    [[nodiscard]] bool wantsPointer() const;
    [[nodiscard]] bool wantsKeyboard() const;

    /// The identifier that shows an engine texture in `ImGui::Image` and similar widgets.
    /// The texture must hold display-ready (sRGB-encoded) colors.
    [[nodiscard]] static ImTextureID textureId(nvrhi::ITexture* texture);

private:
    UiSystem() = default;

    std::unique_ptr<UiRenderer> m_renderer;
    /// ImGui keeps a pointer to this text for as long as the context lives.
    std::string m_settingsFile;
    bool m_contextCreated = false;
    bool m_platformBackend = false;
    bool m_frameOpen = false;
};

} // namespace cordite
