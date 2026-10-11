#pragma once

/// @file
/// Materials: how a surface responds to light.
///
/// A material is a set of numbers and textures in the metallic-roughness model that glTF
/// uses, so an imported model looks the way its author saw it. Materials are assets of
/// their own, stored as JSON, and a material can name a parent and state only what it
/// changes - a "material instance":
///
/// @code{.json}
/// {
///     "format": "cordite.material",
///     "version": 1,
///     "parent": "5d0b6c1e-9c3f-4f0e-8a54-0d6f3c1b2a79",   // optional
///     "baseColor": [0.55, 0.12, 0.08, 1],
///     "roughness": 0.35
/// }
/// @endcode
///
/// @code
/// CORDITE_TRY_ASSIGN(const Json document, readJsonFile(path));
/// CORDITE_TRY_ASSIGN(const MaterialFile file, parseMaterialFile(document));
/// MaterialData material;                              // or a copy of the parent, if file.parent names one
/// file.properties.applyTo(&material);                 // what the file states wins
///
/// CORDITE_TRY(writeJsonFile(path, serializeMaterial(material)));
/// @endcode
///
/// Textures are referred to by asset id. How a texture is read is decided by the slot it
/// is in, not by the texture: base color and emissive are colors (sRGB), the others are
/// data (linear). A texture can therefore not be "imported with the wrong color space".

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace cordite {

inline constexpr uint32_t kMaterialFormatVersion = 1;

/// What a material does with the alpha of its base color.
enum class AlphaMode : uint8_t {
    /// Ignores it; the surface is solid.
    Opaque,
    /// Draws the surface where alpha is at least `alphaCutoff` and nothing elsewhere.
    Masked,
    /// Lets what is behind show through, in proportion to alpha.
    Blended,
};

/// What happens to texture coordinates outside the unit square.
enum class TextureWrap : uint8_t {
    Repeat,
    MirroredRepeat,
    Clamp,
};

enum class TextureFilter : uint8_t {
    /// Hard-edged texels, for pixel art and lookup tables.
    Nearest,
    /// Smooth, with mipmaps and anisotropic filtering.
    Linear,
};

std::string_view nameOf(AlphaMode mode);
std::string_view nameOf(TextureWrap wrap);
std::string_view nameOf(TextureFilter filter);

struct MaterialData {
    /// Linear color and opacity. Multiplied with `baseColorTexture`.
    Vec4 baseColor{1.0f};
    /// Color (sRGB) with opacity in alpha.
    Uuid baseColorTexture;

    /// 0 for a non-metal, 1 for bare metal. Multiplied with the blue channel of
    /// `metallicRoughnessTexture`.
    float metallic = 0.0f;
    /// 0 is a mirror, 1 is chalk. Multiplied with the green channel of
    /// `metallicRoughnessTexture`.
    float roughness = 0.5f;
    /// Data: roughness in green, metallic in blue, as glTF packs them. Red is free for
    /// occlusion, so one texture can serve this slot and `occlusionTexture`.
    Uuid metallicRoughnessTexture;
    /// Strength of the reflection of a non-metal, 0.5 being the usual 4%.
    float specular = 0.5f;

    /// Tangent-space normals with +Y up (the OpenGL convention, which glTF uses).
    Uuid normalTexture;
    /// Scales how far the normal map bends the surface normal.
    float normalScale = 1.0f;

    /// Data: how much ambient light reaches the surface, in red.
    Uuid occlusionTexture;
    /// 0 ignores the occlusion texture, 1 applies it fully.
    float occlusionStrength = 1.0f;

    /// Linear color of the light the surface gives off, before `emissiveIntensity`.
    /// Multiplied with `emissiveTexture`.
    Vec3 emissiveColor{0.0f};
    float emissiveIntensity = 1.0f;
    /// Color (sRGB).
    Uuid emissiveTexture;

    AlphaMode alphaMode = AlphaMode::Opaque;
    /// For AlphaMode::Masked.
    float alphaCutoff = 0.5f;
    /// Draws both sides of each triangle, the back with a flipped normal.
    bool doubleSided = false;

    /// Which set of texture coordinates the textures use: 0 or 1.
    int32_t uvChannel = 0;
    /// The occlusion texture is often laid out differently and has a setting of its own.
    int32_t occlusionUvChannel = 0;
    /// Applied to the coordinates of every texture but occlusion: `uv * uvScale + uvOffset`.
    Vec2 uvScale{1.0f};
    Vec2 uvOffset{0.0f};

    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Linear;

    bool operator==(const MaterialData&) const = default;
};

/// The description of MaterialData's properties: names, ranges, and which of them refer
/// to textures.
[[nodiscard]] const TypeInfo& materialTypeInfo();

/// The content of a material file before it is combined with its parent.
struct MaterialFile {
    /// The material whose values this one starts from. Nil starts from the defaults.
    Uuid parent;
    /// The properties the file states, validated.
    PropertyPatch properties;
};

/// Validates a material document. Unknown members and properties are errors, as are
/// values outside their range.
[[nodiscard]] Result<MaterialFile> parseMaterialFile(const Json& document);

/// Writes every property of a material, as a document without a parent.
[[nodiscard]] Json serializeMaterial(const MaterialData& material);

/// The ids of the textures a material refers to, without nil ones or repetitions, in
/// the order of the properties.
[[nodiscard]] std::vector<Uuid> materialTextures(const MaterialData& material);

} // namespace cordite
