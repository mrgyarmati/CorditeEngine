#include "cordite/renderer/TextureBudget.h"

#include "cordite/assets/Mipmaps.h"

#include <algorithm>

namespace cordite {

uint32_t firstTextureLevel(const TextureFootprint& texture, uint32_t limit, uint32_t bias) {
    const uint32_t levels = std::max(mipLevelCount(texture.width, texture.height), 1u);
    uint32_t level = 0;
    while (limit != 0 && level + 1 < levels && std::max(texture.width >> level, texture.height >> level) > limit) {
        ++level;
    }
    const uint32_t last = levels - 1;
    return bias >= last - level ? last : level + bias;
}

uint64_t textureBytesFrom(const TextureFootprint& texture, uint32_t firstLevel) {
    uint64_t bytes = 0;
    for (uint32_t level = firstLevel; level < mipLevelCount(texture.width, texture.height); ++level) {
        const UVec2 size = mipLevelSize(texture.width, texture.height, level);
        bytes += uint64_t{size.x} * size.y * texture.bytesPerTexel;
    }
    return bytes;
}

uint32_t textureBiasFor(std::span<const TextureFootprint> textures, uint32_t limit, uint64_t budget) {
    if (budget == 0) {
        return 0;
    }
    // Beyond this every texture is at its last level, and nothing gets smaller.
    uint32_t mostLevels = 1;
    for (const TextureFootprint& texture : textures) {
        mostLevels = std::max(mostLevels, mipLevelCount(texture.width, texture.height));
    }
    for (uint32_t bias = 0; bias + 1 < mostLevels; ++bias) {
        uint64_t total = 0;
        for (const TextureFootprint& texture : textures) {
            total += textureBytesFrom(texture, firstTextureLevel(texture, limit, bias));
        }
        if (total <= budget) {
            return bias;
        }
    }
    return mostLevels - 1;
}

} // namespace cordite
