#pragma once

/// @file
/// Turns shader requests into GPU shader objects.
///
/// ShaderLibrary produces SPIR-V; ShaderManager creates the NVRHI shader for it and
/// keeps it, so that every pass asking for the same permutation shares one object.
///
/// @code
/// ShaderManager shaders(graphics.device(), library);
///
/// ShaderRequest request;
/// request.path = "passes/Blit.slang";
/// request.entryPoint = "blitPixel";
/// request.stage = ShaderStage::Pixel;
/// CORDITE_TRY_ASSIGN(nvrhi::ShaderHandle pixelShader, shaders.get(request));
/// @endcode
///
/// Not thread-safe; use from the render thread.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/shaders/ShaderLibrary.h"

#include <nvrhi/nvrhi.h>

#include <span>
#include <string>
#include <unordered_map>

namespace cordite {

class ShaderManager {
public:
    /// Both the device and the library must outlive the manager.
    ShaderManager(nvrhi::IDevice* device, ShaderLibrary& library);
    ~ShaderManager() = default;
    CORDITE_NON_MOVABLE(ShaderManager);

    /// Returns the GPU shader for a request, loading or compiling it on first use.
    [[nodiscard]] Result<nvrhi::ShaderHandle> get(ShaderRequest request);

    /// Forgets the GPU shaders of the given requests, typically the result of
    /// ShaderLibrary::invalidateChanged(). Pipelines built from them must be rebuilt.
    void evict(std::span<const ShaderRequest> requests);

    /// Forgets every GPU shader.
    void clear();

    [[nodiscard]] ShaderLibrary& library() { return m_library; }

private:
    nvrhi::IDevice* m_device;
    ShaderLibrary& m_library;
    std::unordered_map<std::string, nvrhi::ShaderHandle> m_shaders;
};

} // namespace cordite
