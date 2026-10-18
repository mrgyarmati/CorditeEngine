#pragma once

/// @file
/// Copying GPU textures back to the CPU, for screenshots, rendering tests and tools.
///
/// @code
/// CORDITE_TRY_ASSIGN(const TexturePixels pixels, readTexture(*graphics, colorTarget));
/// // pixels.data holds pixels.height rows of pixels.width * bytesPerPixel bytes each.
/// @endcode
///
/// Readback waits for the GPU to finish, so it stalls the frame. Do not use it in the
/// per-frame path of a game.

#include "cordite/core/Result.h"
#include "cordite/rhi/GraphicsDevice.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cordite {

/// The contents of one mip level of one slice of a texture: of an array, or of the depth of
/// a 3D texture.
struct TexturePixels {
    uint32_t width = 0;
    uint32_t height = 0;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    /// Bytes per pixel of `format`.
    uint32_t bytesPerPixel = 0;
    /// Rows from the top, tightly packed (no row padding).
    std::vector<std::byte> data;
};

/// Reads one mip level of one slice of a texture: `slice` is an index into the array of
/// an array texture, and how deep a slice of a 3D texture is at that level. The texture
/// must be in a state NVRHI tracks: created with keepInitialState, or given a permanent
/// state. Block-compressed and depth-stencil formats are not supported.
[[nodiscard]] Result<TexturePixels> readTexture(GraphicsDevice& device, nvrhi::ITexture* texture, uint32_t mipLevel = 0,
                                                uint32_t slice = 0);

} // namespace cordite
