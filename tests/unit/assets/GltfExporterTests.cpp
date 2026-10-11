#include "cordite/assets/GltfExporter.h"

#include "cordite/assets/GltfImporter.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/Json.h"

#include "AssetTestSupport.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::sameSurface;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// The vertices a mesh's triangles use, as rows of numbers.
std::vector<std::array<float, 14>> usedVertices(const MeshData& mesh) {
    std::vector<bool> used(mesh.vertices.size(), false);
    for (const uint32_t index : mesh.indices) {
        used[index] = true;
    }
    std::vector<std::array<float, 14>> rows;
    for (size_t index = 0; index < mesh.vertices.size(); ++index) {
        if (used[index]) {
            std::array<float, 14> row{};
            std::memcpy(row.data(), &mesh.vertices[index], sizeof(MeshVertex));
            rows.push_back(row);
        }
    }
    return rows;
}

/// True when two meshes have the same vertices (all attributes) and the same triangles.
bool sameMesh(const MeshData& a, const MeshData& b, float tolerance = 1.0e-5f) {
    const auto first = usedVertices(a);
    const auto second = usedVertices(b);
    if (first.size() != second.size() || !sameSurface(a, b, tolerance)) {
        return false;
    }

    // Every vertex of one mesh has a partner of its own in the other. Vertices are
    // paired by search rather than by sorting: a difference in the last bit must not
    // change which vertex is compared with which.
    std::vector<bool> taken(second.size(), false);
    for (const std::array<float, 14>& vertex : first) {
        bool found = false;
        for (size_t candidate = 0; candidate < second.size() && !found; ++candidate) {
            if (taken[candidate]) {
                continue;
            }
            found = true;
            for (size_t member = 0; member < 14 && found; ++member) {
                found = std::abs(vertex[member] - second[candidate][member]) <= tolerance;
            }
            if (found) {
                taken[candidate] = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/// A document that uses everything the exporter writes.
GltfDocument makeDocument() {
    GltfDocument document;

    document.images.push_back({.name = "Wall", .uri = "textures/wall.png"});
    document.images.push_back({.name = "Packed", .encoded = test::encodeSolidPng(2, 2, Vec4(0.2f, 0.4f, 0.6f, 1.0f))});

    GltfMaterial paint;
    paint.name = "Paint";
    paint.material.baseColor = Vec4(0.75f, 0.25f, 0.125f, 0.5f);
    paint.material.metallic = 0.25f;
    paint.material.roughness = 0.625f;
    paint.material.normalScale = 1.5f;
    paint.material.occlusionStrength = 0.75f;
    paint.material.emissiveColor = Vec3(1.0f, 0.5f, 0.25f);
    paint.material.emissiveIntensity = 4.0f;
    paint.material.alphaMode = AlphaMode::Masked;
    paint.material.alphaCutoff = 0.25f;
    paint.material.doubleSided = true;
    paint.material.uvChannel = 1;
    paint.material.occlusionUvChannel = 1;
    paint.material.uvScale = Vec2(4.0f, 2.0f);
    paint.material.uvOffset = Vec2(0.5f, 0.25f);
    paint.material.wrapU = TextureWrap::Clamp;
    paint.material.wrapV = TextureWrap::MirroredRepeat;
    paint.material.filter = TextureFilter::Nearest;
    paint.images = {.baseColor = 0, .metallicRoughness = 1, .normal = 1, .occlusion = 1, .emissive = 0};
    document.materials.push_back(paint);

    GltfMaterial glass;
    glass.name = "Glass";
    glass.material.alphaMode = AlphaMode::Blended;
    glass.material.baseColor = Vec4(0.5f, 0.75f, 1.0f, 0.25f);
    document.materials.push_back(glass);

    GltfMesh crate;
    crate.mesh = makeBox(Vec3(1.0f, 2.0f, 3.0f));
    crate.mesh.name = "Crate";
    crate.materials = {0};
    document.meshes.push_back(crate);

    // Two parts: the second has a material, the first has none.
    GltfMesh lamp;
    lamp.mesh.name = "Lamp";
    const MeshData base = makeCylinder(0.25f, 1.0f, 12);
    const MeshData shade = makeSphere(0.5f, 12, 6);
    appendSubmesh(lamp.mesh, base.vertices, base.indices);
    appendSubmesh(lamp.mesh, shade.vertices, shade.indices);
    lamp.materials = {std::nullopt, 1};
    document.meshes.push_back(lamp);

    GltfNode room;
    room.name = "Room";
    room.children = {1, 2};
    document.nodes.push_back(room);

    GltfNode crateNode;
    crateNode.name = "Crate";
    crateNode.parent = 0;
    crateNode.mesh = 0;
    crateNode.children = {3};
    crateNode.transform.position = Vec3(1.0f, 0.5f, -2.0f);
    crateNode.transform.rotation = glm::angleAxis(0.5f, glm::normalize(Vec3(1.0f, 2.0f, 3.0f)));
    crateNode.transform.scale = Vec3(1.0f, 2.0f, 0.5f);
    document.nodes.push_back(crateNode);

    GltfNode view;
    view.name = "View";
    view.parent = 0;
    view.camera = ModelCamera{.verticalFov = 0.75f, .nearPlane = 0.25f, .farPlane = 250.0f};
    document.nodes.push_back(view);

    GltfNode lampNode;
    lampNode.name = "Lamp";
    lampNode.parent = 1;
    lampNode.mesh = 1;
    lampNode.camera =
        ModelCamera{.orthographic = true, .orthographicHeight = 6.0f, .nearPlane = 0.5f, .farPlane = 40.0f};
    document.nodes.push_back(lampNode);

    document.roots = {0};
    return document;
}

void checkSameDocument(const GltfDocument& actual, const GltfDocument& expected) {
    CHECK(actual.warnings.empty());
    CHECK(actual.roots == expected.roots);

    REQUIRE(actual.images.size() == expected.images.size());
    for (size_t index = 0; index < expected.images.size(); ++index) {
        CHECK(actual.images[index] == expected.images[index]);
    }

    REQUIRE(actual.materials.size() == expected.materials.size());
    for (size_t index = 0; index < expected.materials.size(); ++index) {
        CAPTURE(expected.materials[index].name);
        CHECK(actual.materials[index] == expected.materials[index]);
    }

    REQUIRE(actual.meshes.size() == expected.meshes.size());
    for (size_t index = 0; index < expected.meshes.size(); ++index) {
        const GltfMesh& mesh = actual.meshes[index];
        const GltfMesh& original = expected.meshes[index];
        CAPTURE(original.mesh.name);
        CHECK(mesh.mesh.name == original.mesh.name);
        CHECK(mesh.materials == original.materials);
        REQUIRE(mesh.mesh.submeshes.size() == original.mesh.submeshes.size());
        for (size_t part = 0; part < original.mesh.submeshes.size(); ++part) {
            CHECK(mesh.mesh.submeshes[part].indexCount == original.mesh.submeshes[part].indexCount);
            CHECK_NEAR(mesh.mesh.submeshes[part].bounds.min, original.mesh.submeshes[part].bounds.min, 1.0e-6f);
            CHECK_NEAR(mesh.mesh.submeshes[part].bounds.max, original.mesh.submeshes[part].bounds.max, 1.0e-6f);
        }
        CHECK(sameMesh(mesh.mesh, original.mesh));
        CHECK(validateMesh(mesh.mesh).has_value());
    }

    REQUIRE(actual.nodes.size() == expected.nodes.size());
    for (size_t index = 0; index < expected.nodes.size(); ++index) {
        const GltfNode& node = actual.nodes[index];
        const GltfNode& original = expected.nodes[index];
        CAPTURE(original.name);
        CHECK(node.name == original.name);
        CHECK(node.parent == original.parent);
        CHECK(node.children == original.children);
        CHECK(node.mesh == original.mesh);
        CHECK(node.camera == original.camera);
        CHECK_NEAR(node.transform.position, original.transform.position, 1.0e-6f);
        CHECK_NEAR(node.transform.scale, original.transform.scale, 1.0e-6f);
        CHECK(test::sameRotation(node.transform.rotation, original.transform.rotation, 1.0e-6f));
    }
}

/// Exports a document that must be refused and returns the error.
Error exportFailure(const GltfDocument& document, const std::filesystem::path& path) {
    const Status status = exportGltf(document, path);
    REQUIRE_FALSE(status.has_value());
    CHECK_FALSE(fileExists(path));
    return status.error();
}

uint32_t readUint32(const std::vector<std::byte>& bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

} // namespace

TEST_SUITE("GltfExporter") {
    TEST_CASE("a document written as .gltf reads back the same") {
        const TempDirectory directory;
        const GltfDocument document = makeDocument();
        const std::filesystem::path path = directory.path() / "scene.gltf";
        REQUIRE(exportGltf(document, path).has_value());

        CHECK(fileExists(directory.path() / "scene.bin"));
        checkSameDocument(unwrap(importGltf(path)), document);
    }

    TEST_CASE("a document written as .glb reads back the same") {
        const TempDirectory directory;
        const GltfDocument document = makeDocument();
        const std::filesystem::path path = directory.path() / "scene.glb";
        REQUIRE(exportGltf(document, path).has_value());

        // One file holds everything.
        CHECK_FALSE(fileExists(directory.path() / "scene.bin"));
        checkSameDocument(unwrap(importGltf(path)), document);
    }

    TEST_CASE("the extension decides the form, whatever its case") {
        const TempDirectory directory;
        const GltfDocument document = makeDocument();
        REQUIRE(exportGltf(document, directory.path() / "UPPER.GLB").has_value());
        const std::vector<std::byte> bytes = unwrap(readBinaryFile(directory.path() / "UPPER.GLB"));
        CHECK(readUint32(bytes, 0) == 0x46546C67u);
    }

    TEST_CASE("the text form is well-formed glTF") {
        const TempDirectory directory;
        const std::filesystem::path path = directory.path() / "scene.gltf";
        REQUIRE(exportGltf(makeDocument(), path).has_value());
        const Json json = unwrap(readJsonFile(path));

        CHECK(json["asset"]["version"] == "2.0");
        CHECK(json["asset"]["generator"].get<std::string>().starts_with("Cordite "));
        CHECK(json["scene"] == 0);
        CHECK(json["scenes"][0]["nodes"] == Json::array({0}));

        // The buffer is the file next door, and as long as it says.
        REQUIRE(json["buffers"].size() == 1);
        CHECK(json["buffers"][0]["uri"] == "scene.bin");
        const std::vector<std::byte> buffer = unwrap(readBinaryFile(directory.path() / "scene.bin"));
        CHECK(json["buffers"][0]["byteLength"] == buffer.size());

        // Every view lies inside the buffer and starts on a four-byte boundary.
        for (const Json& view : json["bufferViews"]) {
            CHECK(view["byteOffset"].get<size_t>() % 4 == 0);
            CHECK(view["byteOffset"].get<size_t>() + view["byteLength"].get<size_t>() <= buffer.size());
        }

        // Vertices are interleaved; positions state their bounds, as the format requires.
        const Json& crate = json["meshes"][0];
        CHECK(crate["name"] == "Crate");
        const Json& positions = json["accessors"][crate["primitives"][0]["attributes"]["POSITION"].get<size_t>()];
        CHECK(positions["min"] == Json::array({-0.5, -1.0, -1.5}));
        CHECK(positions["max"] == Json::array({0.5, 1.0, 1.5}));
        CHECK(json["bufferViews"][positions["bufferView"].get<size_t>()]["byteStride"] == 56);
        // Few vertices: sixteen-bit indices.
        CHECK(json["accessors"][crate["primitives"][0]["indices"].get<size_t>()]["componentType"] == 5123);

        // The two parts of the lamp share their vertices and differ in indices and material.
        const Json& lamp = json["meshes"][1]["primitives"];
        REQUIRE(lamp.size() == 2);
        CHECK(lamp[0]["attributes"] == lamp[1]["attributes"]);
        CHECK(lamp[0]["indices"] != lamp[1]["indices"]);
        CHECK_FALSE(lamp[0].contains("material"));
        CHECK(lamp[1]["material"] == 1);

        const Json& paint = json["materials"][0];
        CHECK(paint["alphaMode"] == "MASK");
        CHECK(paint["alphaCutoff"] == 0.25);
        CHECK(paint["doubleSided"] == true);
        CHECK(paint["normalTexture"]["scale"] == 1.5);
        CHECK(paint["occlusionTexture"]["strength"] == 0.75);
        CHECK(paint["occlusionTexture"]["texCoord"] == 1);
        // Occlusion uses the same set of coordinates as the other textures here, and so is
        // scaled and shifted with them.
        CHECK(paint["occlusionTexture"]["extensions"]["KHR_texture_transform"]["scale"] == Json::array({4.0, 2.0}));
        CHECK(paint["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"]["scale"] ==
              Json::array({4.0, 2.0}));
        CHECK(paint["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] == 4.0);
        CHECK(json["materials"][1]["alphaMode"] == "BLEND");
        CHECK_FALSE(json["materials"][1].contains("alphaCutoff"));

        // Two images used with one sampler: two textures, one sampler.
        CHECK(json["samplers"].size() == 1);
        CHECK(json["samplers"][0]["magFilter"] == 9728);
        CHECK(json["samplers"][0]["wrapS"] == 33071);
        CHECK(json["samplers"][0]["wrapT"] == 33648);
        CHECK(json["textures"].size() == 2);

        CHECK(json["images"][0]["uri"] == "textures/wall.png");
        CHECK(json["images"][1]["mimeType"] == "image/png");
        CHECK(json["images"][1].contains("bufferView"));

        CHECK(json["cameras"].size() == 2);
        CHECK(json["cameras"][0]["type"] == "perspective");
        CHECK(json["cameras"][1]["orthographic"]["ymag"] == 3.0);

        const std::vector<std::string> extensions = json["extensionsUsed"].get<std::vector<std::string>>();
        CHECK(std::ranges::find(extensions, "KHR_texture_transform") != extensions.end());
        CHECK(std::ranges::find(extensions, "KHR_materials_emissive_strength") != extensions.end());
    }

    TEST_CASE("the binary form has a correct header and chunks") {
        const TempDirectory directory;
        const std::filesystem::path path = directory.path() / "scene.glb";
        REQUIRE(exportGltf(makeDocument(), path).has_value());
        const std::vector<std::byte> bytes = unwrap(readBinaryFile(path));

        REQUIRE(bytes.size() > 28);
        CHECK(readUint32(bytes, 0) == 0x46546C67u);
        CHECK(readUint32(bytes, 4) == 2u);
        CHECK(readUint32(bytes, 8) == bytes.size());

        const uint32_t jsonLength = readUint32(bytes, 12);
        CHECK(readUint32(bytes, 16) == 0x4E4F534Au);
        CHECK(jsonLength % 4 == 0);
        const std::string text(reinterpret_cast<const char*>(bytes.data()) + 20, jsonLength);
        const Json json = unwrap(parseJson(text));
        // In a .glb the buffer has no location: it is the chunk that follows.
        CHECK_FALSE(json["buffers"][0].contains("uri"));

        const size_t binaryHeader = 20 + jsonLength;
        const uint32_t binaryLength = readUint32(bytes, binaryHeader);
        CHECK(readUint32(bytes, binaryHeader + 4) == 0x004E4942u);
        CHECK(binaryLength % 4 == 0);
        CHECK(binaryHeader + 8 + binaryLength == bytes.size());
        CHECK(json["buffers"][0]["byteLength"].get<size_t>() <= binaryLength);
    }

    TEST_CASE("a document without geometry has no buffer") {
        const TempDirectory directory;
        GltfDocument document;
        document.materials.push_back({.name = "Only"});
        document.nodes.push_back({.name = "Empty"});
        document.roots = {0};

        SUBCASE("as text") {
            const std::filesystem::path path = directory.path() / "bare.gltf";
            REQUIRE(exportGltf(document, path).has_value());
            const Json json = unwrap(readJsonFile(path));
            CHECK_FALSE(json.contains("buffers"));
            CHECK_FALSE(json.contains("bufferViews"));
            CHECK_FALSE(json.contains("accessors"));
            CHECK_FALSE(json.contains("meshes"));
            CHECK_FALSE(json.contains("extensionsUsed"));
            CHECK_FALSE(fileExists(directory.path() / "bare.bin"));
            CHECK(unwrap(importGltf(path)).nodes[0].name == "Empty");
        }
        SUBCASE("as binary") {
            const std::filesystem::path path = directory.path() / "bare.glb";
            REQUIRE(exportGltf(document, path).has_value());
            const std::vector<std::byte> bytes = unwrap(readBinaryFile(path));
            // The header and the JSON chunk, and nothing after it.
            CHECK(bytes.size() == 20 + readUint32(bytes, 12));
            CHECK(unwrap(importGltf(path)).materials[0].name == "Only");
        }
    }

    TEST_CASE("an empty document is a valid file") {
        const TempDirectory directory;
        const std::filesystem::path path = directory.path() / "nothing.gltf";
        REQUIRE(exportGltf(GltfDocument(), path).has_value());
        const Json json = unwrap(readJsonFile(path));
        // A scene without nodes has no "nodes" member; an empty array is not allowed.
        CHECK(json["scenes"][0].empty());
        CHECK(unwrap(importGltf(path)).roots.empty());
    }

    TEST_CASE("large meshes get 32-bit indices") {
        const TempDirectory directory;
        GltfDocument document;
        document.meshes.push_back({.mesh = makePlane(Vec2(10.0f), UVec2(300, 300)), .materials = {std::nullopt}});
        REQUIRE(document.meshes[0].mesh.vertices.size() > 0xFFFF);

        const std::filesystem::path path = directory.path() / "large.gltf";
        REQUIRE(exportGltf(document, path).has_value());
        const Json json = unwrap(readJsonFile(path));
        CHECK(json["accessors"][json["meshes"][0]["primitives"][0]["indices"].get<size_t>()]["componentType"] == 5125);

        const GltfDocument imported = unwrap(importGltf(path));
        CHECK(imported.meshes[0].mesh.vertices.size() == document.meshes[0].mesh.vertices.size());
        CHECK(sameSurface(imported.meshes[0].mesh, document.meshes[0].mesh));
    }

    TEST_CASE("emission brighter than glTF allows goes into the strength") {
        const TempDirectory directory;
        GltfDocument document;
        GltfMaterial lava;
        lava.name = "Lava";
        lava.material.emissiveColor = Vec3(4.0f, 2.0f, 1.0f);
        lava.material.emissiveIntensity = 3.0f;
        document.materials.push_back(lava);

        const std::filesystem::path path = directory.path() / "lava.gltf";
        REQUIRE(exportGltf(document, path).has_value());
        const Json json = unwrap(readJsonFile(path));
        CHECK(json["materials"][0]["emissiveFactor"] == Json::array({1.0, 0.5, 0.25}));

        // What comes back is split differently, but emits the same light.
        const MaterialData imported = unwrap(importGltf(path)).materials[0].material;
        CHECK(imported.emissiveColor == Vec3(1.0f, 0.5f, 0.25f));
        CHECK(imported.emissiveIntensity == 12.0f);
    }

    TEST_CASE("reflectivity survives as an index of refraction") {
        const TempDirectory directory;
        for (const float specular : {0.0f, 0.25f, 0.5f, 0.8f, 1.0f}) {
            CAPTURE(specular);
            GltfDocument document;
            GltfMaterial material;
            material.material.specular = specular;
            document.materials.push_back(material);

            const std::filesystem::path path = directory.path() / "specular.gltf";
            REQUIRE(exportGltf(document, path).has_value());
            const Json json = unwrap(readJsonFile(path));
            // The default needs no extension.
            CHECK(json["materials"][0].contains("extensions") == (specular != 0.5f));
            CHECK(unwrap(importGltf(path)).materials[0].material.specular == doctest::Approx(specular).epsilon(1e-4));
        }
    }

    TEST_CASE("names with spaces and other special characters are escaped in the file and restored on import") {
        const TempDirectory directory;
        GltfDocument document = makeDocument();
        document.images[0].uri = "my textures/wall #1 50%.png";

        const std::filesystem::path path = directory.path() / pathFromUtf8("my scène.gltf");
        REQUIRE(exportGltf(document, path).has_value());
        const Json json = unwrap(readJsonFile(path));
        CHECK(json["images"][0]["uri"] == "my%20textures/wall%20%231%2050%25.png");
        CHECK(json["buffers"][0]["uri"] == "my%20sc%C3%A8ne.bin");

        const GltfDocument imported = unwrap(importGltf(path));
        CHECK(imported.images[0].uri == "my textures/wall #1 50%.png");
        CHECK(imported.meshes[0].mesh.triangleCount() == 12);
    }

    TEST_CASE("an occlusion texture in a set of coordinates of its own is written as it is") {
        const TempDirectory directory;
        GltfDocument document = makeDocument();
        MaterialData& paint = document.materials[0].material;
        paint.uvChannel = 0;
        paint.occlusionUvChannel = 1;
        REQUIRE(paint.uvScale != Vec2(1.0f));
        REQUIRE(document.materials[0].images.occlusion.has_value());

        const std::filesystem::path path = directory.path() / "own.gltf";
        REQUIRE(exportGltf(document, path).has_value());
        const Json json = unwrap(readJsonFile(path));
        const Json& written = json["materials"][0];
        CHECK(written["occlusionTexture"]["texCoord"] == 1);
        CHECK_FALSE(written["occlusionTexture"].contains("extensions"));
        CHECK(written["pbrMetallicRoughness"]["baseColorTexture"].contains("extensions"));

        // And it reads back as it was, with nothing to remark on.
        const GltfDocument imported = unwrap(importGltf(path));
        CHECK(imported.warnings.empty());
        CHECK(imported.materials[0].material == paint);
    }

    TEST_CASE("a part without triangles is left out") {
        const TempDirectory directory;
        GltfDocument document;
        GltfMesh mesh;
        mesh.mesh = makeBox();
        appendSubmesh(mesh.mesh, {}, {});
        mesh.materials = {std::nullopt, std::nullopt};
        document.meshes.push_back(mesh);

        const std::filesystem::path path = directory.path() / "parts.gltf";
        REQUIRE(exportGltf(document, path).has_value());
        CHECK(unwrap(readJsonFile(path))["meshes"][0]["primitives"].size() == 1);
    }

    TEST_CASE("documents that are not consistent are refused, and nothing is written") {
        const TempDirectory directory;
        const std::filesystem::path path = directory.path() / "bad.gltf";
        GltfDocument document = makeDocument();

        SUBCASE("a file name that is neither .gltf nor .glb") {
            const Error error = exportFailure(document, directory.path() / "scene.obj");
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message.find("must end in .gltf or .glb") != std::string::npos);
        }
        SUBCASE("a mesh that is not valid") {
            document.meshes[0].mesh.indices[0] = 9999;
            const Error error = exportFailure(document, path);
            CHECK(error.message.find("mesh 'Crate'") != std::string::npos);
            CHECK(error.message.find(pathToUtf8(path)) != std::string::npos);
        }
        SUBCASE("a mesh without vertices") {
            document.meshes[0].mesh = MeshData();
            CHECK(exportFailure(document, path).message.find("it has no vertices") != std::string::npos);
        }
        SUBCASE("a mesh whose parts are all empty") {
            // Valid as a mesh, but glTF has no way to write it down.
            MeshData& mesh = document.meshes[0].mesh;
            mesh.indices.clear();
            mesh.submeshes = {Submesh{}};
            REQUIRE(validateMesh(mesh).has_value());
            CHECK(exportFailure(document, path).message.find("mesh 'Crate' has no triangles") != std::string::npos);
        }
        SUBCASE("a material that does not exist") {
            document.meshes[0].materials[0] = 7;
            CHECK(exportFailure(document, path).message.find("mesh 'Crate' refers to material 7") != std::string::npos);
        }
        SUBCASE("an image that does not exist") {
            document.materials[0].images.normal = 5;
            CHECK(exportFailure(document, path).message.find("material 'Paint' refers to image 5") !=
                  std::string::npos);
        }
        SUBCASE("an embedded image that is not an image") {
            document.images[1].encoded = {std::byte{'G'}, std::byte{'I'}, std::byte{'F'}, std::byte{'8'}};
            CHECK(exportFailure(document, path)
                      .message.find("image 'Packed' is embedded but holds neither PNG nor JPEG") != std::string::npos);
        }
        SUBCASE("an image with neither a file nor data") {
            document.images[0].uri.clear();
            CHECK(exportFailure(document, path).code == ErrorCode::InvalidArgument);
        }
        SUBCASE("a node whose mesh does not exist") {
            document.nodes[1].mesh = 9;
            CHECK(exportFailure(document, path).message.find("node 'Crate' refers to mesh 9") != std::string::npos);
        }
        SUBCASE("a child that does not exist") {
            document.nodes[0].children.push_back(40);
            CHECK(exportFailure(document, path).message.find("node 'Room' has the child 40") != std::string::npos);
        }
        SUBCASE("a node with two parents") {
            document.nodes[2].children.push_back(3);
            CHECK(exportFailure(document, path).message.find("node 'Lamp' has more than one parent") !=
                  std::string::npos);
        }
        SUBCASE("a node that is its own ancestor") {
            document.nodes[3].children.push_back(0);
            CHECK(exportFailure(document, path).message.find("is its own ancestor") != std::string::npos);
        }
        SUBCASE("a root that has a parent") {
            document.roots = {1};
            CHECK(exportFailure(document, path).message.find("root 1 is not a node without a parent") !=
                  std::string::npos);
        }
        SUBCASE("a root that does not exist") {
            document.roots = {12};
            CHECK(exportFailure(document, path).code == ErrorCode::InvalidArgument);
        }
        CHECK_FALSE(fileExists(directory.path() / "bad.bin"));
    }

    TEST_CASE("a JPEG image is recognized as one") {
        const TempDirectory directory;
        GltfDocument document;
        // The first bytes are enough for the exporter; nothing decodes the image here.
        document.images.push_back(
            {.name = "Photo", .encoded = {std::byte{0xFF}, std::byte{0xD8}, std::byte{0xFF}, std::byte{0xE0}}});
        const std::filesystem::path path = directory.path() / "photo.gltf";
        REQUIRE(exportGltf(document, path).has_value());
        CHECK(unwrap(readJsonFile(path))["images"][0]["mimeType"] == "image/jpeg");
        CHECK(unwrap(importGltf(path)).images[0].encoded.size() == 4);
    }
}
