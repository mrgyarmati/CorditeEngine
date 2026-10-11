#pragma once

/// @file
/// The textures one view of a scene is rendered through.
///
/// A scene is drawn in linear light with a depth buffer, and the result is then turned
/// into the display-ready image that is shown, captured or drawn over. Each view - the
/// game's, an editor viewport's - has a SceneTargets of its own, sized to what it shows.
///
/// @code
/// CORDITE_TRY_ASSIGN(SceneTargets targets, SceneTargets::create(device, {1280, 720}, "Game view"));
///
/// sceneRenderer.render(commandList, world, view, targets);
/// showOrSave(targets.display());
///
/// // Draw tool overlays over the finished image, hidden where the scene is nearer:
/// linePass.draw(commandList, targets.overlayFramebuffer(), lines, {.viewProjection = view.viewProjection});
///
/// CORDITE_TRY(targets.resize(device, newSize));
/// @endcode
///
/// | Texture      | Format       | Holds                                               |
/// |--------------|--------------|-----------------------------------------------------|
/// | scene color  | RGBA16_FLOAT | Linear light, not limited to the range of a display |
/// | depth        | D32          | Reversed depth: 1 at the near plane, 0 far away     |
/// | display      | RGBA8_UNORM  | sRGB-encoded color, as a screen expects it          |

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"

#include <nvrhi/nvrhi.h>

#include <string>

namespace cordite {

class SceneTargets {
public:
    /// An empty set: isValid() is false until it is assigned one made by create().
    SceneTargets() = default;

    /// `size` is in pixels, at least 1 and at most 16384 a side. `debugName` prefixes
    /// the names graphics debuggers show.
    [[nodiscard]] static Result<SceneTargets> create(nvrhi::IDevice* device, UVec2 size,
                                                     std::string debugName = "Scene");

    [[nodiscard]] bool isValid() const { return m_sceneFramebuffer != nullptr; }

    [[nodiscard]] UVec2 size() const { return m_size; }

    [[nodiscard]] nvrhi::ITexture* sceneColor() const { return m_sceneColor; }

    [[nodiscard]] nvrhi::ITexture* depth() const { return m_depth; }

    [[nodiscard]] nvrhi::ITexture* display() const { return m_display; }

    /// Scene color and depth: what meshes are drawn into.
    [[nodiscard]] nvrhi::IFramebuffer* sceneFramebuffer() const { return m_sceneFramebuffer; }

    /// The display image alone: what the scene color is encoded into.
    [[nodiscard]] nvrhi::IFramebuffer* displayFramebuffer() const { return m_displayFramebuffer; }

    /// The display image with the scene's depth: for what is drawn over the finished
    /// picture but belongs in the scene, such as debug lines and editor gizmos.
    [[nodiscard]] nvrhi::IFramebuffer* overlayFramebuffer() const { return m_overlayFramebuffer; }

    /// Fills scene color with `background` (linear) and depth with "far". The command
    /// list must be open.
    void clear(nvrhi::ICommandList* commandList, const Vec3& background) const;

    /// Replaces the textures with ones of a new size. Does nothing when the size is
    /// unchanged. The old textures stay alive until the GPU has finished with them;
    /// their content is not carried over.
    [[nodiscard]] Status resize(nvrhi::IDevice* device, UVec2 size);

private:
    UVec2 m_size{0, 0};
    std::string m_debugName;
    nvrhi::TextureHandle m_sceneColor;
    nvrhi::TextureHandle m_depth;
    nvrhi::TextureHandle m_display;
    nvrhi::FramebufferHandle m_sceneFramebuffer;
    nvrhi::FramebufferHandle m_displayFramebuffer;
    nvrhi::FramebufferHandle m_overlayFramebuffer;
};

} // namespace cordite
