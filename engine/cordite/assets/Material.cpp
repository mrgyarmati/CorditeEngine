#include "cordite/assets/Material.h"

#include "cordite/core/Document.h"
#include "cordite/math/MathReflection.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

constexpr DocumentFormat kMaterialFormat{
    .name = "cordite.material", .version = kMaterialFormatVersion, .kind = "material"};

/// Members of a material file that are not properties of the material.
constexpr std::array<std::string_view, 3> kHeaderMembers = {"format", "version", "parent"};

PropertyAttributes textureSlot(std::string description) {
    return {.description = std::move(description),
            .semantic = PropertySemantic::AssetReference,
            .referenceType = "Texture"};
}

TypeInfo describeMaterial() {
    const std::vector<std::pair<TextureWrap, std::string>> wrapOptions = {
        {TextureWrap::Repeat, std::string(nameOf(TextureWrap::Repeat))},
        {TextureWrap::MirroredRepeat, std::string(nameOf(TextureWrap::MirroredRepeat))},
        {TextureWrap::Clamp, std::string(nameOf(TextureWrap::Clamp))},
    };

    return TypeBuilder<MaterialData>("Material")
        .property("baseColor", &MaterialData::baseColor,
                  {.description = "Color and opacity of the surface, multiplied with the base color texture",
                   .semantic = PropertySemantic::Color,
                   .minimum = 0.0,
                   .maximum = 1.0})
        .property("baseColorTexture", &MaterialData::baseColorTexture,
                  textureSlot("Color texture (sRGB), with opacity in its alpha channel"))
        .property("metallic", &MaterialData::metallic,
                  {.description = "0 for a non-metal, 1 for bare metal", .minimum = 0.0, .maximum = 1.0})
        .property("roughness", &MaterialData::roughness,
                  {.description = "0 is a mirror, 1 is completely matte", .minimum = 0.0, .maximum = 1.0})
        .property("metallicRoughnessTexture", &MaterialData::metallicRoughnessTexture,
                  textureSlot("Data texture: roughness in green, metallic in blue; multiplied with the two values"))
        .property("specular", &MaterialData::specular,
                  {.description = "Reflectivity of a non-metal; 0.5 is the usual 4%", .minimum = 0.0, .maximum = 1.0})
        .property("normalTexture", &MaterialData::normalTexture,
                  textureSlot("Tangent-space normal map with +Y up (OpenGL convention)"))
        .property("normalScale", &MaterialData::normalScale,
                  {.description = "Strength of the normal map", .minimum = -8.0, .maximum = 8.0})
        .property("occlusionTexture", &MaterialData::occlusionTexture,
                  textureSlot("Data texture: ambient occlusion in red"))
        .property("occlusionStrength", &MaterialData::occlusionStrength,
                  {.description = "How much of the occlusion texture is applied", .minimum = 0.0, .maximum = 1.0})
        .property("emissiveColor", &MaterialData::emissiveColor,
                  {.description = "Color of the light the surface gives off, multiplied with the emissive texture",
                   .semantic = PropertySemantic::Color,
                   .minimum = 0.0})
        .property("emissiveIntensity", &MaterialData::emissiveIntensity,
                  {.description = "Brightness of the emitted light", .minimum = 0.0})
        .property("emissiveTexture", &MaterialData::emissiveTexture,
                  textureSlot("Color texture (sRGB) of emitted light"))
        .enumProperty("alphaMode", &MaterialData::alphaMode,
                      {{AlphaMode::Opaque, std::string(nameOf(AlphaMode::Opaque))},
                       {AlphaMode::Masked, std::string(nameOf(AlphaMode::Masked))},
                       {AlphaMode::Blended, std::string(nameOf(AlphaMode::Blended))}},
                      {.description = "What the opacity does: nothing, cut holes, or let the background through"})
        .property("alphaCutoff", &MaterialData::alphaCutoff,
                  {.description = "Opacity below which a masked surface is not drawn", .minimum = 0.0, .maximum = 1.0})
        .property("doubleSided", &MaterialData::doubleSided, {.description = "Draw the back of each triangle too"})
        .property("uvChannel", &MaterialData::uvChannel,
                  {.description = "Texture coordinate set used by every texture but occlusion",
                   .minimum = 0.0,
                   .maximum = 1.0})
        .property(
            "occlusionUvChannel", &MaterialData::occlusionUvChannel,
            {.description = "Texture coordinate set used by the occlusion texture", .minimum = 0.0, .maximum = 1.0})
        .property("uvScale", &MaterialData::uvScale,
                  {.description = "Repeats of the textures across the unit square (not applied to occlusion)"})
        .property("uvOffset", &MaterialData::uvOffset,
                  {.description = "Shift of the textures, in texture coordinates (not applied to occlusion)"})
        .enumProperty("wrapU", &MaterialData::wrapU, wrapOptions,
                      {.description = "What happens beyond the left and right edges of the textures"})
        .enumProperty("wrapV", &MaterialData::wrapV, wrapOptions,
                      {.description = "What happens beyond the top and bottom edges of the textures"})
        .enumProperty("filter", &MaterialData::filter,
                      {{TextureFilter::Nearest, std::string(nameOf(TextureFilter::Nearest))},
                       {TextureFilter::Linear, std::string(nameOf(TextureFilter::Linear))}},
                      {.description = "Smooth (with mipmaps) or hard-edged texels"})
        .build();
}

} // namespace

std::string_view nameOf(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Opaque:
            return "opaque";
        case AlphaMode::Masked:
            return "masked";
        case AlphaMode::Blended:
            return "blended";
    }
    return "unknown";
}

std::string_view nameOf(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::Repeat:
            return "repeat";
        case TextureWrap::MirroredRepeat:
            return "mirroredRepeat";
        case TextureWrap::Clamp:
            return "clamp";
    }
    return "unknown";
}

std::string_view nameOf(TextureFilter filter) {
    switch (filter) {
        case TextureFilter::Nearest:
            return "nearest";
        case TextureFilter::Linear:
            return "linear";
    }
    return "unknown";
}

const TypeInfo& materialTypeInfo() {
    static const TypeInfo kType = describeMaterial();
    return kType;
}

Result<MaterialFile> parseMaterialFile(const Json& document) {
    CORDITE_TRY(readDocumentHeader(document, kMaterialFormat));

    MaterialFile file;
    if (const auto parent = document.find("parent"); parent != document.end()) {
        if (const Status status = PropertyCodec<Uuid>::fromJson(*parent, file.parent); !status) {
            return makeError(ErrorCode::ParseError, "'parent': {}", status.error().message);
        }
    }

    Json properties = Json::object();
    for (const auto& [key, value] : document.items()) {
        if (std::ranges::find(kHeaderMembers, std::string_view(key)) == kHeaderMembers.end()) {
            properties[key] = value;
        }
    }
    CORDITE_TRY_ASSIGN(file.properties, materialTypeInfo().parse(properties));
    return file;
}

Json serializeMaterial(const MaterialData& material) {
    Json document = makeDocument(kMaterialFormat);
    document.update(materialTypeInfo().toJson(&material));
    return document;
}

std::vector<Uuid> materialTextures(const MaterialData& material) {
    std::vector<Uuid> textures;
    for (const Uuid& texture : {material.baseColorTexture, material.metallicRoughnessTexture, material.normalTexture,
                                material.occlusionTexture, material.emissiveTexture}) {
        if (!texture.isNil() && std::ranges::find(textures, texture) == textures.end()) {
            textures.push_back(texture);
        }
    }
    return textures;
}

} // namespace cordite
