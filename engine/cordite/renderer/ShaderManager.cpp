#include "cordite/renderer/ShaderManager.h"

#include "cordite/core/Assert.h"

namespace cordite {
namespace {

nvrhi::ShaderType toNvrhiStage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return nvrhi::ShaderType::Vertex;
        case ShaderStage::Pixel:
            return nvrhi::ShaderType::Pixel;
        case ShaderStage::Compute:
            return nvrhi::ShaderType::Compute;
    }
    return nvrhi::ShaderType::None;
}

} // namespace

ShaderManager::ShaderManager(nvrhi::IDevice* device, ShaderLibrary& library) : m_device(device), m_library(library) {
    CORDITE_ASSERT(device != nullptr, "ShaderManager needs a device");
}

Result<nvrhi::ShaderHandle> ShaderManager::get(ShaderRequest request) {
    CORDITE_TRY(request.normalize());
    const std::string key = request.key().toHex();
    if (const auto found = m_shaders.find(key); found != m_shaders.end()) {
        return found->second;
    }

    CORDITE_TRY_ASSIGN(const std::shared_ptr<const ShaderBinary> binary, m_library.load(request));

    nvrhi::ShaderDesc desc;
    desc.shaderType = toNvrhiStage(request.stage);
    desc.debugName = request.describe();
    // Slang names the SPIR-V entry point "main" whatever the source function is called;
    // DXIL holds the one entry point, which Direct3D 12 does not ask the name of.
    desc.entryName = "main";

    nvrhi::ShaderHandle shader = m_device->createShader(desc, binary->code.data(), binary->code.size());
    if (!shader) {
        return makeError(ErrorCode::DeviceError, "the device rejected shader {}", request.describe());
    }
    m_shaders.emplace(key, shader);
    return shader;
}

void ShaderManager::evict(std::span<const ShaderRequest> requests) {
    for (ShaderRequest request : requests) {
        if (request.normalize()) {
            m_shaders.erase(request.key().toHex());
        }
    }
}

void ShaderManager::clear() {
    m_shaders.clear();
}

} // namespace cordite
