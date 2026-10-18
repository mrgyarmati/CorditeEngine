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
/// | motion          | RG16_FLOAT    | How far each pixel moved since the last picture     |
/// | ambient occl.   | R8_UNORM      | How much of the light from all around gets through  |
/// | traced occl.    | RG16_FLOAT    | The same before it is smoothed, and the depth; made |
/// |                 |               | when ambient occlusion is first drawn, at its size  |
/// | scene color     | RGBA16_FLOAT  | Linear light, not limited to the range of a display |
/// | bloom levels    | RGBA16_FLOAT  | rgb: what blooms, blurred; a: log of the brightness |
/// | exposure, twice | R16_FLOAT     | One texel: the stops automatic exposure adds        |
/// | display         | RGBA8_UNORM   | sRGB-encoded color, as a screen expects it          |
///
/// The shader side of the G-buffer is shaders/common/GBuffer.slang. The motion vectors
/// are written by the base pass beside it (MotionVectors.h). The ambient
/// occlusion is what AmbientOcclusionPass makes of the depth and the normals, for the
/// lighting to read; it means something in a frame in which that pass ran. The bloom levels and
/// the exposure are what PostProcessPass works with. Two things here last from frame to
/// frame: the exposure - a view gets used to light slowly, so each frame starts from the
/// exposure of the one before - and what the view remembers of its last picture, for the
/// motion of the next (motionHistory()).

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/MotionVectors.h"

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

    /// How far, and which way, what each pixel shows moved since the view was last drawn
    /// (MotionVectors.h), as two half floats: what the base pass writes for the solid
    /// surfaces it draws, and leaves at nothing where nothing was drawn.
    [[nodiscard]] nvrhi::ITexture* motion() const { return m_motion; }

    /// What the view remembers of the last time it was drawn, for the motion of the next
    /// picture (MotionVectors.h). Like the exposure, it is gone when the view changes its
    /// size.
    [[nodiscard]] MotionHistory& motionHistory() { return m_motionHistory; }

    [[nodiscard]] const MotionHistory& motionHistory() const { return m_motionHistory; }

    /// How much of the light from all around reaches what each pixel shows: 1 all of
    /// it, 0 none. It holds what AmbientOcclusionPass last drew; a view that does not
    /// draw it tells its passes not to read it (SceneView::ambientOcclusion).
    [[nodiscard]] nvrhi::ITexture* ambientOcclusion() const { return m_ambientOcclusion; }

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

    /// The G-buffer targets, then the motion vectors, with depth: what the base pass draws
    /// into.
    [[nodiscard]] nvrhi::IFramebuffer* gbufferFramebuffer() const { return m_gbufferFramebuffer; }

    /// The ambient occlusion alone.
    [[nodiscard]] nvrhi::IFramebuffer* ambientOcclusionFramebuffer() const { return m_ambientOcclusionFramebuffer; }

    /// Makes sure there is an image for the first pass of the ambient occlusion to
    /// draw into, with a texel for every `divisor` pixels each way (1 to 4): makes it,
    /// or replaces the one of another size. Fails when the device cannot create it; the
    /// targets then hold none.
    [[nodiscard]] Status prepareTracedOcclusion(nvrhi::IDevice* device, uint32_t divisor);

    /// Lets go of that image: for a view that draws no ambient occlusion any more.
    void releaseTracedOcclusion();

    /// The image prepareTracedOcclusion() made - r: the share of the light that gets
    /// through, g: the distance from the camera - and the framebuffer that draws into
    /// it. Null before that call and after releaseTracedOcclusion().
    [[nodiscard]] nvrhi::ITexture* tracedOcclusion() const { return m_tracedOcclusion; }

    [[nodiscard]] nvrhi::IFramebuffer* tracedOcclusionFramebuffer() const { return m_tracedOcclusionFramebuffer; }

    /// The memory of that image, in bytes: nothing when there is none.
    [[nodiscard]] uint64_t tracedOcclusionBytes() const;

    /// Makes sure there are the images the reflections from the screen are drawn with
    /// (ScreenReflectionPass): a copy of the scene color, as large as the view, and an
    /// image of what was found, with a texel for every `divisor` pixels each way (1 to 4).
    /// Makes them, or replaces those of another divisor. Fails when the device cannot
    /// create them; the targets then hold none.
    [[nodiscard]] Status prepareScreenReflections(nvrhi::IDevice* device, uint32_t divisor);

    /// Lets go of those images: for a view that draws no reflections from the screen any
    /// more.
    void releaseScreenReflections();

    /// The copy of the scene color that prepareScreenReflections() made, which the
    /// reflections are taken from while the scene color is drawn into. Null before that
    /// call and after releaseScreenReflections().
    [[nodiscard]] nvrhi::ITexture* reflectionSource() const { return m_reflectionSource; }

    /// What was found, and the framebuffer that draws into it: where in the picture the
    /// reflection of a pixel met what was drawn (r, g), how much that is trusted (b) and
    /// whether it met anything (a), in numbers of 16 bits from 0 to 1. Null before
    /// prepareScreenReflections() and after releaseScreenReflections().
    [[nodiscard]] nvrhi::ITexture* reflectionHits() const { return m_reflectionHits; }

    [[nodiscard]] nvrhi::IFramebuffer* reflectionHitsFramebuffer() const { return m_reflectionHitsFramebuffer; }

    /// The memory of those images, in bytes: nothing when there are none.
    [[nodiscard]] uint64_t screenReflectionBytes() const;

    /// Makes sure there is an image for the finished picture to be drawn into before
    /// its edges are smoothed (PostProcessPass): as large as the display image, in the
    /// colors of a display, with the brightness of each pixel in alpha. Fails when the
    /// device cannot create it; the targets then hold none.
    [[nodiscard]] Status prepareAliased(nvrhi::IDevice* device);

    /// Lets go of that image: for a view whose picture is not anti-aliased any more.
    void releaseAliased();

    /// The image prepareAliased() made, and the framebuffer that draws into it. Null
    /// before that call and after releaseAliased().
    [[nodiscard]] nvrhi::ITexture* aliased() const { return m_aliased; }

    [[nodiscard]] nvrhi::IFramebuffer* aliasedFramebuffer() const { return m_aliasedFramebuffer; }

    /// The memory of that image, in bytes: nothing when there is none.
    [[nodiscard]] uint64_t aliasedBytes() const;

    /// Makes sure there are the two images depth of field is worked out in
    /// (DepthOfFieldPass): each half as wide and as high as the view - rounded up - in
    /// light without an upper limit, with how blurred each texel is in alpha. Fails when
    /// the device cannot create them; the targets then hold neither.
    [[nodiscard]] Status prepareDepthOfField(nvrhi::IDevice* device);

    /// Lets go of those images: for a view that has no depth of field any more.
    void releaseDepthOfField();

    /// The images prepareDepthOfField() made - what the picture is blurred from, and the
    /// blur - and the framebuffers that draw into them. Null before that call and after
    /// releaseDepthOfField().
    [[nodiscard]] nvrhi::ITexture* depthOfFieldSource() const { return m_depthOfFieldSource; }

    [[nodiscard]] nvrhi::IFramebuffer* depthOfFieldSourceFramebuffer() const { return m_depthOfFieldSourceFramebuffer; }

    [[nodiscard]] nvrhi::ITexture* depthOfFieldBlur() const { return m_depthOfFieldBlur; }

    [[nodiscard]] nvrhi::IFramebuffer* depthOfFieldBlurFramebuffer() const { return m_depthOfFieldBlurFramebuffer; }

    /// The memory of those images, in bytes: nothing when there are none.
    [[nodiscard]] uint64_t depthOfFieldBytes() const;

    /// Makes sure there are the two images the light shafts are worked out in
    /// (LightShaftPass): each half as wide and as high as the view - rounded up - in light
    /// without an upper limit. Fails when the device cannot create them; the targets then
    /// hold neither.
    [[nodiscard]] Status prepareLightShafts(nvrhi::IDevice* device);

    /// Lets go of those images: for a view that has no light shafts any more.
    void releaseLightShafts();

    /// One of the two images prepareLightShafts() made, 0 or 1, and the framebuffer that
    /// draws into it. Null before that call and after releaseLightShafts().
    [[nodiscard]] nvrhi::ITexture* lightShaftImage(uint32_t index) const;

    [[nodiscard]] nvrhi::IFramebuffer* lightShaftFramebuffer(uint32_t index) const;

    /// The memory of those images, in bytes: nothing when there are none.
    [[nodiscard]] uint64_t lightShaftBytes() const;

    /// Makes sure there are the two volumes that what lights throw into the fog is worked
    /// out in (VolumetricFogPass), for a grid of `size` boxes: columns, rows and slices
    /// (FogVolumeGrid). Four half floats a box, each. Keeps those it has when they are of
    /// that size. Fails when the device cannot create them; the targets then hold neither.
    [[nodiscard]] Status prepareFogVolume(nvrhi::IDevice* device, UVec3 size);

    /// Lets go of those volumes: for a view whose fog no light lights any more.
    void releaseFogVolume();

    /// The first volume prepareFogVolume() made: what the lights throw towards the viewer
    /// at the middle of each box, per unit of light the fog scatters there. Null before
    /// that call and after releaseFogVolume().
    [[nodiscard]] nvrhi::ITexture* fogScattering() const { return m_fogScattering; }

    /// The second: along each column, the sum of what the fog throws towards the viewer up
    /// to the end of each box - what the lighting reads (ViewBindings, t8). Null before
    /// prepareFogVolume() and after releaseFogVolume().
    [[nodiscard]] nvrhi::ITexture* fogVolume() const { return m_fogVolume; }

    /// The memory of those volumes, in bytes: nothing when there are none.
    [[nodiscard]] uint64_t fogVolumeBytes() const;

    /// Makes sure there are the images motion blur is worked out in (MotionBlurPass): a
    /// copy of the scene color, for the last of its passes to read while it draws over it,
    /// and two images of the longest blur of each tile of the picture - a texel for every
    /// motionBlurRadius() pixels each way, rounded up - in pixels, as two half floats.
    /// Fails when the device cannot create them; the targets then hold none.
    [[nodiscard]] Status prepareMotionBlur(nvrhi::IDevice* device);

    /// Lets go of those images: for a view that has no motion blur any more.
    void releaseMotionBlur();

    /// The copy of the picture prepareMotionBlur() made. Null before that call and after
    /// releaseMotionBlur().
    [[nodiscard]] nvrhi::ITexture* motionBlurSource() const { return m_motionBlurSource; }

    /// One of the two images of tiles prepareMotionBlur() made, 0 or 1, and the framebuffer
    /// that draws into it. Null before that call and after releaseMotionBlur().
    [[nodiscard]] nvrhi::ITexture* motionBlurTiles(uint32_t index) const;

    [[nodiscard]] nvrhi::IFramebuffer* motionBlurTileFramebuffer(uint32_t index) const;

    /// The memory of those images, in bytes: nothing when there are none.
    [[nodiscard]] uint64_t motionBlurBytes() const;

    /// The G-buffer targets without the depth: what decals are drawn into, reading the
    /// depth as a texture (DecalPass).
    [[nodiscard]] nvrhi::IFramebuffer* decalFramebuffer() const { return m_decalFramebuffer; }

    /// Makes sure there is an image for the normals of the G-buffer as the base pass left
    /// them, which decals read while they draw into the normals: as large as the view, of
    /// the format of GBufferTarget::Normal. Fails when the device cannot create it; the
    /// targets then hold none.
    [[nodiscard]] Status prepareDecalNormals(nvrhi::IDevice* device);

    /// Lets go of that image: for a view that has no decals any more.
    void releaseDecalNormals();

    /// The image prepareDecalNormals() made. Null before that call and after
    /// releaseDecalNormals().
    [[nodiscard]] nvrhi::ITexture* decalNormals() const { return m_decalNormals; }

    /// The memory of that image, in bytes: nothing when there is none.
    [[nodiscard]] uint64_t decalNormalBytes() const;

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

    /// Fills scene color with `background` (linear) and depth with "far". The command list
    /// must be open, and must then draw into both: a command list keeps alive what it
    /// draws into, reads or copies, not what it only clears - targets that are replaced
    /// before it has run would be destroyed in use.
    void clear(nvrhi::ICommandList* commandList, const Vec3& background) const;

    /// Fills the G-buffer and the motion vectors with zeros: for a frame that then draws
    /// into them, as clear() says.
    void clearGBuffer(nvrhi::ICommandList* commandList) const;

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
    nvrhi::TextureHandle m_motion;
    MotionHistory m_motionHistory;
    nvrhi::TextureHandle m_ambientOcclusion;
    nvrhi::TextureHandle m_tracedOcclusion;
    /// The divisor m_tracedOcclusion was made for.
    uint32_t m_tracedOcclusionDivisor = 0;
    nvrhi::TextureHandle m_reflectionSource;
    nvrhi::TextureHandle m_reflectionHits;
    nvrhi::FramebufferHandle m_reflectionHitsFramebuffer;
    /// The divisor m_reflectionHits was made for.
    uint32_t m_reflectionDivisor = 0;
    nvrhi::TextureHandle m_display;
    nvrhi::TextureHandle m_aliased;
    nvrhi::TextureHandle m_decalNormals;
    nvrhi::TextureHandle m_depthOfFieldSource;
    nvrhi::TextureHandle m_depthOfFieldBlur;
    std::array<nvrhi::TextureHandle, 2> m_lightShafts;
    nvrhi::TextureHandle m_fogScattering;
    nvrhi::TextureHandle m_fogVolume;
    nvrhi::TextureHandle m_motionBlurSource;
    std::array<nvrhi::TextureHandle, 2> m_motionBlurTiles;
    std::array<nvrhi::FramebufferHandle, 2> m_motionBlurTileFramebuffers;
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
    nvrhi::FramebufferHandle m_decalFramebuffer;
    nvrhi::FramebufferHandle m_ambientOcclusionFramebuffer;
    nvrhi::FramebufferHandle m_tracedOcclusionFramebuffer;
    nvrhi::FramebufferHandle m_lightingFramebuffer;
    nvrhi::FramebufferHandle m_sceneFramebuffer;
    nvrhi::FramebufferHandle m_displayFramebuffer;
    nvrhi::FramebufferHandle m_aliasedFramebuffer;
    nvrhi::FramebufferHandle m_depthOfFieldSourceFramebuffer;
    nvrhi::FramebufferHandle m_depthOfFieldBlurFramebuffer;
    std::array<nvrhi::FramebufferHandle, 2> m_lightShaftFramebuffers;
    nvrhi::FramebufferHandle m_overlayFramebuffer;
};

} // namespace cordite
