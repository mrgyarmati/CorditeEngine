#pragma once

/// @file
/// Finishes the picture of a view: from the light the renderer worked out to the colors
/// a screen shows.
///
/// @code
/// CORDITE_TRY_ASSIGN(PostProcessPass postProcess, PostProcessPass::create(device, shaders));
///
/// // After everything has been drawn into targets.sceneColor():
/// postProcess.draw(commandList, targets, settings,
///                  {.quality = PostProcessQuality::Medium, .antiAliasing = AntiAliasing::Fxaa,
///                   .deltaSeconds = frameSeconds, .timeSeconds = now});
/// showOrSave(targets.display());
/// @endcode
///
/// What is done, in the order it is done, is described in PostProcessSettings.h. The
/// passes behind it:
///
/// | Pass             | Reads                          | Writes                         |
/// |------------------|--------------------------------|--------------------------------|
/// | Bloom prefilter  | scene color                    | the first bloom level          |
/// | Bloom downsample | a bloom level                  | the next smaller one           |
/// | Exposure         | the smallest level, exposure   | the exposure of this frame     |
/// | Bloom upsample   | a bloom level                  | the next larger one, blended   |
/// | Final            | scene color, bloom, exposure, the lookup table | the display image |
/// | Anti-aliasing    | the picture of the final pass  | the display image              |
///
/// The bloom levels are only filled when something asks for them - bloom or automatic
/// exposure - and the exposure is only measured when it is automatic. With
/// anti-aliasing the final pass draws into an image of its own, which the view's targets
/// hold for as long as their picture is anti-aliased (SceneTargets::aliased()), and the
/// last pass smooths that into the display image.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordite {

/// What a picture is finished with, besides its settings.
struct PostProcessFrame {
    /// Off is for whoever draws the view to act on, by not finishing the picture at
    /// all; this pass takes it for the lowest quality.
    PostProcessQuality quality = PostProcessQuality::Medium;
    /// How the edges of the finished picture are smoothed. Left off, the final pass is
    /// the last one.
    AntiAliasing antiAliasing = AntiAliasing::Off;
    /// How long ago the view was last drawn, in seconds: automatic exposure gets used
    /// to light over time.
    float deltaSeconds = 1.0f / 60.0f;
    /// The time, in seconds: film grain changes with it.
    double timeSeconds = 0.0;
    /// The texture of PostProcessSettings::colorLut, or null for none. It must be N * N
    /// texels wide and N high, N at least 2; any other is reported and left out.
    nvrhi::ITexture* colorLut = nullptr;
};

class PostProcessPass {
public:
    /// Loads the shaders.
    [[nodiscard]] static Result<PostProcessPass> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Finishes the picture: reads the scene color of `targets`, writes their display
    /// image, and uses their bloom levels and exposure on the way. The command list
    /// must be open. `settings` are brought into their ranges first (sanitized()).
    /// Returns how many passes it took: one, those of the bloom and the exposure, and
    /// that of the anti-aliasing. A picture that cannot be anti-aliased - the device
    /// has no room for the image that takes - is finished without, and the log says so.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, const PostProcessSettings& settings,
                  const PostProcessFrame& frame);

    /// For how many sets of targets binding sets are being kept. They are made when a
    /// set of targets is first drawn and dropped when it has not been drawn for some
    /// sixty calls, which is how the memory of views that are gone comes back.
    [[nodiscard]] size_t cachedViewCount() const { return m_views.size(); }

private:
    /// The binding sets of one set of targets. They are made when first needed and
    /// kept, because a view is drawn every frame and its textures stay the same.
    struct ViewCache {
        uint64_t serial = 0;
        /// Counted in calls of draw(): what has not been used for a while is dropped.
        uint64_t lastUsed = 0;
        nvrhi::BindingSetHandle sceneColor;
        std::array<nvrhi::BindingSetHandle, kMaxBloomLevels> bloomLevels;
        /// By the smallest bloom level in use and by which exposure texture is the old one.
        std::array<std::array<nvrhi::BindingSetHandle, 2>, kMaxBloomLevels> exposure;
        /// By the largest bloom level in use and by which exposure texture is the new
        /// one; each for the lookup table it was made with.
        std::array<std::array<nvrhi::BindingSetHandle, 2>, kMaxBloomLevels> final;
        std::array<std::array<nvrhi::ITexture*, 2>, kMaxBloomLevels> finalLut{};
        /// What the anti-aliasing reads, and the image it was made for.
        nvrhi::BindingSetHandle antiAliasing;
        nvrhi::ITexture* aliased = nullptr;
    };

    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::SamplerHandle sampler;
        nvrhi::TextureHandle noLut;
        nvrhi::BindingLayoutHandle sampleLayout;
        nvrhi::BindingLayoutHandle exposureLayout;
        nvrhi::BindingLayoutHandle finalLayout;
        FullscreenPass prefilter;
        FullscreenPass downsample;
        FullscreenPass upsample;
        FullscreenPass exposure;
        FullscreenPass final;
        FullscreenPass antiAliasing;
    };

    explicit PostProcessPass(Parts parts);

    [[nodiscard]] ViewCache& cacheFor(const SceneTargets& targets);
    [[nodiscard]] nvrhi::IBindingSet* sampleBindings(nvrhi::BindingSetHandle& set, nvrhi::ITexture* texture);
    /// What the anti-aliasing of `targets` is drawn with, having made sure that they
    /// hold the image it reads; null, with what is in the way reported once, when
    /// that cannot be had.
    [[nodiscard]] nvrhi::IBindingSet* antiAliasingBindings(ViewCache& view, SceneTargets& targets);

    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_constants;
    nvrhi::SamplerHandle m_sampler;
    /// A black texel, bound where the settings ask for no lookup table.
    nvrhi::TextureHandle m_noLut;
    nvrhi::BindingLayoutHandle m_sampleLayout;
    nvrhi::BindingLayoutHandle m_exposureLayout;
    nvrhi::BindingLayoutHandle m_finalLayout;
    FullscreenPass m_prefilter;
    FullscreenPass m_downsample;
    FullscreenPass m_upsample;
    FullscreenPass m_exposure;
    FullscreenPass m_final;
    FullscreenPass m_antiAliasing;
    std::vector<ViewCache> m_views;
    uint64_t m_drawCount = 0;
    /// So that a lookup table of the wrong shape is reported once, not every frame,
    /// and a picture that cannot be anti-aliased likewise.
    bool m_reportedLut = false;
    bool m_reportedAntiAliasing = false;
};

} // namespace cordite
