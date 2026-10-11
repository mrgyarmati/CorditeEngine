#include "cordite/assets/Material.h"

#include "cordite/core/Json.h"

#include "AssetTestSupport.h"

#include <string>

using namespace cordite;
using cordite::test::unwrap;

namespace {

constexpr const char* kTextureA = "11111111-1111-4111-8111-111111111111";
constexpr const char* kTextureB = "22222222-2222-4222-8222-222222222222";
constexpr const char* kParent = "33333333-3333-4333-8333-333333333333";

Json parse(std::string_view text) {
    return unwrap(parseJson(text));
}

/// Parses a material document and returns the error it must produce.
Error parseFailure(std::string_view text) {
    const Result<MaterialFile> file = parseMaterialFile(parse(text));
    REQUIRE_FALSE(file.has_value());
    return file.error();
}

} // namespace

TEST_SUITE("Material") {
    TEST_CASE("a default material is plain, opaque and mid-rough") {
        const MaterialData material;
        CHECK(material.baseColor == Vec4(1.0f));
        CHECK(material.metallic == 0.0f);
        CHECK(material.roughness == 0.5f);
        CHECK(material.specular == 0.5f);
        CHECK(material.normalScale == 1.0f);
        CHECK(material.occlusionStrength == 1.0f);
        CHECK(material.emissiveColor == Vec3(0.0f));
        CHECK(material.emissiveIntensity == 1.0f);
        CHECK(material.alphaMode == AlphaMode::Opaque);
        CHECK(material.alphaCutoff == 0.5f);
        CHECK_FALSE(material.doubleSided);
        CHECK(material.uvScale == Vec2(1.0f));
        CHECK(material.wrapU == TextureWrap::Repeat);
        CHECK(material.filter == TextureFilter::Linear);
        CHECK(materialTextures(material).empty());
    }

    TEST_CASE("the names of the options are the ones used in files") {
        CHECK(nameOf(AlphaMode::Opaque) == "opaque");
        CHECK(nameOf(AlphaMode::Masked) == "masked");
        CHECK(nameOf(AlphaMode::Blended) == "blended");
        CHECK(nameOf(TextureWrap::Repeat) == "repeat");
        CHECK(nameOf(TextureWrap::MirroredRepeat) == "mirroredRepeat");
        CHECK(nameOf(TextureWrap::Clamp) == "clamp");
        CHECK(nameOf(TextureFilter::Nearest) == "nearest");
        CHECK(nameOf(TextureFilter::Linear) == "linear");
    }

    TEST_CASE("every property is described, and texture slots say what they refer to") {
        const TypeInfo& type = materialTypeInfo();
        CHECK(type.name() == "Material");
        CHECK(type.properties().size() == 23);

        size_t textureSlots = 0;
        for (const PropertyInfo& property : type.properties()) {
            CAPTURE(property.name());
            CHECK_FALSE(property.attributes().description.empty());
            if (property.type() == PropertyType::Uuid) {
                ++textureSlots;
                CHECK(property.attributes().semantic == PropertySemantic::AssetReference);
                CHECK(property.attributes().referenceType == "Texture");
                CHECK(property.name().ends_with("Texture"));
            }
        }
        CHECK(textureSlots == 5);
    }

    TEST_CASE("a serialized material starts with its header and states every property") {
        const Json document = serializeMaterial(MaterialData());
        const std::string text = dumpJson(document);
        CHECK(text.starts_with("{\n    \"format\": \"cordite.material\",\n    \"version\": 1,\n    \"baseColor\""));
        CHECK(document.size() == 2 + materialTypeInfo().properties().size());
        CHECK(document["baseColorTexture"].is_null());
        CHECK(document["alphaMode"] == "opaque");
        CHECK(document["roughness"] == 0.5);
        CHECK_FALSE(document.contains("parent"));
    }

    TEST_CASE("a material survives being written and read back") {
        MaterialData material;
        material.baseColor = Vec4(0.55f, 0.12f, 0.08f, 0.9f);
        material.baseColorTexture = *Uuid::parse(kTextureA);
        material.metallic = 0.85f;
        material.roughness = 0.35f;
        material.metallicRoughnessTexture = *Uuid::parse(kTextureB);
        material.specular = 0.3f;
        material.normalTexture = *Uuid::parse(kTextureB);
        material.normalScale = -1.5f;
        material.occlusionTexture = *Uuid::parse(kTextureB);
        material.occlusionStrength = 0.7f;
        material.emissiveColor = Vec3(1.0f, 0.4f, 0.1f);
        material.emissiveIntensity = 12.0f;
        material.emissiveTexture = *Uuid::parse(kTextureA);
        material.alphaMode = AlphaMode::Masked;
        material.alphaCutoff = 0.25f;
        material.doubleSided = true;
        material.uvChannel = 1;
        material.occlusionUvChannel = 1;
        material.uvScale = Vec2(4.0f, 2.0f);
        material.uvOffset = Vec2(0.5f, -0.25f);
        material.wrapU = TextureWrap::Clamp;
        material.wrapV = TextureWrap::MirroredRepeat;
        material.filter = TextureFilter::Nearest;

        const Json document = serializeMaterial(material);
        const MaterialFile file = unwrap(parseMaterialFile(document));
        CHECK(file.parent.isNil());

        MaterialData restored;
        file.properties.applyTo(&restored);
        CHECK(restored == material);
        // Written again, it is the same text: files do not churn.
        CHECK(dumpJson(serializeMaterial(restored)) == dumpJson(document));
    }

    TEST_CASE("a file states only what it changes; the rest comes from what it is applied to") {
        const MaterialFile file = unwrap(parseMaterialFile(parse(fmt::format(R"({{
            "format": "cordite.material",
            "version": 1,
            "parent": "{}",
            // Rusty.
            "baseColor": [0.4, 0.2, 0.1, 1],
            "roughness": 0.9
        }})",
                                                                             kParent))));
        CHECK(file.parent == *Uuid::parse(kParent));
        CHECK_FALSE(file.properties.isEmpty());

        // Stands for the parent: polished metal with a texture.
        MaterialData material;
        material.metallic = 1.0f;
        material.roughness = 0.1f;
        material.baseColorTexture = *Uuid::parse(kTextureA);

        file.properties.applyTo(&material);
        CHECK(material.baseColor == Vec4(0.4f, 0.2f, 0.1f, 1.0f));
        CHECK(material.roughness == 0.9f);
        // Not mentioned by the file, so kept.
        CHECK(material.metallic == 1.0f);
        CHECK(material.baseColorTexture == *Uuid::parse(kTextureA));
    }

    TEST_CASE("a file with only a header is a material with nothing changed") {
        const MaterialFile file =
            unwrap(parseMaterialFile(parse(R"({"format": "cordite.material", "version": 1, "parent": null})")));
        CHECK(file.parent.isNil());
        CHECK(file.properties.isEmpty());
    }

    TEST_CASE("documents that are not materials are refused with a reason") {
        SUBCASE("not an object") {
            const Error error = parseFailure("[1, 2]");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message == "a material must be a JSON object, found array");
        }
        SUBCASE("another format") {
            const Error error = parseFailure(R"({"format": "cordite.scene", "version": 1})");
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message == "not a Cordite material: 'format' must be \"cordite.material\"");
        }
        SUBCASE("no version") {
            CHECK(parseFailure(R"({"format": "cordite.material"})").message == "the material has no 'version'");
        }
        SUBCASE("a newer version") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 2, "sheen": 1})");
            CHECK(error.code == ErrorCode::VersionMismatch);
            CHECK(error.message == "the material has format version 2, but this engine reads versions 1 to 1");
        }
        SUBCASE("an unknown property") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 1, "rougness": 0.5})");
            CHECK(error.message.find("rougness") != std::string::npos);
        }
        SUBCASE("a value out of range") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 1, "roughness": 1.5})");
            CHECK(error.message.find("roughness") != std::string::npos);
            CHECK(error.message.find("1.5") != std::string::npos);
        }
        SUBCASE("a color component out of range") {
            const Error error =
                parseFailure(R"({"format": "cordite.material", "version": 1, "baseColor": [1, 1, 2, 1]})");
            CHECK(error.message.find("baseColor") != std::string::npos);
        }
        SUBCASE("an unknown option") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 1, "alphaMode": "glass"})");
            CHECK(error.message.find("expected one of [opaque, masked, blended]") != std::string::npos);
        }
        SUBCASE("a texture that is not an id") {
            const Error error =
                parseFailure(R"({"format": "cordite.material", "version": 1, "normalTexture": "bricks_n.png"})");
            CHECK(error.message.find("normalTexture") != std::string::npos);
        }
        SUBCASE("a parent that is not an id") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 1, "parent": 7})");
            CHECK(error.message.find("'parent'") != std::string::npos);
        }
        SUBCASE("a texture coordinate set that does not exist") {
            const Error error = parseFailure(R"({"format": "cordite.material", "version": 1, "uvChannel": 2})");
            CHECK(error.message.find("uvChannel") != std::string::npos);
        }
    }

    TEST_CASE("a refused document changes nothing") {
        // Parsing validates everything before anything is applied, so there is no patch
        // to apply half of.
        const Result<MaterialFile> file = parseMaterialFile(
            parse(R"({"format": "cordite.material", "version": 1, "metallic": 1, "roughness": "smooth"})"));
        CHECK_FALSE(file.has_value());
    }

    TEST_CASE("the textures of a material are listed once each, without empty slots") {
        MaterialData material;
        material.normalTexture = *Uuid::parse(kTextureB);
        material.baseColorTexture = *Uuid::parse(kTextureA);
        // One packed texture in two slots.
        material.metallicRoughnessTexture = *Uuid::parse(kTextureB);
        material.occlusionTexture = *Uuid::parse(kTextureB);

        const std::vector<Uuid> textures = materialTextures(material);
        REQUIRE(textures.size() == 2);
        CHECK(textures[0] == *Uuid::parse(kTextureA));
        CHECK(textures[1] == *Uuid::parse(kTextureB));
    }
}
