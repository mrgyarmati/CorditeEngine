#include "cordite/renderer/DebugViewPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/GBufferBindings.h"

#include <nvrhi/utils.h>

#include <array>
#include <utility>

namespace cordite {
namespace {

/// Mirrors DebugViewConstants in shaders/passes/DebugView.slang.
struct DebugViewConstants {
    /// A RenderDebugView for which isDrawnFromGBuffer() is true.
    uint32_t view;
    std::array<float, 3> padding;
};

static_assert(sizeof(DebugViewConstants) == 16, "must match the cbuffer layout in DebugView.slang");

// The shader tells the views apart by these numbers.
static_assert(static_cast<uint32_t>(RenderDebugView::BaseColor) == 1 &&
                  static_cast<uint32_t>(RenderDebugView::ShadowCascades) == 10 &&
                  static_cast<uint32_t>(RenderDebugView::AmbientOcclusion) == 11 &&
                  static_cast<uint32_t>(RenderDebugView::Fog) == 12,
              "must match the kView constants in DebugView.slang");

/// The constants are rewritten for every view that shows a debug view, every frame.
constexpr uint32_t kMaxConstantVersions = 64;

} // namespace

DebugViewPass::DebugViewPass(nvrhi::IDevice* device, nvrhi::BindingLayoutHandle gbufferLayout,
                             nvrhi::BufferHandle constants, FullscreenPass pass)
    : m_device(device),
      m_gbufferLayout(std::move(gbufferLayout)),
      m_constants(std::move(constants)),
      m_pass(std::move(pass)) {}

Result<DebugViewPass> DebugViewPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                            nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr,
                   "the debug view pass needs a device and the view layout");
    nvrhi::BindingLayoutHandle gbufferLayout = createGBufferLayout(device, true);
    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(DebugViewConstants), "DebugViewConstants", kMaxConstantVersions));
    if (!gbufferLayout || !constants) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the debug view pass");
    }

    FullscreenPassDesc desc;
    desc.debugName = "Debug view";
    desc.pixelShader = {.path = "passes/DebugView.slang", .entryPoint = "debugViewPixel", .stage = ShaderStage::Pixel};
    desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), gbufferLayout};
    CORDITE_TRY_ASSIGN(FullscreenPass pass, FullscreenPass::create(device, shaders, std::move(desc)));
    return DebugViewPass(device, std::move(gbufferLayout), std::move(constants), std::move(pass));
}

void DebugViewPass::draw(nvrhi::ICommandList* commandList, const SceneTargets& targets,
                         nvrhi::IBindingSet* viewBindings, RenderDebugView view) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "a debug view needs a command list, valid targets and the view");
    CORDITE_ASSERT(isDrawnFromGBuffer(view), "'{}' is not a view of the G-buffer", nameOf(view));
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr || !isDrawnFromGBuffer(view)) {
        return;
    }

    DebugViewConstants constants{};
    constants.view = static_cast<uint32_t>(view);
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    const nvrhi::BindingSetHandle gbuffer = createGBufferBindings(m_device, m_gbufferLayout, targets, m_constants);
    if (!gbuffer) {
        CORDITE_LOG_ERROR("Renderer", "cannot bind the G-buffer for the debug view '{}'", nameOf(view));
        return;
    }
    const std::array<nvrhi::IBindingSet*, 2> bindingSets = {viewBindings, gbuffer.Get()};
    m_pass.draw(commandList, targets.lightingFramebuffer(), bindingSets);
}

} // namespace cordite
