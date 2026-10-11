#include "cordite/rhi/TextureReadback.h"

#include <algorithm>
#include <cstring>

namespace cordite {

Result<TexturePixels> readTexture(GraphicsDevice& device, nvrhi::ITexture* texture, uint32_t mipLevel,
                                  uint32_t arraySlice) {
    if (texture == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "cannot read a null texture");
    }

    const nvrhi::TextureDesc& desc = texture->getDesc();
    if (mipLevel >= desc.mipLevels || arraySlice >= desc.arraySize) {
        return makeError(ErrorCode::OutOfRange, "texture '{}' has no mip {} / slice {} ({} mips, {} slices)",
                         desc.debugName, mipLevel, arraySlice, desc.mipLevels, desc.arraySize);
    }

    const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(desc.format);
    if (formatInfo.blockSize != 1 || formatInfo.hasDepth || formatInfo.hasStencil) {
        return makeError(ErrorCode::Unsupported, "cannot read back textures of format {}", formatInfo.name);
    }

    TexturePixels pixels;
    pixels.width = std::max(1u, desc.width >> mipLevel);
    pixels.height = std::max(1u, desc.height >> mipLevel);
    pixels.format = desc.format;
    pixels.bytesPerPixel = formatInfo.bytesPerBlock;

    nvrhi::IDevice* nvrhiDevice = device.device();

    nvrhi::TextureDesc stagingDesc;
    stagingDesc.width = pixels.width;
    stagingDesc.height = pixels.height;
    stagingDesc.format = desc.format;
    stagingDesc.debugName = "Readback staging";
    const nvrhi::StagingTextureHandle staging =
        nvrhiDevice->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging) {
        return makeError(ErrorCode::DeviceError, "cannot create a {}x{} staging texture", pixels.width, pixels.height);
    }

    nvrhi::TextureSlice sourceSlice;
    sourceSlice.mipLevel = mipLevel;
    sourceSlice.arraySlice = arraySlice;

    const nvrhi::CommandListHandle commandList = nvrhiDevice->createCommandList();
    commandList->open();
    commandList->copyTexture(staging, nvrhi::TextureSlice(), texture, sourceSlice);
    commandList->close();
    nvrhiDevice->executeCommandList(commandList);
    if (!nvrhiDevice->waitForIdle()) {
        return makeError(ErrorCode::DeviceError, "the device was lost while reading back '{}'", desc.debugName);
    }

    size_t rowPitch = 0;
    const void* mapped =
        nvrhiDevice->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (mapped == nullptr) {
        return makeError(ErrorCode::DeviceError, "cannot map the staging texture for '{}'", desc.debugName);
    }

    // The mapped rows may be padded; copy them into a tightly packed buffer.
    const size_t rowBytes = static_cast<size_t>(pixels.width) * pixels.bytesPerPixel;
    pixels.data.resize(rowBytes * pixels.height);
    for (uint32_t row = 0; row < pixels.height; ++row) {
        std::memcpy(pixels.data.data() + rowBytes * row, static_cast<const std::byte*>(mapped) + rowPitch * row,
                    rowBytes);
    }
    nvrhiDevice->unmapStagingTexture(staging);
    return pixels;
}

} // namespace cordite
