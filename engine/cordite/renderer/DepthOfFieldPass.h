#pragma once

/// @file
/// Depth of field: the passes that blur what is not at the focus distance - into the light
/// of a view, before the rest of its picture is finished (PostProcessPass), so that bloom
/// and exposure see the blur.
///
/// @code
/// CORDITE_TRY_ASSIGN(DepthOfFieldPass focus, DepthOfFieldPass::create(device, shaders, view.layout()));
///
/// // After everything has been drawn into targets.sceneColor(), before it is finished:
/// if (hasDepthOfField(look)) {
///     focus.draw(commandList, targets, view.bindings(), look, {.quality = PostProcessQuality::Medium});
/// }
/// @endcode
///
/// Three passes (shaders/passes/DepthOfField.slang, with the arithmetic of focusBlur() in
/// PostProcessSettings.h): the picture and how blurred each block of four of its pixels is,
/// into an image half as wide and as high; that blurred, each texel by those around it
/// whose own blur reaches it; and the blur laid over the picture by blending, as much as
/// each pixel is blurred, from the texels of the blur that are blurred like the pixel -
/// where nothing is, the picture stays exactly as it was. Both
/// half-sized images belong to the targets of the view (SceneTargets::prepareDepthOfField):
/// the pass keeps nothing between calls.
///
/// What is drawn forward - translucent surfaces - is not in the depth buffer, and is
/// blurred as much as what is behind it.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

/// How the depth of field of a view is drawn, besides the settings of its scene.
struct DepthOfFieldFrame {
    /// How many texels each texel of the blur is made of
    /// (PostProcessQualitySettings::depthOfFieldTaps). Off is taken for the lowest.
    PostProcessQuality quality = PostProcessQuality::Medium;
    /// Instead of the picture, where it is blurred: black where it is sharp, red in front
    /// of that and blue behind, as much as it is blurred (RenderDebugView::Focus).
    bool showFocus = false;
};

class DepthOfFieldPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<DepthOfFieldPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                         nvrhi::IBindingLayout* viewLayout);

    /// Blurs what the scene color of `targets` shows as far as `settings` say it is out of
    /// focus, by the depth of each pixel. `viewBindings` are those of the view the targets
    /// were drawn for. The command list must be open.
    ///
    /// The half-sized images the passes draw into are asked of the targets, which make
    /// them the first time (SceneTargets::prepareDepthOfField).
    ///
    /// Returns how many passes it took: three - or none, with the picture left as it was,
    /// when what the passes need could not be made, which the log then says once.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  const PostProcessSettings& settings, const DepthOfFieldFrame& frame);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::BindingLayoutHandle layout;
        FullscreenPass prefilter;
        FullscreenPass gather;
        FullscreenPass composite;
    };

    explicit DepthOfFieldPass(Parts parts);

    /// The set of one of the passes: the constants, what it reads, the depth, and the image
    /// the first pass made, which the last reads besides the blur.
    [[nodiscard]] nvrhi::BindingSetHandle bindingsFor(nvrhi::ITexture* source, nvrhi::ITexture* depth,
                                                      nvrhi::ITexture* prefiltered) const;

    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_constants;
    /// The constants and three textures: the same shape for all three passes.
    nvrhi::BindingLayoutHandle m_layout;
    FullscreenPass m_prefilter;
    FullscreenPass m_gather;
    FullscreenPass m_composite;
    /// So that a failure to make what the passes need is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
