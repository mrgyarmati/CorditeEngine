#pragma once

/// @file
/// Ambient occlusion from the screen: the passes that draw it.
///
/// @code
/// CORDITE_TRY_ASSIGN(AmbientOcclusionPass occlusion, AmbientOcclusionPass::create(device, shaders, view.layout()));
///
/// // After the base pass has filled the depth and the G-buffer of `targets`, and before
/// // anything reads targets.ambientOcclusion():
/// const AmbientOcclusionSettings settings = ambientOcclusionSettingsFor(look, ScreenEffectsQuality::Medium);
/// const uint32_t passes = occlusion.draw(commandList, targets, view.bindings(), settings);
/// @endcode
///
/// Two passes: the first works out, for every pixel or for one in four, how much of the
/// light from all around gets to what the pixel shows (shaders/passes/AmbientOcclusion.slang,
/// with the arithmetic of AmbientOcclusion.h); the second makes of that a value for
/// every pixel, in SceneTargets::ambientOcclusion(), which the lighting reads
/// (shaders/passes/AmbientOcclusionResolve.slang).
///
/// Both images belong to the targets of the view: the pass keeps nothing between calls.

#include "cordite/core/Result.h"
#include "cordite/renderer/AmbientOcclusion.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

class AmbientOcclusionPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<AmbientOcclusionPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             nvrhi::IBindingLayout* viewLayout);

    /// Works out the ambient occlusion of what `targets` hold - their depth and their
    /// normals - into their ambientOcclusion(): 1 where all the light from around gets
    /// through, less where something stands in its way. `viewBindings` are those of the
    /// view the targets were drawn for. The command list must be open.
    ///
    /// The image the first pass draws into is asked of the targets, which make it, or
    /// make it anew, when the settings need another size than they hold
    /// (SceneTargets::prepareTracedOcclusion).
    ///
    /// Returns how many passes it took: two - or none, with ambientOcclusion() left as
    /// it was, when `settings` are not enabled or what the passes need could not be
    /// made, which the log then says.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  const AmbientOcclusionSettings& settings);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::BindingLayoutHandle layout;
        FullscreenPass trace;
        FullscreenPass resolve;
    };

    explicit AmbientOcclusionPass(Parts parts);

    /// The set of one of the two passes: the constants and the two textures it reads.
    [[nodiscard]] nvrhi::BindingSetHandle bindingsFor(nvrhi::ITexture* first, nvrhi::ITexture* second) const;

    nvrhi::IDevice* m_device;
    nvrhi::BufferHandle m_constants;
    /// The constants and two textures: the same shape for both passes.
    nvrhi::BindingLayoutHandle m_layout;
    FullscreenPass m_trace;
    FullscreenPass m_resolve;
    /// So that a failure to make what the passes need is reported once, not every frame.
    bool m_reportedFailure = false;
};

} // namespace cordite
