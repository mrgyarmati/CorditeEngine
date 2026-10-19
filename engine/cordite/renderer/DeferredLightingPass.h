#pragma once

/// @file
/// Deferred lighting: lights every pixel of the G-buffer once.
///
/// @code
/// CORDITE_TRY_ASSIGN(DeferredLightingPass lighting, DeferredLightingPass::create(device, shaders, view.layout()));
///
/// // After the base pass has filled the G-buffer of `targets`:
/// lighting.draw(commandList, targets, view.bindings());
/// @endcode
///
/// The pass writes linear light into the scene color of the targets. Pixels in which
/// nothing was drawn show the sky of the view's environment, when it has one that is
/// to be seen; otherwise they are left alone, so the background stays what it was
/// cleared to. The lights are those of the view (see ViewBindings): its directional
/// lights, the point, spot and rect lights of the cluster each pixel is in, ambient light
/// from a sky and a ground, and its environment.

#include "cordite/core/Result.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ShaderManager.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace cordite {

/// What the lighting pass makes of the surfaces in the G-buffer.
enum class DeferredLightingMode : uint8_t {
    /// The surfaces as they are, in all the light of the view.
    Lit,
    /// Every surface plain matte grey: the light without the materials.
    LightingOnly,
    /// Every surface a perfect mirror, lit by its surroundings alone - the environment
    /// and the ambient light: what there is to reflect.
    Reflections,
    /// What the lights throw towards the viewer from the fog in front of each pixel
    /// (VolumetricFog.h), alone, over black.
    FogLight,
    /// Which environment probes light each surface (RenderDebugView::Probes).
    Probes,
};

class DeferredLightingPass {
public:
    /// Loads the shaders. `viewLayout` is ViewBindings::layout().
    [[nodiscard]] static Result<DeferredLightingPass> create(nvrhi::IDevice* device, ShaderManager& shaders,
                                                             nvrhi::IBindingLayout* viewLayout);

    /// Lights the G-buffer of `targets` into their scene color, as `mode` says. The
    /// command list must be open.
    void draw(nvrhi::ICommandList* commandList, const SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
              DeferredLightingMode mode = DeferredLightingMode::Lit);

private:
    DeferredLightingPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle gbufferLayout, FullscreenPass pass,
                         FullscreenPass lightingOnlyPass, FullscreenPass reflectionsPass, FullscreenPass fogLightPass,
                         FullscreenPass probesPass);

    nvrhi::IDevice* m_device;
    nvrhi::BindingLayoutHandle m_gbufferLayout;
    FullscreenPass m_pass;
    FullscreenPass m_lightingOnlyPass;
    FullscreenPass m_reflectionsPass;
    FullscreenPass m_fogLightPass;
    FullscreenPass m_probesPass;
};

} // namespace cordite
