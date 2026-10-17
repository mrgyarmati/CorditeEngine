#pragma once

/// @file
/// Motion blur: the passes that smear what moved along the way it went - into the light of
/// a view, after its depth of field and before the rest of its picture is finished
/// (PostProcessPass), so that bloom and exposure see the blur.
///
/// @code
/// CORDITE_TRY_ASSIGN(MotionBlurPass blur, MotionBlurPass::create(device, shaders, view.layout()));
///
/// // After the depth of field, before the picture is finished:
/// if (hasMotionBlur(look)) {
///     blur.draw(commandList, targets, view.bindings(), look, {.quality = PostProcessQuality::Medium});
/// }
/// @endcode
///
/// Three passes (shaders/passes/MotionBlur.slang, with the arithmetic of MotionBlur.h): the
/// longest blur of each tile of the picture, from the motion vectors of the view
/// (MotionVectors.h); the longest of each tile and the tiles around it; and every pixel
/// blurred from points along that, from a copy of the picture made before. The tiles and
/// the copy belong to the targets of the view (SceneTargets::prepareMotionBlur): the pass
/// keeps nothing between calls.
///
/// What is drawn forward - translucent surfaces - has no motion vectors of its own, and is
/// blurred as what is behind it moved.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

/// How the motion blur of a view is drawn, besides the settings of its scene.
struct MotionBlurFrame {
    /// How many points each pixel is blurred from
    /// (PostProcessQualitySettings::motionBlurSamples). Off is taken for the lowest.
    PostProcessQuality quality = PostProcessQuality::Medium;
};

class MotionBlurPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<MotionBlurPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                       nvrhi::IBindingLayout* viewLayout);

    /// Blurs what moved in the scene color of `targets` as much as `settings` say
    /// (PostProcessSettings::motionBlur), by its motion vectors and its depth.
    /// `viewBindings` are those of the view the targets were drawn for, with the
    /// view-projection of its picture before. The command list must be open.
    ///
    /// The tiles and the copy of the picture the passes work with are asked of the
    /// targets, which make them the first time (SceneTargets::prepareMotionBlur).
    ///
    /// Returns how many passes it took: three - or none, with the picture left as it was,
    /// when what the passes need could not be made, which the log then says once.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  const PostProcessSettings& settings, const MotionBlurFrame& frame);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::BindingLayoutHandle layout;
        FullscreenPass tiles;
        FullscreenPass neighbours;
        FullscreenPass blur;
    };

    explicit MotionBlurPass(Parts parts);

    /// The set of one of the passes: the constants, the tiles it reads, the copy of the
    /// picture, the depth and the motion vectors.
    [[nodiscard]] nvrhi::BindingSetHandle bindingsFor(nvrhi::ITexture* tiles, const SceneTargets& targets) const;

    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_constants;
    /// The constants and four textures: the same shape for all three passes.
    nvrhi::BindingLayoutHandle m_layout;
    FullscreenPass m_tiles;
    FullscreenPass m_neighbours;
    FullscreenPass m_blur;
    /// So that a failure to make what the passes need is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
