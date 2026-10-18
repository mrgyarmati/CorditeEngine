#pragma once

/// @file
/// Reflections from the screen: the passes that draw them (their arithmetic is
/// ScreenReflections.h).
///
/// @code
/// CORDITE_TRY_ASSIGN(ScreenReflectionPass reflections, ScreenReflectionPass::create(device, shaders, view.layout()));
///
/// // After the lighting has filled the scene color of `targets`, before anything is drawn
/// // over it:
/// const ScreenReflectionSettings settings = screenReflectionSettingsFor(look, ScreenEffectsQuality::Medium);
/// const uint32_t passes = reflections.draw(commandList, targets, view.bindings(), settings, false);
/// @endcode
///
/// The scene color is copied (SceneTargets::reflectionSource()), and two passes follow:
/// the first follows the reflection of each pixel, or of one in four, through the depth
/// buffer, into SceneTargets::reflectionHits() (shaders/passes/ScreenReflectionTrace.slang);
/// the second puts the light of what each reflection met in the place of what the
/// surroundings gave the lighting to reflect, into the scene color
/// (shaders/passes/ScreenReflectionComposite.slang). Its images belong to the targets of
/// the view: the pass keeps nothing between calls.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ScreenReflections.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

class ScreenReflectionPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<ScreenReflectionPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             nvrhi::IBindingLayout* viewLayout);

    /// Puts the light of what the reflections of the surfaces of `targets` meet in their
    /// picture in the place of what the surroundings gave the lighting to reflect, in the
    /// scene color - which the lighting has filled. `viewBindings` are those of the view
    /// the targets were drawn for. With `showOnly`, the scene color shows what is put in
    /// place instead, over black (RenderDebugView::ScreenReflections). The command list
    /// must be open.
    ///
    /// The images the passes need are asked of the targets, which make them when the
    /// settings need another size than they hold (SceneTargets::prepareScreenReflections).
    ///
    /// Returns how many passes it took: two - or none, with the scene color as it was (or
    /// black, with `showOnly`), when `settings` draw none or what the passes need could not
    /// be made, which the log then says once.
    uint32_t draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                  const ScreenReflectionSettings& settings, bool showOnly);

private:
    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle constants;
        nvrhi::BindingLayoutHandle gbufferLayout;
        nvrhi::BindingLayoutHandle compositeLayout;
        FullscreenPass trace;
        FullscreenPass composite;
        FullscreenPass showOnly;
    };

    explicit ScreenReflectionPass(Parts parts);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::BufferHandle m_constants;
    /// The G-buffer with the constants of the passes (GBufferBindings.h), and the copy of
    /// the scene color with what was found (register space 2).
    nvrhi::BindingLayoutHandle m_gbufferLayout;
    nvrhi::BindingLayoutHandle m_compositeLayout;
    FullscreenPass m_trace;
    FullscreenPass m_composite;
    FullscreenPass m_showOnly;
    bool m_reportedFailure = false;
};

} // namespace cordite
