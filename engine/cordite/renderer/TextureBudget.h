#pragma once

/// @file
/// What textures cost on the GPU, and how that is kept predictable.
///
/// Two things bound the memory of textures. The limit is the largest side a texture keeps:
/// one that is larger is uploaded without its largest levels - the quality of the program
/// says how large (textureLimitFor() in SceneRenderer.h). The budget is the most a
/// project's textures may take together: when the textures known so far would take more,
/// every one of them drops the same number of levels more - the bias, the smallest at which
/// they fit (textureBiasFor()). The bias follows from which textures there are, never from
/// the order they came in: two players at the same place see the same sharpness, whatever
/// they looked at first.
///
/// @code
/// const std::vector<TextureFootprint> known = {{.width = 4096, .height = 4096, .bytesPerTexel = 4},
///                                              {.width = 1024, .height = 512, .bytesPerTexel = 8}};
/// const uint32_t bias = textureBiasFor(known, 2048, uint64_t{32} << 20);
/// const uint32_t first = firstTextureLevel(known[0], 2048, bias);   // a level of 2048 or less
/// const uint64_t bytes = textureBytesFrom(known[0], first);         // that level and the rest
/// @endcode

#include <cstdint>
#include <span>

namespace cordite {

/// What a texture takes on the GPU: its size, and how many bytes a texel of it is there.
struct TextureFootprint {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t bytesPerTexel = 4;

    bool operator==(const TextureFootprint&) const = default;
};

/// The level of `texture` the GPU keeps first: the first whose larger side is at most
/// `limit` (0 for none), and `bias` levels further - never past its last level, a texel
/// across.
[[nodiscard]] uint32_t firstTextureLevel(const TextureFootprint& texture, uint32_t limit, uint32_t bias);

/// The bytes `texture` takes on the GPU from level `firstLevel` on, all its smaller levels
/// with it. Nothing from beyond its last level.
[[nodiscard]] uint64_t textureBytesFrom(const TextureFootprint& texture, uint32_t firstLevel);

/// The smallest bias at which all of `textures`, under `limit`, take no more than `budget`
/// bytes together. 0 when there is no budget (0) or they fit as they are; when they do not
/// fit even at their smallest, the bias that leaves every one of them at its last level.
[[nodiscard]] uint32_t textureBiasFor(std::span<const TextureFootprint> textures, uint32_t limit, uint64_t budget);

} // namespace cordite
