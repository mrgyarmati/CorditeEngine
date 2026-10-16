#pragma once

/// @file
/// Light shafts: the passes that add the light of the sun, scattered along rays from where
/// it is in the picture, to the light of a view - before it is blurred out of focus and
/// finished (DepthOfFieldPass, PostProcessPass).
///
/// @code
/// CORDITE_TRY_ASSIGN(LightShaftPass shafts, LightShaftPass::create(device, shaders, view.layout()));
///
/// // After everything has been drawn into targets.sceneColor():
/// if (hasLightShafts(look) && lights.sun()) {
///     shafts.draw(commandList, targets, view.bindings(), look, {.view = cameraView, .sun = *lights.sun()});
/// }
/// @endcode
///
/// Five passes (shaders/passes/LightShafts.slang, with the arithmetic of LightShafts.h):
/// what the sky gives to the shafts, in an image half as wide and as high, nothing where
/// something was drawn; three that blur it along the way to the sun, eight taps each, the
/// steps of each pass eight times those of the one before - 512 taps over the length of
/// the shafts; and one that adds the blur to the scene color. The half-sized images belong
/// to the targets of the view (SceneTargets::prepareLightShafts): the pass keeps nothing
/// between calls.

#include "cordite/core/Result.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

/// What the light shafts of a view are drawn from, besides the settings of its scene.
struct LightShaftFrame {
    /// The view the targets were drawn for: where the sun is in its picture.
    CameraView view;
    /// The sun of the scene (SceneLights::sun()).
    SceneSun sun;
    /// The shafts alone, over black, instead of over the picture
    /// (RenderDebugView::LightShafts).
    bool showShafts = false;
};

class LightShaftPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<LightShaftPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                       nvrhi::IBindingLayout* viewLayout);

    /// Adds the light shafts of `frame.sun` to the scene color of `targets`, as `settings`
    /// say. `viewBindings` are those of the view the targets were drawn for. The command
    /// list must be open.
    ///
    /// Returns how many passes it took: five - or none, with the picture left as it was,
    /// when the view has no shafts by where its sun is (lightShaftFade(), sunScreenPoint())
    /// or what the passes need could not be made, which the log then says once.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  const PostProcessSettings& settings, const LightShaftFrame& frame);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::SamplerHandle sampler;
        nvrhi::BindingLayoutHandle layout;
        FullscreenPass source;
        FullscreenPass blur;
        FullscreenPass composite;
    };

    explicit LightShaftPass(Parts parts);

    /// The set of one of the passes: the constants, what it reads, the depth and a sampler.
    [[nodiscard]] nvrhi::BindingSetHandle bindingsFor(nvrhi::ITexture* source, nvrhi::ITexture* depth) const;

    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_constants;
    nvrhi::SamplerHandle m_sampler;
    /// The constants, two textures and a sampler: the same shape for every pass.
    nvrhi::BindingLayoutHandle m_layout;
    FullscreenPass m_source;
    FullscreenPass m_blur;
    FullscreenPass m_composite;
    /// So that a failure to make what the passes need is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
