#include "cordite/assets/GltfImporter.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/StringUtils.h"

#include "GltfTestSupport.h"

#include <cmath>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkTangentFrames;
using cordite::test::checkTangentsFollowUvs;
using cordite::test::GltfFile;
using cordite::test::kGltfFloat;
using cordite::test::kGltfUnsignedByte;
using cordite::test::kGltfUnsignedInt;
using cordite::test::kGltfUnsignedShort;
using cordite::test::parseOrFail;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A triangle in the XY plane that is counter-clockwise, and so faces +Z.
const std::vector<float> kTrianglePositions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

/// The corners of the unit square in the XY plane: bottom-left, bottom-right, top-right, top-left.
const std::vector<float> kSquarePositions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};

/// The image mapped onto that square the usual way.
const std::vector<float> kSquareUvs = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};

/// The normal every triangle of a mesh must have, checked on its geometry as well as on
/// the vertex normals.
void checkFacing(const MeshData& mesh, const Vec3& expected) {
    REQUIRE(mesh.indices.size() >= 3);
    for (size_t first = 0; first < mesh.indices.size(); first += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[first]];
        const MeshVertex& b = mesh.vertices[mesh.indices[first + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[first + 2]];
        CHECK_NEAR(glm::normalize(glm::cross(b.position - a.position, c.position - a.position)), expected, 1.0e-5f);
        CHECK_NEAR(a.normal, expected, 1.0e-5f);
    }
}

/// Imports a file that must be refused and returns the error.
Error importFailure(const std::filesystem::path& path, const GltfImportOptions& options = {}) {
    const Result<GltfDocument> document = importGltf(path, options);
    REQUIRE_FALSE(document.has_value());
    // Every message says which file it is about.
    CHECK(document.error().message.find(pathToUtf8(path)) != std::string::npos);
    return document.error();
}

bool hasWarning(const GltfDocument& document, std::string_view fragment) {
    return std::ranges::any_of(document.warnings,
                               [&](const std::string& warning) { return warning.find(fragment) != std::string::npos; });
}

/// A file with one material described by `material`, and optionally textures.
GltfDocument importMaterial(const TempDirectory& directory, std::string_view material, std::string_view rest = "") {
    GltfFile file;
    file.json["materials"] = Json::array({parseOrFail(material)});
    if (!rest.empty()) {
        file.json.update(parseOrFail(rest));
    }
    return unwrap(importGltf(file.write(directory.path(), "material")));
}

/// Two images and two textures that use them, with different samplers.
constexpr std::string_view kTwoTextures = R"({
    "images": [{"uri": "a.png"}, {"uri": "b.png"}],
    "samplers": [{"magFilter": 9728, "wrapS": 33071, "wrapT": 33648}, {}],
    "textures": [{"source": 0, "sampler": 0}, {"source": 1, "sampler": 1}, {"source": 1, "sampler": 0}]
})";

} // namespace

TEST_SUITE("GltfImporter") {
    TEST_CASE("a bare triangle becomes a complete mesh") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");
        file.json["nodes"] = parseOrFail(R"([{"mesh": 0}])");
        file.json["scenes"] = parseOrFail(R"([{"nodes": [0]}])");
        file.json["scene"] = 0;

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "triangle")));
        CHECK(document.warnings.empty());
        REQUIRE(document.meshes.size() == 1);

        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.name == "Mesh 0");
        CHECK(validateMesh(mesh).has_value());
        CHECK(mesh.vertices.size() == 3);
        CHECK(mesh.triangleCount() == 1);
        REQUIRE(mesh.submeshes.size() == 1);
        CHECK(mesh.bounds.min == Vec3(0.0f));
        CHECK(mesh.bounds.max == Vec3(1.0f, 1.0f, 0.0f));
        // No normals in the file: flat ones, as the specification says.
        checkFacing(mesh, Vec3(0.0f, 0.0f, 1.0f));
        checkTangentFrames(mesh);

        // The part has no material.
        REQUIRE(document.meshes[0].materials.size() == 1);
        CHECK_FALSE(document.meshes[0].materials[0].has_value());

        REQUIRE(document.nodes.size() == 1);
        // A node without a name is called after its mesh.
        CHECK(document.nodes[0].name == "Mesh 0");
        CHECK(document.nodes[0].mesh == 0u);
        CHECK(document.nodes[0].transform == Transform());
        CHECK(document.roots == std::vector<uint32_t>{0});
    }

    TEST_CASE("attributes given by the file are kept as they are") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
        // Deliberately not what MikkTSpace would compute, to show it is not recomputed.
        file.addAccessor<float>("VEC4", kGltfFloat, {0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1});
        file.addAccessor<float>("VEC2", kGltfFloat, kSquareUvs);
        file.addAccessor<float>("VEC2", kGltfFloat, {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f});
        file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {0, 1, 2, 0, 2, 3});
        file.json["meshes"] = parseOrFail(R"([{"name": "Square", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2, "TEXCOORD_0": 3, "TEXCOORD_1": 4},
            "indices": 5
        }]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "square")));
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.name == "Square");
        REQUIRE(mesh.vertices.size() == 4);
        CHECK(mesh.triangleCount() == 2);
        checkFacing(mesh, Vec3(0.0f, 0.0f, 1.0f));

        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.tangent, Vec4(0.0f, 1.0f, 0.0f, -1.0f), 1.0e-6f);
            // Texture coordinates travel with their vertex, whatever order vertices end up in.
            CHECK(vertex.uv0 == Vec2(vertex.position.x, 1.0f - vertex.position.y));
            const float corner = vertex.position.x + 2.0f * vertex.position.y;
            const Vec2 expectedUv1 = corner == 0.0f   ? Vec2(0.1f, 0.2f)
                                     : corner == 1.0f ? Vec2(0.3f, 0.4f)
                                     : corner == 3.0f ? Vec2(0.5f, 0.6f)
                                                      : Vec2(0.7f, 0.8f);
            CHECK(vertex.uv1 == expectedUv1);
        }
    }

    TEST_CASE("missing tangents are computed from the texture coordinates") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
        file.addAccessor<float>("VEC2", kGltfFloat, kSquareUvs);
        file.addAccessor<uint8_t>("SCALAR", kGltfUnsignedByte, {0, 1, 2, 0, 2, 3});
        file.json["meshes"] = parseOrFail(
            R"([{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "square")));
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.vertices.size() == 4);
        checkTangentFrames(mesh);
        checkTangentsFollowUvs(mesh);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.tangent, Vec4(1.0f, 0.0f, 0.0f, 1.0f), 1.0e-5f);
        }
    }

    TEST_CASE("tangents in a file without normals are ignored, as the specification requires") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        file.addAccessor<float>("VEC4", kGltfFloat, {0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1});
        file.addAccessor<float>("VEC2", kGltfFloat, kSquareUvs);
        file.addAccessor<uint32_t>("SCALAR", kGltfUnsignedInt, {0, 1, 2, 0, 2, 3});
        file.json["meshes"] = parseOrFail(
            R"([{"primitives": [{"attributes": {"POSITION": 0, "TANGENT": 1, "TEXCOORD_0": 2}, "indices": 3}]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "square")));
        const MeshData& mesh = document.meshes[0].mesh;
        checkFacing(mesh, Vec3(0.0f, 0.0f, 1.0f));
        checkTangentsFollowUvs(mesh);
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.tangent, Vec4(1.0f, 0.0f, 0.0f, 1.0f), 1.0e-5f);
        }
    }

    TEST_CASE("strips and fans are turned into triangles that face the same way") {
        const TempDirectory directory;
        for (const int mode : {5, 6}) {
            CAPTURE(mode);
            GltfFile file;
            if (mode == 5) {
                // A strip zigzags: bottom-left, bottom-right, top-left, top-right.
                file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0});
            } else {
                // A fan goes round its first vertex.
                file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, -1, 1, 0});
            }
            file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");
            file.json["meshes"][0]["primitives"][0]["mode"] = mode;

            const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "strip")));
            const MeshData& mesh = document.meshes[0].mesh;
            CHECK(mesh.triangleCount() == 3);
            checkFacing(mesh, Vec3(0.0f, 0.0f, 1.0f));
        }
    }

    TEST_CASE("an indexed strip uses its indices") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        // Bottom-left, bottom-right, top-left, top-right.
        file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {0, 1, 3, 2});
        file.json["meshes"] =
            parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "mode": 5}]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "strip")));
        CHECK(document.meshes[0].mesh.triangleCount() == 2);
        checkFacing(document.meshes[0].mesh, Vec3(0.0f, 0.0f, 1.0f));
    }

    TEST_CASE("integer attributes are converted, normalized or not, as the accessor says") {
        const TempDirectory directory;
        GltfFile file;
        // Positions as plain 16-bit integers (KHR_mesh_quantization): metres, unscaled.
        file.addAccessor<int16_t>("VEC3", 5122, {0, 0, 0, 2, 0, 0, 0, 3, 0});
        // Texture coordinates as normalized bytes and shorts.
        file.addAccessor<uint8_t>("VEC2", kGltfUnsignedByte, {0, 255, 255, 255, 0, 0}, true);
        file.addAccessor<uint16_t>("VEC2", kGltfUnsignedShort, {0, 65535, 65535, 65535, 0, 0}, true);
        // Normals as normalized signed bytes.
        file.addAccessor<int8_t>("VEC3", 5120, {0, 0, 127, 0, 0, 127, 0, 0, 127}, true);
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{
            "attributes": {"POSITION": 0, "TEXCOORD_0": 1, "TEXCOORD_1": 2, "NORMAL": 3}
        }]}])");
        file.json["extensionsUsed"] = parseOrFail(R"(["KHR_mesh_quantization"])");
        file.json["extensionsRequired"] = parseOrFail(R"(["KHR_mesh_quantization"])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "quantized")));
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.bounds.max == Vec3(2.0f, 3.0f, 0.0f));
        for (const MeshVertex& vertex : mesh.vertices) {
            CHECK_NEAR(vertex.normal, Vec3(0.0f, 0.0f, 1.0f), 1.0e-6f);
            CHECK(vertex.uv0 == vertex.uv1);
            CHECK(vertex.uv0.x == (vertex.position.x > 0.0f ? 1.0f : 0.0f));
            CHECK(vertex.uv0.y == (vertex.position.y > 0.0f ? 0.0f : 1.0f));
        }
    }

    TEST_CASE("a sparse accessor replaces the elements it lists") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        // Moves the third vertex from (0, 1, 0) to (0, 5, 0).
        file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {2});
        file.addAccessor<float>("VEC3", kGltfFloat, {0.0f, 5.0f, 0.0f});
        file.accessors()[0]["sparse"] = parseOrFail(R"({
            "count": 1,
            "indices": {"bufferView": 1, "componentType": 5123},
            "values": {"bufferView": 2}
        })");
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "sparse")));
        CHECK(document.meshes[0].mesh.bounds.max == Vec3(1.0f, 5.0f, 0.0f));
    }

    TEST_CASE("parts with the same material become one submesh; each material gets its own") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.addAccessor<float>("VEC3", kGltfFloat, {5, 0, 0, 6, 0, 0, 5, 1, 0});
        file.addAccessor<float>("VEC3", kGltfFloat, {9, 0, 0, 10, 0, 0, 9, 1, 0});
        file.addAccessor<float>("VEC3", kGltfFloat, {20, 0, 0, 21, 0, 0, 20, 1, 0});
        file.json["materials"] = parseOrFail(R"([{"name": "Red"}, {"name": "Blue"}])");
        // Blue, none, blue again, red: three slots in the order of first use.
        file.json["meshes"] = parseOrFail(R"([{"primitives": [
            {"attributes": {"POSITION": 0}, "material": 1},
            {"attributes": {"POSITION": 1}},
            {"attributes": {"POSITION": 2}, "material": 1},
            {"attributes": {"POSITION": 3}, "material": 0}
        ]}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "parts")));
        const GltfMesh& mesh = document.meshes[0];
        REQUIRE(mesh.mesh.submeshes.size() == 3);
        REQUIRE(mesh.materials.size() == 3);
        CHECK(mesh.materials[0] == 1u);
        CHECK_FALSE(mesh.materials[1].has_value());
        CHECK(mesh.materials[2] == 0u);

        // The blue submesh holds both blue parts.
        CHECK(mesh.mesh.submeshes[0].indexCount == 6);
        CHECK(mesh.mesh.submeshes[0].bounds.min.x == 0.0f);
        CHECK(mesh.mesh.submeshes[0].bounds.max.x == 10.0f);
        CHECK(mesh.mesh.submeshes[1].indexCount == 3);
        CHECK(mesh.mesh.submeshes[1].bounds.min.x == 5.0f);
        CHECK(mesh.mesh.submeshes[2].bounds.min.x == 20.0f);
        CHECK(validateMesh(mesh.mesh).has_value());
    }

    TEST_CASE("what cannot be drawn is left out, with a warning, and the rest is imported") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.addAccessor<float>("VEC4", kGltfFloat, {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1});
        file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 1, 0, 0, 1, 0, 0, 1});
        file.json["meshes"] = parseOrFail(R"([
            {"name": "Mixed", "primitives": [
                {"attributes": {"POSITION": 0}, "mode": 1, "targets": [{"POSITION": 0}]},
                {"attributes": {"POSITION": 0, "COLOR_0": 1}, "targets": [{"POSITION": 0}]},
                {"attributes": {"NORMAL": 2}, "targets": [{"POSITION": 0}]}
            ]},
            {"name": "Dots", "primitives": [{"attributes": {"POSITION": 0}, "mode": 0}]}
        ])");
        file.json["nodes"] = parseOrFail(R"([{"name": "A", "mesh": 0}, {"name": "B", "mesh": 1}])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "mixed")));
        CHECK(hasWarning(document, "mesh 'Mixed' contains points or lines, which are not imported"));
        CHECK(hasWarning(document, "mesh 'Mixed' has vertex colors, which are not imported"));
        CHECK(hasWarning(document, "mesh 'Mixed' has morph targets, which are not imported"));
        CHECK(hasWarning(document, "mesh 'Mixed' has a part without positions, which is skipped"));
        CHECK(hasWarning(document, "mesh 'Dots' contains points or lines, which are not imported"));

        // The entries stay where they are in the file, so that indices keep their meaning.
        REQUIRE(document.meshes.size() == 2);
        CHECK(document.meshes[0].mesh.submeshes.size() == 1);
        CHECK(document.meshes[0].mesh.triangleCount() == 1);
        CHECK(document.meshes[1].mesh.submeshes.empty());
        CHECK(document.meshes[1].mesh.name == "Dots");

        // A node whose mesh is empty has no mesh.
        CHECK(document.nodes[0].mesh == 0u);
        CHECK_FALSE(document.nodes[1].mesh.has_value());
    }

    TEST_CASE("a warning is given once, however often its cause occurs") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["meshes"] = parseOrFail(R"([{"name": "Lines", "primitives": [
            {"attributes": {"POSITION": 0}, "mode": 1},
            {"attributes": {"POSITION": 0}, "mode": 2},
            {"attributes": {"POSITION": 0}, "mode": 3}
        ]}])");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "lines")));
        CHECK(document.warnings.size() == 1);
    }

    TEST_CASE("the defaults of a glTF material are those of the specification") {
        const TempDirectory directory;
        const GltfDocument document = importMaterial(directory, "{}");
        REQUIRE(document.materials.size() == 1);
        const GltfMaterial& material = document.materials[0];
        CHECK(material.name == "Material 0");
        CHECK(material.material.baseColor == Vec4(1.0f));
        // Unlike the engine's own default, a glTF material is metallic and rough.
        CHECK(material.material.metallic == 1.0f);
        CHECK(material.material.roughness == 1.0f);
        CHECK(material.material.specular == 0.5f);
        // What only matters with a texture keeps the engine's default without one.
        CHECK(material.material.normalScale == 1.0f);
        CHECK(material.material.occlusionStrength == 1.0f);
        CHECK(material.material.alphaMode == AlphaMode::Opaque);
        CHECK(material.material.alphaCutoff == 0.5f);
        CHECK_FALSE(material.material.doubleSided);
        CHECK(material.material.emissiveColor == Vec3(0.0f));
        CHECK(material.material.emissiveIntensity == 1.0f);
        CHECK(material.images == GltfMaterialImages());
        CHECK(document.warnings.empty());
    }

    TEST_CASE("every value of a glTF material is carried over") {
        const TempDirectory directory;
        const GltfDocument document = importMaterial(directory, R"({
            "name": "Hazard paint",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.5, 0.25, 0.125, 0.75],
                "metallicFactor": 0.25,
                "roughnessFactor": 0.75,
                "baseColorTexture": {"index": 0},
                "metallicRoughnessTexture": {"index": 1}
            },
            "normalTexture": {"index": 1, "scale": 2},
            "occlusionTexture": {"index": 1, "strength": 0.5},
            "emissiveTexture": {"index": 0},
            "emissiveFactor": [1, 0.5, 0.25],
            "alphaMode": "MASK",
            "alphaCutoff": 0.25,
            "doubleSided": true,
            "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 8}}
        })",
                                                     R"({
            "images": [{"uri": "color.png"}, {"uri": "data.png"}],
            "textures": [{"source": 0}, {"source": 1}]
        })");

        const GltfMaterial& material = document.materials[0];
        const MaterialData& data = material.material;
        CHECK(material.name == "Hazard paint");
        CHECK(data.baseColor == Vec4(0.5f, 0.25f, 0.125f, 0.75f));
        CHECK(data.metallic == 0.25f);
        CHECK(data.roughness == 0.75f);
        CHECK(data.normalScale == 2.0f);
        CHECK(data.occlusionStrength == 0.5f);
        CHECK(data.emissiveColor == Vec3(1.0f, 0.5f, 0.25f));
        CHECK(data.emissiveIntensity == 8.0f);
        CHECK(data.alphaMode == AlphaMode::Masked);
        CHECK(data.alphaCutoff == 0.25f);
        CHECK(data.doubleSided);

        CHECK(material.images.baseColor == 0u);
        CHECK(material.images.metallicRoughness == 1u);
        CHECK(material.images.normal == 1u);
        CHECK(material.images.occlusion == 1u);
        CHECK(material.images.emissive == 0u);
        // The ids are not the importer's to know.
        CHECK(materialTextures(data).empty());
        CHECK(document.warnings.empty());
    }

    TEST_CASE("blended materials and values outside their range") {
        const TempDirectory directory;
        const GltfDocument document = importMaterial(directory, R"({
            "pbrMetallicRoughness": {"baseColorFactor": [2, -1, 0.5, 9], "metallicFactor": 7, "roughnessFactor": -3},
            "emissiveFactor": [-1, 3, 0],
            "alphaMode": "BLEND"
        })");
        const MaterialData& data = document.materials[0].material;
        CHECK(data.alphaMode == AlphaMode::Blended);
        CHECK(data.baseColor == Vec4(1.0f, 0.0f, 0.5f, 1.0f));
        CHECK(data.metallic == 1.0f);
        CHECK(data.roughness == 0.0f);
        // Emission may be brighter than one, but not negative.
        CHECK(data.emissiveColor == Vec3(0.0f, 3.0f, 0.0f));
    }

    TEST_CASE("reflectivity comes from the index of refraction and the specular factor") {
        const TempDirectory directory;
        SUBCASE("the default index of refraction is the engine's default") {
            const GltfDocument document =
                importMaterial(directory, R"({"extensions": {"KHR_materials_ior": {"ior": 1.5}}})");
            CHECK(document.materials[0].material.specular == doctest::Approx(0.5f));
        }
        SUBCASE("no refraction means no reflection") {
            const GltfDocument document =
                importMaterial(directory, R"({"extensions": {"KHR_materials_ior": {"ior": 1.0}}})");
            CHECK(document.materials[0].material.specular == 0.0f);
        }
        SUBCASE("the specular factor scales it") {
            const GltfDocument document =
                importMaterial(directory, R"({"extensions": {"KHR_materials_specular": {"specularFactor": 0.5}}})");
            CHECK(document.materials[0].material.specular == doctest::Approx(0.25f));
        }
        SUBCASE("and it stops at what the engine can express") {
            const GltfDocument document =
                importMaterial(directory, R"({"extensions": {"KHR_materials_ior": {"ior": 2.5}}})");
            CHECK(document.materials[0].material.specular == 1.0f);
        }
    }

    TEST_CASE("the sampler and the layout of the first texture apply to the material") {
        const TempDirectory directory;
        const GltfDocument document = importMaterial(directory, R"({
            "pbrMetallicRoughness": {
                "baseColorTexture": {
                    "index": 0,
                    "texCoord": 0,
                    "extensions": {"KHR_texture_transform": {"scale": [4, 2], "offset": [0.5, 0.25], "texCoord": 1}}
                }
            },
            "occlusionTexture": {"index": 2, "texCoord": 0}
        })",
                                                     kTwoTextures);

        const MaterialData& data = document.materials[0].material;
        CHECK(data.wrapU == TextureWrap::Clamp);
        CHECK(data.wrapV == TextureWrap::MirroredRepeat);
        CHECK(data.filter == TextureFilter::Nearest);
        CHECK(data.uvScale == Vec2(4.0f, 2.0f));
        CHECK(data.uvOffset == Vec2(0.5f, 0.25f));
        // The extension's own coordinate set wins over the texture's.
        CHECK(data.uvChannel == 1);
        // The occlusion texture has the other set to itself, and no layout of its own.
        CHECK(data.occlusionUvChannel == 0);
        CHECK(document.materials[0].images.baseColor == 0u);
        CHECK(document.materials[0].images.occlusion == 1u);
        CHECK(document.warnings.empty());
    }

    TEST_CASE("a number too large to be one does not get into a material") {
        const TempDirectory directory;
        GltfFile file;
        file.json.update(parseOrFail(kTwoTextures));
        file.json["materials"] = parseOrFail(R"([{"pbrMetallicRoughness": {"baseColorTexture": {
            "index": 0,
            "extensions": {"KHR_texture_transform": {"scale": [12345.5, 2], "offset": [0.25, 12345.5]}}
        }}}])");
        const std::filesystem::path path = file.write(directory.path(), "huge");

        // JSON cannot say "infinity", but it can say 1e999, which reads as it.
        const std::string text = replaceAll(unwrap(readTextFile(path)), "12345.5", "1e999");
        REQUIRE(text.find("1e999") != std::string::npos);
        REQUIRE(writeTextFile(path, text).has_value());

        const GltfDocument document = unwrap(importGltf(path));
        CHECK(document.materials[0].material.uvScale == Vec2(1.0f, 2.0f));
        CHECK(document.materials[0].material.uvOffset == Vec2(0.25f, 0.0f));
    }

    TEST_CASE("an occlusion texture shares the layout of the textures whose coordinates it uses") {
        const TempDirectory directory;
        constexpr std::string_view kScaled = R"("extensions": {"KHR_texture_transform": {"scale": [4, 2]}})";

        SUBCASE("packed with the others and laid out like them") {
            const GltfDocument document = importMaterial(
                directory,
                fmt::format(R"({{"pbrMetallicRoughness": {{"metallicRoughnessTexture": {{"index": 0, {0}}}}},
                                "occlusionTexture": {{"index": 0, {0}}}}})",
                            kScaled),
                kTwoTextures);
            CHECK(document.warnings.empty());
            CHECK(document.materials[0].material.uvScale == Vec2(4.0f, 2.0f));
            CHECK(document.materials[0].material.uvChannel == 0);
            CHECK(document.materials[0].material.occlusionUvChannel == 0);
        }
        SUBCASE("the only texture there is") {
            const GltfDocument document = importMaterial(
                directory, fmt::format(R"({{"occlusionTexture": {{"index": 0, "texCoord": 1, {}}}}})", kScaled),
                kTwoTextures);
            CHECK(document.warnings.empty());
            // Its layout is the material's, so that it applies.
            CHECK(document.materials[0].material.uvScale == Vec2(4.0f, 2.0f));
            CHECK(document.materials[0].material.uvChannel == 1);
            CHECK(document.materials[0].material.occlusionUvChannel == 1);
        }
        SUBCASE("in a set of coordinates of its own, as it is") {
            const GltfDocument document =
                importMaterial(directory,
                               fmt::format(R"({{"pbrMetallicRoughness": {{"baseColorTexture": {{"index": 0, {}}}}},
                                "occlusionTexture": {{"index": 0, "texCoord": 1}}}})",
                                           kScaled),
                               kTwoTextures);
            CHECK(document.warnings.empty());
            CHECK(document.materials[0].material.uvScale == Vec2(4.0f, 2.0f));
            CHECK(document.materials[0].material.uvChannel == 0);
            CHECK(document.materials[0].material.occlusionUvChannel == 1);
        }
    }

    TEST_CASE("what the material model cannot express is reported") {
        const TempDirectory directory;
        SUBCASE("textures sampled in different ways") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                "normalTexture": {"index": 1}
            })",
                                                         kTwoTextures);
            CHECK(hasWarning(document, "wraps or filters its textures differently"));
            // The first texture's sampler is the one that is used.
            CHECK(document.materials[0].material.wrapU == TextureWrap::Clamp);
        }
        SUBCASE("textures laid out differently") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                "normalTexture": {"index": 2, "texCoord": 1}
            })",
                                                         kTwoTextures);
            CHECK(hasWarning(document, "lays its textures out differently"));
            CHECK(document.materials[0].material.uvChannel == 0);
        }
        SUBCASE("a rotated texture") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {
                    "index": 0, "extensions": {"KHR_texture_transform": {"rotation": 1.5}}
                }}
            })",
                                                         kTwoTextures);
            CHECK(hasWarning(document, "rotates a texture"));
        }
        SUBCASE("an occlusion texture laid out differently from the others it shares coordinates with") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                "occlusionTexture": {"index": 0, "extensions": {"KHR_texture_transform": {"scale": [2, 2]}}}
            })",
                                                         kTwoTextures);
            CHECK(hasWarning(document, "lays its textures out differently"));
            CHECK(document.materials[0].material.uvScale == Vec2(1.0f));
        }
        SUBCASE("a scaled occlusion texture in a set of coordinates of its own") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                "occlusionTexture": {"index": 0, "texCoord": 1,
                                     "extensions": {"KHR_texture_transform": {"scale": [2, 2]}}}
            })",
                                                         kTwoTextures);
            CHECK(
                hasWarning(document, "scales or shifts an occlusion texture that has texture coordinates of its own"));
            CHECK(document.materials[0].material.occlusionUvChannel == 1);
        }
        SUBCASE("a third set of texture coordinates") {
            const GltfDocument document = importMaterial(
                directory, R"({"pbrMetallicRoughness": {"baseColorTexture": {"index": 0, "texCoord": 2}}})",
                kTwoTextures);
            CHECK(hasWarning(document, "uses texture coordinate set 2"));
            CHECK(document.materials[0].material.uvChannel == 0);
        }
        SUBCASE("an unlit material") {
            const GltfDocument document = importMaterial(directory, R"({"extensions": {"KHR_materials_unlit": {}}})");
            CHECK(hasWarning(document, "is unlit"));
        }
        SUBCASE("the specular-glossiness model") {
            const GltfDocument document = importMaterial(directory, R"({
                "extensions": {"KHR_materials_pbrSpecularGlossiness": {
                    "diffuseFactor": [0.5, 0.5, 0.5, 1], "glossinessFactor": 0.75, "diffuseTexture": {"index": 0}
                }}
            })",
                                                         kTwoTextures);
            CHECK(hasWarning(document, "specular-glossiness"));
            const MaterialData& data = document.materials[0].material;
            CHECK(data.baseColor == Vec4(0.5f, 0.5f, 0.5f, 1.0f));
            CHECK(data.roughness == 0.25f);
            CHECK(data.metallic == 0.0f);
            CHECK(document.materials[0].images.baseColor == 0u);
        }
        SUBCASE("a texture in a format that cannot be read") {
            const GltfDocument document = importMaterial(directory, R"({
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}
            })",
                                                         R"({
                "images": [{"uri": "a.ktx2"}],
                "textures": [{"extensions": {"KHR_texture_basisu": {"source": 0}}}]
            })");
            CHECK(hasWarning(document, "uses a texture in a format that is not supported"));
            CHECK_FALSE(document.materials[0].images.baseColor.has_value());
        }
    }

    TEST_CASE("images are found in files, in the buffer and in data URIs") {
        const TempDirectory directory;
        const std::vector<std::byte> png = test::encodeSolidPng(2, 2, Vec4(1.0f, 0.0f, 0.0f, 1.0f));

        GltfFile file;
        const int view = file.addView(png);
        file.json["images"] = parseOrFail(R"([
            {"name": "Outside", "uri": "textures/my%20wall.png"},
            {"mimeType": "image/png"},
            {"name": "Inline"}
        ])");
        file.json["images"][1]["bufferView"] = view;
        file.json["images"][2]["uri"] = fmt::format("data:image/png;base64,{}", test::encodeBase64(png));

        SUBCASE("with the data loaded") {
            const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "images")));
            REQUIRE(document.images.size() == 3);
            CHECK(document.images[0].name == "Outside");
            // Escapes are resolved; the path stays relative to the glTF file.
            CHECK(document.images[0].uri == "textures/my wall.png");
            CHECK_FALSE(document.images[0].isEmbedded());
            CHECK(document.images[0].encoded.empty());

            CHECK(document.images[1].name == "Image 1");
            CHECK(document.images[1].isEmbedded());
            CHECK(document.images[1].encoded == png);

            CHECK(document.images[2].isEmbedded());
            CHECK(document.images[2].encoded == png);
        }
        SUBCASE("with only the structure") {
            const GltfDocument document =
                unwrap(importGltf(file.write(directory.path(), "images"), {.loadData = false}));
            REQUIRE(document.images.size() == 3);
            CHECK(document.images[0].uri == "textures/my wall.png");
            CHECK(document.images[1].isEmbedded());
            CHECK(document.images[1].encoded.empty());
            CHECK(document.images[2].isEmbedded());
            CHECK(document.images[2].encoded.empty());
        }
    }

    TEST_CASE("nodes keep their hierarchy, their placement and their cameras") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["meshes"] = parseOrFail(R"([{"name": "Tri", "primitives": [{"attributes": {"POSITION": 0}}]}])");
        file.json["cameras"] = parseOrFail(R"([
            {"type": "perspective", "perspective": {"yfov": 0.5, "znear": 0.25, "zfar": 500}},
            {"type": "perspective", "perspective": {"yfov": 1.25, "znear": 0.5}},
            {"type": "orthographic", "orthographic": {"xmag": 8, "ymag": 4, "znear": 1, "zfar": 50}}
        ])");
        file.json["nodes"] = parseOrFail(R"([
            {"name": "Root", "children": [1, 2], "translation": [1, 2, 3], "scale": [2, 2, 2]},
            {"name": "Turned", "rotation": [0, 0.7071068, 0, 0.7071068], "mesh": 0, "children": [3]},
            {"camera": 0},
            {"name": "By matrix", "matrix": [0, 0, -3, 0,  0, 2, 0, 0,  1, 0, 0, 0,  5, 6, 7, 1], "camera": 2},
            {"name": "Elsewhere", "camera": 1}
        ])");
        file.json["scenes"] = parseOrFail(R"([{"nodes": [4]}, {"nodes": [0]}])");
        file.json["scene"] = 1;

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "nodes")));
        REQUIRE(document.nodes.size() == 5);
        // The default scene, not the first one.
        CHECK(document.roots == std::vector<uint32_t>{0});

        const GltfNode& root = document.nodes[0];
        CHECK(root.name == "Root");
        CHECK(root.transform.position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(root.transform.scale == Vec3(2.0f));
        CHECK(root.transform.rotation == kIdentityQuat);
        CHECK(root.children == std::vector<uint32_t>{1, 2});
        CHECK_FALSE(root.parent.has_value());

        const GltfNode& turned = document.nodes[1];
        CHECK(turned.parent == 0u);
        CHECK(turned.mesh == 0u);
        // A quarter turn about Y: -Z, which is forward, now points to -X.
        CHECK_NEAR(turned.transform.rotation * Vec3(0.0f, 0.0f, -1.0f), Vec3(-1.0f, 0.0f, 0.0f), 1.0e-5f);

        const GltfNode& unnamed = document.nodes[2];
        CHECK(unnamed.name == "Node 2");
        REQUIRE(unnamed.camera.has_value());
        CHECK_FALSE(unnamed.camera->orthographic);
        CHECK(unnamed.camera->verticalFov == 0.5f);
        CHECK(unnamed.camera->nearPlane == 0.25f);
        CHECK(unnamed.camera->farPlane == 500.0f);

        const GltfNode& byMatrix = document.nodes[3];
        CHECK(byMatrix.parent == 1u);
        CHECK_NEAR(byMatrix.transform.position, Vec3(5.0f, 6.0f, 7.0f), 1.0e-5f);
        CHECK_NEAR(byMatrix.transform.scale, Vec3(3.0f, 2.0f, 1.0f), 1.0e-5f);
        // The same quarter turn, written as a matrix: X now points along -Z.
        CHECK_NEAR(byMatrix.transform.rotation * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f), 1.0e-5f);
        REQUIRE(byMatrix.camera.has_value());
        CHECK(byMatrix.camera->orthographic);
        // glTF gives half the height.
        CHECK(byMatrix.camera->orthographicHeight == 8.0f);
        CHECK(byMatrix.camera->nearPlane == 1.0f);
        CHECK(byMatrix.camera->farPlane == 50.0f);

        // A far plane the file does not give is reported as zero.
        REQUIRE(document.nodes[4].camera.has_value());
        CHECK(document.nodes[4].camera->farPlane == 0.0f);
        CHECK(document.nodes[4].camera->verticalFov == 1.25f);
    }

    TEST_CASE("a file without scenes uses every node that has no parent") {
        const TempDirectory directory;
        GltfFile file;
        file.json["nodes"] = parseOrFail(R"([{"children": [2]}, {}, {}])");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "noscene")));
        CHECK(document.roots == std::vector<uint32_t>{0, 1});
    }

    TEST_CASE("a file without a default scene uses its first") {
        const TempDirectory directory;
        GltfFile file;
        file.json["nodes"] = parseOrFail(R"([{}, {}, {}])");
        file.json["scenes"] = parseOrFail(R"([{"nodes": [2, 1]}, {"nodes": [0]}])");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "first")));
        CHECK(document.roots == std::vector<uint32_t>{2, 1});
    }

    TEST_CASE("a scene that lists a node twice does not make it a root twice") {
        const TempDirectory directory;
        GltfFile file;
        file.json["nodes"] = parseOrFail(R"([{"children": [1]}, {}])");
        file.json["scenes"] = parseOrFail(R"([{"nodes": [0, 0]}])");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "roots")));
        CHECK(document.roots == std::vector<uint32_t>{0});
    }

    TEST_CASE("hierarchies that are not trees are refused") {
        const TempDirectory directory;
        SUBCASE("a scene that lists a node which has a parent") {
            GltfFile file;
            file.json["nodes"] = parseOrFail(R"([{"children": [1]}, {}])");
            file.json["scenes"] = parseOrFail(R"([{"nodes": [0, 1]}])");
            CHECK(importFailure(file.write(directory.path(), "tree")).code == ErrorCode::ParseError);
        }
        SUBCASE("a node with two parents") {
            GltfFile file;
            file.json["nodes"] = parseOrFail(R"([{"children": [2]}, {"children": [2]}, {}])");
            CHECK(importFailure(file.write(directory.path(), "tree")).code == ErrorCode::ParseError);
        }
        SUBCASE("nodes that are each other's ancestors") {
            GltfFile file;
            file.json["nodes"] = parseOrFail(R"([{"children": [1]}, {"children": [2]}, {"children": [0]}])");
            CHECK(importFailure(file.write(directory.path(), "tree")).code == ErrorCode::ParseError);
        }
        SUBCASE("a child that does not exist") {
            GltfFile file;
            file.json["nodes"] = parseOrFail(R"([{"children": [5]}])");
            CHECK(importFailure(file.write(directory.path(), "tree")).code == ErrorCode::ParseError);
        }
    }

    TEST_CASE("a broken rotation is replaced by none") {
        const TempDirectory directory;
        GltfFile file;
        file.json["nodes"] = parseOrFail(R"([{"rotation": [0, 0, 0, 0]}, {"rotation": [0, 3, 0, 4]}])");
        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "rotation")));
        CHECK(document.nodes[0].transform.rotation == kIdentityQuat);
        // One that is merely not unit length is normalized.
        CHECK(glm::length(document.nodes[1].transform.rotation) == doctest::Approx(1.0f));
        CHECK(document.nodes[1].transform.rotation.y == doctest::Approx(0.6f));
    }

    TEST_CASE("binary files and embedded buffers hold the same model") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {0, 1, 2, 0, 2, 3});
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}])");

        const GltfDocument separate = unwrap(importGltf(file.write(directory.path(), "separate")));
        const GltfDocument embedded =
            unwrap(importGltf(file.write(directory.path(), "embedded", GltfFile::Buffer::DataUri)));
        const GltfDocument binary = unwrap(importGltf(file.writeBinary(directory.path(), "binary")));

        CHECK(separate.meshes[0].mesh.triangleCount() == 2);
        CHECK(embedded.meshes[0].mesh == separate.meshes[0].mesh);
        CHECK(binary.meshes[0].mesh == separate.meshes[0].mesh);
    }

    TEST_CASE("the structure alone lists the same meshes and submeshes, without their data") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["materials"] = parseOrFail(R"([{"name": "A"}, {"name": "B"}])");
        file.json["meshes"] = parseOrFail(R"([{"name": "Two", "primitives": [
            {"attributes": {"POSITION": 0}, "material": 1},
            {"attributes": {"POSITION": 0}, "mode": 1},
            {"attributes": {"POSITION": 0}, "material": 0}
        ]}])");
        file.json["nodes"] = parseOrFail(R"([{"mesh": 0}])");
        const std::filesystem::path path = file.write(directory.path(), "structure");
        // Proof that the buffer is not touched: it is no longer there.
        std::filesystem::remove(directory.path() / "structure.bin");

        const GltfDocument document = unwrap(importGltf(path, {.loadData = false}));
        REQUIRE(document.meshes.size() == 1);
        CHECK(document.meshes[0].mesh.name == "Two");
        CHECK(document.meshes[0].mesh.vertices.empty());
        CHECK(document.meshes[0].mesh.submeshes.size() == 2);
        CHECK(document.meshes[0].materials == std::vector<std::optional<uint32_t>>{1u, 0u});
        CHECK(document.materials.size() == 2);
        CHECK(document.nodes[0].mesh == 0u);
        CHECK(hasWarning(document, "points or lines"));

        // With the data, the same file cannot be read at all.
        const Error error = importFailure(path);
        CHECK(error.message.find("structure.bin") != std::string::npos);
    }

    TEST_CASE("an empty glTF file is an empty document") {
        const TempDirectory directory;
        const GltfDocument document = unwrap(importGltf(GltfFile().write(directory.path(), "empty")));
        CHECK(document.meshes.empty());
        CHECK(document.materials.empty());
        CHECK(document.images.empty());
        CHECK(document.nodes.empty());
        CHECK(document.roots.empty());
        CHECK(document.warnings.empty());
    }

    TEST_CASE("features that are not imported are named") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("MAT4", kGltfFloat, std::vector<float>(16, 0.0f));
        file.json["nodes"] =
            parseOrFail(R"([{"name": "Bone"}, {"name": "Lamp", "extensions": {"KHR_lights_punctual": {"light": 0}}}])");
        file.json["skins"] = parseOrFail(R"([{"joints": [0], "inverseBindMatrices": 0}])");
        file.json["extensions"] = parseOrFail(R"({"KHR_lights_punctual": {"lights": [{"type": "point"}]}})");
        file.json["extensionsUsed"] = parseOrFail(R"(["KHR_lights_punctual"])");

        const GltfDocument document = unwrap(importGltf(file.write(directory.path(), "features")));
        CHECK(hasWarning(document, "it contains skins or animations, which are not imported"));
        CHECK(hasWarning(document, "it contains lights, which are not imported"));
    }

    TEST_CASE("files that cannot be read are refused with a reason") {
        const TempDirectory directory;
        const auto writeText = [&](std::string_view name, std::string_view text) {
            const std::filesystem::path path = directory.path() / pathFromUtf8(name);
            REQUIRE(writeTextFile(path, text).has_value());
            return path;
        };

        SUBCASE("a file that does not exist") {
            const Error error = importFailure(directory.path() / "missing.gltf");
            CHECK(error.code == ErrorCode::NotFound);
        }
        SUBCASE("text that is not JSON") {
            const Error error = importFailure(writeText("garbage.gltf", "this is not a glTF file at all"));
            CHECK(error.code == ErrorCode::ParseError);
            CHECK(error.message.find("its JSON is malformed") != std::string::npos);
        }
        SUBCASE("a file too short to be anything") {
            const Error error = importFailure(writeText("short.gltf", "{}"));
            CHECK(error.code == ErrorCode::ParseError);
        }
        SUBCASE("glTF 1.0") {
            const Error error = importFailure(writeText("old.gltf", R"({"asset": {"version": "1.0"}, "nodes": {}})"));
            CHECK(error.message.find("glTF 1.0") != std::string::npos);
        }
        SUBCASE("a binary file cut short") {
            GltfFile file;
            file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
            file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");
            const std::filesystem::path path = file.writeBinary(directory.path(), "cut");
            std::vector<std::byte> bytes = unwrap(readBinaryFile(path));
            bytes.resize(bytes.size() - 20);
            REQUIRE(writeBinaryFile(path, bytes).has_value());
            CHECK(importFailure(path).code == ErrorCode::ParseError);
        }
        SUBCASE("an extension it cannot do without") {
            const Error error = importFailure(
                writeText("draco.gltf",
                          R"({"asset": {"version": "2.0"}, "extensionsRequired": ["KHR_draco_mesh_compression"]})"));
            CHECK(error.code == ErrorCode::Unsupported);
            CHECK(error.message.find("it requires the extension KHR_draco_mesh_compression") != std::string::npos);
        }
    }

    TEST_CASE("files whose numbers do not match their data are refused") {
        const TempDirectory directory;
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {0, 1, 2});
        file.json["meshes"] =
            parseOrFail(R"([{"name": "Tri", "primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}])");

        SUBCASE("the file as it is imports") {
            CHECK(importGltf(file.write(directory.path(), "fine")).has_value());
        }
        SUBCASE("an accessor longer than its buffer view") {
            file.accessors()[0]["count"] = 4;
            CHECK(importFailure(file.write(directory.path(), "bad")).message.find("accessor 0 refers to data beyond") !=
                  std::string::npos);
        }
        SUBCASE("an accessor that starts beyond its buffer view") {
            file.accessors()[0]["byteOffset"] = 4096;
            CHECK(importFailure(file.write(directory.path(), "bad")).code == ErrorCode::ParseError);
        }
        SUBCASE("a buffer view longer than the buffer") {
            file.views()[1]["byteLength"] = 4096;
            CHECK(importFailure(file.write(directory.path(), "bad")).code == ErrorCode::ParseError);
        }
        SUBCASE("a buffer view that starts beyond the buffer") {
            file.views()[1]["byteOffset"] = 9223372036854775807ll;
            CHECK(importFailure(file.write(directory.path(), "bad")).code == ErrorCode::ParseError);
        }
        SUBCASE("a count chosen to make the size calculation overflow") {
            // 2^62 + 1 elements of four bytes each: the product wraps around to four.
            file.accessors()[1]["componentType"] = kGltfUnsignedInt;
            file.accessors()[1]["count"] = 4611686018427387905ll;
            const Error error = importFailure(file.write(directory.path(), "bad"));
            CHECK(error.code == ErrorCode::OutOfRange);
            CHECK(error.message.find("accessor 1 has 4611686018427387905 elements") != std::string::npos);
        }
        SUBCASE("more vertices than a mesh may have") {
            // Positions without a buffer view are all zero, and cost a file nothing.
            file.accessors()[0].erase("bufferView");
            file.accessors()[0]["count"] = kMaxMeshVertices + 1;
            file.json["meshes"][0]["primitives"][0].erase("indices");
            const Error error = importFailure(file.write(directory.path(), "bad"));
            CHECK(error.code == ErrorCode::OutOfRange);
            CHECK(error.message.find("POSITION of part 0 of mesh 'Tri'") != std::string::npos);
        }
        SUBCASE("an index without a vertex") {
            file.bytes().resize(36);
            test::appendBytes<uint16_t>(file.bytes(), {0, 1, 7});
            const Error error = importFailure(file.write(directory.path(), "bad"));
            CHECK(error.code == ErrorCode::ParseError);
        }
        SUBCASE("attributes of different lengths") {
            file.addAccessor<float>("VEC3", kGltfFloat, {0, 0, 1, 0, 0, 1});
            file.json["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 2;
            CHECK(importFailure(file.write(directory.path(), "bad")).code == ErrorCode::ParseError);
        }
        SUBCASE("an attribute with the wrong number of components") {
            file.addAccessor<float>("VEC2", kGltfFloat, {0, 0, 1, 0, 0, 1});
            file.json["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 2;
            const Error error = importFailure(file.write(directory.path(), "bad"));
            CHECK(error.message.find("NORMAL of part 0 of mesh 'Tri' has the wrong number of components") !=
                  std::string::npos);
        }
        SUBCASE("a buffer file shorter than the glTF says") {
            const std::filesystem::path path = file.write(directory.path(), "bad");
            REQUIRE(writeBinaryFile(directory.path() / "bad.bin", std::vector<std::byte>(10)).has_value());
            const Error error = importFailure(path);
            CHECK(error.message.find("'bad.bin' has 10 bytes, but the file expects 42") != std::string::npos);
        }
        SUBCASE("a sparse accessor that replaces an element which does not exist") {
            file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {9});
            file.addAccessor<float>("VEC3", kGltfFloat, {0.0f, 5.0f, 0.0f});
            file.accessors()[0]["sparse"] = parseOrFail(R"({
                "count": 1, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}
            })");
            CHECK(importFailure(file.write(directory.path(), "bad")).code == ErrorCode::ParseError);
        }
        SUBCASE("a sparse accessor with more replacements than its data holds") {
            file.addAccessor<uint16_t>("SCALAR", kGltfUnsignedShort, {2});
            file.addAccessor<float>("VEC3", kGltfFloat, {0.0f, 5.0f, 0.0f});
            file.accessors()[0]["sparse"] = parseOrFail(R"({
                "count": 3, "indices": {"bufferView": 2, "componentType": 5123}, "values": {"bufferView": 3}
            })");
            CHECK(importFailure(file.write(directory.path(), "bad")).message.find("sparse") != std::string::npos);
        }
    }

    TEST_CASE("a file may describe only so much geometry in all, however little data it holds") {
        const TempDirectory directory;
        // Three meshes that all use the same three positions: nine vertices, nine indices.
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["meshes"] = parseOrFail(R"([
            {"primitives": [{"attributes": {"POSITION": 0}}]},
            {"primitives": [{"attributes": {"POSITION": 0}}]},
            {"primitives": [{"attributes": {"POSITION": 0}}]}
        ])");
        const std::filesystem::path path = file.write(directory.path(), "shared");

        CHECK(unwrap(importGltf(path, {.maxVertices = 9, .maxIndices = 9})).meshes.size() == 3);

        for (const GltfImportOptions& options : {GltfImportOptions{.maxVertices = 8, .maxIndices = 9},
                                                 GltfImportOptions{.maxVertices = 9, .maxIndices = 8},
                                                 GltfImportOptions{.maxVertices = 0, .maxIndices = 0}}) {
            const Error error = importFailure(path, options);
            CHECK(error.code == ErrorCode::OutOfRange);
            CHECK(error.message.find(fmt::format("the limits for a whole file are {} vertices and {} indices",
                                                 options.maxVertices, options.maxIndices)) != std::string::npos);
        }

        // Listing what a file contains reads no geometry, and is not limited.
        CHECK(importGltf(path, {.loadData = false, .maxVertices = 0, .maxIndices = 0}).has_value());
    }

    TEST_CASE("strips and fans count as the triangles they make") {
        const TempDirectory directory;
        // A strip of four vertices is two triangles: six corners.
        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kSquarePositions);
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}, "mode": 5}]}])");
        const std::filesystem::path path = file.write(directory.path(), "strip");

        CHECK(importGltf(path, {.maxVertices = 4, .maxIndices = 6}).has_value());
        CHECK(importFailure(path, {.maxVertices = 4, .maxIndices = 5}).code == ErrorCode::OutOfRange);
    }

    TEST_CASE("the images inside a file may only take so many bytes in all") {
        const TempDirectory directory;
        const std::vector<std::byte> png = test::encodeSolidPng(2, 2, Vec4(1.0f));

        // The same bytes used by two images, and once more as a data URI.
        GltfFile file;
        const int view = file.addView(png);
        file.json["images"] = parseOrFail(R"([{"mimeType": "image/png"}, {"mimeType": "image/png"}, {}])");
        file.json["images"][0]["bufferView"] = view;
        file.json["images"][1]["bufferView"] = view;
        file.json["images"][2]["uri"] = fmt::format("data:image/png;base64,{}", test::encodeBase64(png));
        const std::filesystem::path path = file.write(directory.path(), "images");

        CHECK(importGltf(path, {.maxEmbeddedImageBytes = 3 * png.size()}).has_value());

        const Error error = importFailure(path, {.maxEmbeddedImageBytes = 3 * png.size() - 1});
        CHECK(error.code == ErrorCode::OutOfRange);
        CHECK(error.message.find("its embedded images take more than") != std::string::npos);

        // Images in files of their own are not read by the importer and do not count.
        CHECK(importGltf(path, {.loadData = false, .maxEmbeddedImageBytes = 0}).has_value());
    }

    TEST_CASE("an embedded image that is not in the file is refused") {
        const TempDirectory directory;
        GltfFile file;
        const int view = file.addView(test::encodeSolidPng(2, 2, Vec4(1.0f)));
        file.json["images"] = parseOrFail(R"([{"name": "Lost", "mimeType": "image/png"}])");
        file.json["images"][0]["bufferView"] = view;

        SUBCASE("longer than the buffer") {
            file.views()[0]["byteLength"] = 100000;
            CHECK(importFailure(file.write(directory.path(), "image")).code == ErrorCode::ParseError);
        }
        SUBCASE("beyond the end of the buffer") {
            file.views()[0]["byteOffset"] = 9223372036854775807ll;
            CHECK(importFailure(file.write(directory.path(), "image")).code == ErrorCode::ParseError);
        }
        SUBCASE("in a buffer that was never loaded") {
            // A second buffer without a location: there is nothing to read it from.
            file.json["buffers"][1]["byteLength"] = 100;
            file.views()[0]["buffer"] = 1;
            file.views()[0]["byteOffset"] = 0;
            const Error error = importFailure(file.write(directory.path(), "image"));
            CHECK(error.message.find("image 'Lost' refers to data that is not in the file") != std::string::npos);
        }
    }

    TEST_CASE("an image with neither a file nor data, or with a data URI that is not base64, is refused") {
        const TempDirectory directory;
        SUBCASE("neither") {
            GltfFile file;
            file.json["images"] = parseOrFail(R"([{"name": "Nothing"}])");
            CHECK(importFailure(file.write(directory.path(), "image")).message.find("has neither a file nor data") !=
                  std::string::npos);
        }
        SUBCASE("not base64") {
            GltfFile file;
            file.json["images"] = parseOrFail(R"([{"name": "Plain", "uri": "data:text/plain,hello"}])");
            CHECK(importFailure(file.write(directory.path(), "image")).message.find("is not base64") !=
                  std::string::npos);
        }
        SUBCASE("broken base64") {
            GltfFile file;
            file.json["images"] = parseOrFail(R"([{"name": "Broken", "uri": "data:image/png;base64,!!!!!!!!"}])");
            CHECK(importFailure(file.write(directory.path(), "image")).message.find("cannot be decoded") !=
                  std::string::npos);
        }
    }

    TEST_CASE("files a glTF refers to are only read from inside the allowed directory") {
        const TempDirectory directory;
        const std::filesystem::path models = directory.path() / "project" / "models";
        const std::filesystem::path shared = directory.path() / "project" / "shared";
        REQUIRE(ensureDirectory(models).has_value());
        REQUIRE(ensureDirectory(shared).has_value());

        GltfFile file;
        file.addAccessor<float>("VEC3", kGltfFloat, kTrianglePositions);
        file.json["meshes"] = parseOrFail(R"([{"primitives": [{"attributes": {"POSITION": 0}}]}])");
        const std::filesystem::path path = file.write(models, "model");

        // Move the buffer next door and point the file at it.
        std::filesystem::rename(models / "model.bin", shared / "model.bin");
        Json document = unwrap(readJsonFile(path));
        document["buffers"][0]["uri"] = "../shared/model.bin";
        REQUIRE(writeJsonFile(path, document).has_value());

        SUBCASE("by default, the directory of the glTF file") {
            const Error error = importFailure(path);
            CHECK(error.message.find("it refers to '../shared/model.bin', which is outside") != std::string::npos);
        }
        SUBCASE("a wider directory allows it") {
            CHECK(importGltf(path, {.rootDirectory = directory.path() / "project"}).has_value());
        }
        SUBCASE("a web address is never followed") {
            document["buffers"][0]["uri"] = "https://example.com/model.bin";
            REQUIRE(writeJsonFile(path, document).has_value());
            CHECK(importFailure(path).message.find("only files next to it and base64 data are supported") !=
                  std::string::npos);
        }
    }
}
