#pragma once

/// @file
/// Render targets: pictures that a camera of the scene draws into while it runs - the
/// screen of a security monitor, a mirror, the preview of a character in a menu - and
/// that materials show as their color or their light, like a texture.
///
/// @code
/// // assets/targets/monitor.target.json:
/// //   {"format": "cordite.target", "version": 1, "width": 320, "height": 180}
/// CORDITE_TRY_ASSIGN(const RenderTargetData target, parseRenderTargetFile(document));
/// // A SceneCapture component draws into it (RenderComponents.h); a material shows it
/// // by naming it as its baseColorTexture or emissiveTexture.
/// @endcode
///
/// A render target holds a finished picture, in the colors of a display: it is a color,
/// never numbers, and a material can show it only in its color slots.

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/Result.h"

#include <cstdint>

namespace cordite {

/// The largest a render target may be along either side, in pixels.
inline constexpr uint32_t kMaxRenderTargetSize = 4096;

/// What a render target file says.
struct RenderTargetData {
    /// In pixels, from 1 to kMaxRenderTargetSize.
    uint32_t width = 256;
    uint32_t height = 256;

    bool operator==(const RenderTargetData&) const = default;
};

/// The description of RenderTargetData: what a render target file may hold, and its
/// ranges.
[[nodiscard]] const TypeInfo& renderTargetTypeInfo();

/// Reads the document of a render target file. Fails for a document of another kind or
/// version, a member that is not known, and a size out of range, naming what is wrong.
[[nodiscard]] Result<RenderTargetData> parseRenderTargetFile(const Json& document);

/// The document of a render target file, as parseRenderTargetFile() reads it.
[[nodiscard]] Json serializeRenderTarget(const RenderTargetData& target);

} // namespace cordite
