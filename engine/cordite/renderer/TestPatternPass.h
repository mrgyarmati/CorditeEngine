#pragma once

/// @file
/// Draws a diagnostic pattern: a checkerboard over a red/green gradient with a bar that
/// sweeps across with time.
///
/// It is shown when there is nothing else to render, it is the image the rendering tests
/// use to verify the device, shader and readback paths end to end, and it is the
/// smallest complete example of a render pass in the engine.
///
/// @code
/// CORDITE_TRY_ASSIGN(TestPatternPass pattern, TestPatternPass::create(device, shaders));
/// pattern.draw(commandList, framebuffer, {.timeSeconds = time});
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/FullscreenPass.h"

#include <nvrhi/nvrhi.h>

namespace cordite {

struct TestPatternParameters {
    /// Moves the bar: it crosses the screen once every four seconds.
    float timeSeconds = 0.0f;
    /// Multiplies the pattern color.
    Vec3 tint{1.0f};
};

class TestPatternPass {
public:
    [[nodiscard]] static Result<TestPatternPass> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// Fills the framebuffer with the pattern. The command list must be open.
    void draw(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer,
              const TestPatternParameters& parameters = {});

private:
    TestPatternPass(nvrhi::BufferHandle constantBuffer, nvrhi::BindingSetHandle bindingSet, FullscreenPass pass);

    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::BindingSetHandle m_bindingSet;
    FullscreenPass m_pass;
};

} // namespace cordite
