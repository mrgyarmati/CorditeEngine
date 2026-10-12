#pragma once

/// @file
/// How a pass reads the G-buffer: the binding set its textures are in.
///
/// Every pass that works from the G-buffer - the lighting, the debug views, and later
/// the effects that need to know what a pixel shows - binds it the same way, as register
/// space 1 next to the view's space 0:
///
/// | Register | Holds                                                    |
/// |----------|----------------------------------------------------------|
/// | b0       | The pass's own constants, if it has any                  |
/// | t0       | G-buffer: base color                                     |
/// | t1       | G-buffer: normal                                         |
/// | t2       | G-buffer: parameters                                     |
/// | t3       | G-buffer: emissive                                       |
/// | t4       | Depth                                                    |
/// | t5       | Ambient occlusion (see SceneTargets::ambientOcclusion)   |
///
/// @code
/// nvrhi::BindingLayoutHandle layout = createGBufferLayout(device, false);
/// // ... create a pipeline with {view.layout(), layout} ...
///
/// const nvrhi::BindingSetHandle gbuffer = createGBufferBindings(device, layout, targets, nullptr);
/// const std::array<nvrhi::IBindingSet*, 2> sets = {view.bindings(), gbuffer.Get()};
/// pass.draw(commandList, targets.lightingFramebuffer(), sets);
/// @endcode
///
/// A pass that reads the depth this way must not draw into a framebuffer that has the
/// same depth attached; SceneTargets::lightingFramebuffer() has none. For the same
/// reason the passes that draw the ambient occlusion do not use this set.

#include "cordite/renderer/SceneTargets.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

/// The layout of the set. With `withConstants`, a volatile constant buffer of the
/// pass's own comes first, at b0. Null when the device cannot create it.
[[nodiscard]] nvrhi::BindingLayoutHandle createGBufferLayout(nvrhi::IDevice* device, bool withConstants);

/// The set for the textures of `targets`, in a layout made by createGBufferLayout().
/// Pass `constants` exactly when the layout was made with them. Null when the device
/// cannot create it.
///
/// Binding sets are cheap, and a command list keeps the ones it uses alive until it has
/// executed, so passes make one per draw rather than keeping one per set of targets.
[[nodiscard]] nvrhi::BindingSetHandle createGBufferBindings(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout,
                                                            const SceneTargets& targets, nvrhi::IBuffer* constants);

} // namespace cordite
