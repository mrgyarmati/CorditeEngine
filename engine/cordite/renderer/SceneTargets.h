#pragma once

/// @file
/// The textures one view of a scene is rendered through.
///
/// Solid surfaces are first written down in the G-buffer - what they are made of, which
/// way they face - and lit from there into the scene color; translucent ones are then
/// drawn over it. The result is turned into the display-ready image that is shown,
/// captured or drawn over. Each view - the game's, an editor viewport's - has a
/// SceneTargets of its own, sized to what it shows.
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
/// | Texture         | Format        | Holds                                               |
/// |-----------------|---------------|-----------------------------------------------------|
/// | G-buffer 0      | SRGBA8_UNORM  | rgb: base color, a: lighting channels               |
/// | G-buffer 1      | RG16_UNORM    | The world-space normal, octahedrally mapped         |
/// | G-buffer 2      | RGBA8_UNORM   | Roughness, metallic, specular, occlusion            |
/// | G-buffer 3      | R11G11B10F    | Emissive light                                      |
/// | depth           | D32           | Reversed depth: 1 at the near plane, 0 far away     |
/// | scene color     | RGBA16_FLOAT  | Linear light, not limited to the range of a display |
/// | bloom levels    | RGBA16_FLOAT  | rgb: what blooms, blurred; a: log of the brightness |
/// | exposure, twice | R16_FLOAT     | One texel: the stops automatic exposure adds        |
/// | display         | RGBA8_UNORM   | sRGB-encoded color, as a screen expects it          |
///
/// The shader side of the G-buffer is shaders/common/GBuffer.slang. The bloom levels and
/// the exposure are what PostProcessPass works with. The exposure is the one thing here
/// that lasts from frame to frame: a view gets used to light slowly, so each frame
/// starts from the exposure of the one before.

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cordite {

/// The color targets of the G-buffer, in the order the base pass writes them.
enum class GBufferTarget : uint8_t {
    /// rgb: base color. Stored sRGB-encoded, which spends the eight bits where the eye
    /// tells shades apart; reads and writes are in linear values. a: the lighting
    /// channels of the surface, as a number out of 255.
    BaseColor,
    /// rg: the unit normal in world space, as a point of an unfolded octahedron.
    Normal,
    /// r: roughness, g: metallic, b: specular, a: occlusion.
    Parameters,
    /// rgb: the light the surface gives off, linear and not limited to one.
    Emissive,
};

inline constexpr size_t kGBufferTargetCount = 4;

/// The most levels the bloom chain of a view has. Each is half the size of the one
/// before; the first is half the size of the view.
inline constexpr uint32_t kMaxBloomLevels = 8;

std::string_view nameOf(GBufferTarget target);

/// The format of a G-buffer target.
[[nodiscard]] nvrhi::Format formatOf(GBufferTarget target);

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

    [[nodiscard]] nvrhi::ITexture* gbuffer(GBufferTarget target) const {
        return m_gbuffer[static_cast<size_t>(target)];
    }

    [[nodiscard]] nvrhi::ITexture* sceneColor() const { return m_sceneColor; }

    [[nodiscard]] nvrhi::ITexture* depth() const { return m_depth; }

    [[nodiscard]] nvrhi::ITexture* display() const { return m_display; }

    /// How many levels the bloom chain has: as many as it takes to get from half the
    /// size of the view down to one texel, and kMaxBloomLevels at the most.
    [[nodiscard]] uint32_t bloomLevelCount() const { return m_bloomLevelCount; }

    /// A level of the bloom chain, 0 being the largest, and the framebuffer that draws
    /// into it. `level` must be less than bloomLevelCount().
    [[nodiscard]] nvrhi::ITexture* bloomLevel(uint32_t level) const;
    [[nodiscard]] nvrhi::IFramebuffer* bloomFramebuffer(uint32_t level) const;

    /// The stops automatic exposure adds to the view, in a texture of one texel: what
    /// the last frame that measured them left. Means something when hasExposure().
    [[nodiscard]] nvrhi::ITexture* exposure() const { return m_exposure[m_exposureIndex]; }

    /// Which of the two exposure textures exposure() is: 0 or 1.
    [[nodiscard]] uint32_t exposureIndex() const { return m_exposureIndex; }

    /// Where a frame writes the exposure it arrives at, while exposure() is still that
    /// of the frame before. advanceExposure() then makes it exposure().
    [[nodiscard]] nvrhi::ITexture* nextExposure() const { return m_exposure[1 - m_exposureIndex]; }

    [[nodiscard]] nvrhi::IFramebuffer* nextExposureFramebuffer() const {
        return m_exposureFramebuffers[1 - m_exposureIndex];
    }

    void advanceExposure() {
        m_exposureIndex = 1 - m_exposureIndex;
        m_hasExposure = true;
    }

    /// False until a frame has measured an exposure, and again after forgetExposure():
    /// the next one measured is then taken as it is, with no getting used to it.
    [[nodiscard]] bool hasExposure() const { return m_hasExposure; }

    void forgetExposure() { m_hasExposure = false; }

    /// A number no other set of textures has had: create() and resize() give a new
    /// one. What is kept for a set of targets elsewhere is told apart by it.
    [[nodiscard]] uint64_t serial() const { return m_serial; }

    /// Depth alone: what the depth pre-pass draws into.
    [[nodiscard]] nvrhi::IFramebuffer* depthFramebuffer() const { return m_depthFramebuffer; }

    /// The G-buffer targets with depth: what the base pass draws into.
    [[nodiscard]] nvrhi::IFramebuffer* gbufferFramebuffer() const { return m_gbufferFramebuffer; }

    /// Scene color alone: what the passes that read the depth as a texture draw into,
    /// the lighting first of all.
    [[nodiscard]] nvrhi::IFramebuffer* lightingFramebuffer() const { return m_lightingFramebuffer; }

    /// Scene color and depth: what the forward pass draws into.
    [[nodiscard]] nvrhi::IFramebuffer* sceneFramebuffer() const { return m_sceneFramebuffer; }

    /// The display image alone: what the scene color is encoded into.
    [[nodiscard]] nvrhi::IFramebuffer* displayFramebuffer() const { return m_displayFramebuffer; }

    /// The display image with the scene's depth: for what is drawn over the finished
    /// picture but belongs in the scene, such as debug lines and editor gizmos.
    [[nodiscard]] nvrhi::IFramebuffer* overlayFramebuffer() const { return m_overlayFramebuffer; }

    /// Fills scene color with `background` (linear), depth with "far" and the G-buffer
    /// with zeros. The command list must be open.
    void clear(nvrhi::ICommandList* commandList, const Vec3& background) const;

    /// Replaces the textures with ones of a new size. Does nothing when the size is
    /// unchanged. The old textures stay alive until the GPU has finished with them;
    /// their content is not carried over.
    [[nodiscard]] Status resize(nvrhi::IDevice* device, UVec2 size);

private:
    UVec2 m_size{0, 0};
    std::string m_debugName;
    std::array<nvrhi::TextureHandle, kGBufferTargetCount> m_gbuffer;
    nvrhi::TextureHandle m_sceneColor;
    nvrhi::TextureHandle m_depth;
    nvrhi::TextureHandle m_display;
    std::array<nvrhi::TextureHandle, kMaxBloomLevels> m_bloom;
    std::array<nvrhi::FramebufferHandle, kMaxBloomLevels> m_bloomFramebuffers;
    uint32_t m_bloomLevelCount = 0;
    std::array<nvrhi::TextureHandle, 2> m_exposure;
    std::array<nvrhi::FramebufferHandle, 2> m_exposureFramebuffers;
    /// Which of the two holds the exposure of the last frame that measured one.
    uint32_t m_exposureIndex = 0;
    bool m_hasExposure = false;
    uint64_t m_serial = 0;
    nvrhi::FramebufferHandle m_depthFramebuffer;
    nvrhi::FramebufferHandle m_gbufferFramebuffer;
    nvrhi::FramebufferHandle m_lightingFramebuffer;
    nvrhi::FramebufferHandle m_sceneFramebuffer;
    nvrhi::FramebufferHandle m_displayFramebuffer;
    nvrhi::FramebufferHandle m_overlayFramebuffer;
};

} // namespace cordite
