#include "cordite/assets/FbxImporter.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/core/FileSystem.h"

#include "AssetTestSupport.h"
#include "GltfTestSupport.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;

namespace {

/// A square of 1 m in the XY plane, from the origin, with texture coordinates whose origin
/// is at the bottom, as OBJ has them; `material` is used for it, from `library`.
std::string squareObj(const std::string& library = "", const std::string& material = "") {
    std::string text;
    if (!library.empty()) {
        text += "mtllib " + library + "\n";
    }
    text += "o Square\n"
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
    if (!material.empty()) {
        text += "usemtl " + material + "\n";
    }
    text += "f 1/1 2/2 3/3 4/4\n";
    return text;
}

/// An ASCII FBX file of one triangle - (0, 0, 0), (1, 0, 0) and a third corner along the
/// up axis - in centimetres, under a node moved 2 cm up and scaled twice, with a material
/// of a diffuse color. `upAxis` is FBX's number of the axis that is up: 1 for Y, 2 for Z.
std::string triangleFbx(int upAxis) {
    const std::string third = upAxis == 1 ? "0,1,0" : "0,0,1";
    const std::string up = upAxis == 1 ? "0,2,0" : "0,0,2";
    return R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",)" +
           std::to_string(upAxis) + R"(
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",)" +
           std::to_string(upAxis == 1 ? 2 : 1) + R"(
		P: "FrontAxisSign", "int", "Integer", "",)" +
           std::to_string(upAxis == 1 ? 1 : -1) + R"(
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::Triangle", "Mesh" {
		Vertices: *9 {
			a: 0,0,0,1,0,0,)" +
           third + R"(
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::Triangle", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Translation", "Lcl Translation", "", "A",)" +
           up + R"(
			P: "Lcl Scaling", "Lcl Scaling", "", "A",2,2,2
		}
	}
	Material: 3000, "Material::Paint", "" {
		Version: 102
		ShadingModel: "phong"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.5,0.25,0.125
			P: "DiffuseFactor", "Number", "", "A",1
		}
	}
}
Connections:  {
	C: "OO",2000,0
	C: "OO",1000,2000
	C: "OO",3000,2000
}
)";
}

struct Directory {
    test::TempDirectory temp;

    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const std::filesystem::path path = temp.path() / pathFromUtf8(name);
        REQUIRE(writeTextFile(path, text).has_value());
        return path;
    }
};

GltfDocument importOrFail(const std::filesystem::path& path, const GltfImportOptions& options = {}) {
    Result<GltfDocument> document = importFbx(path, options);
    REQUIRE_MESSAGE(document.has_value(), test::errorOf(document));
    return std::move(*document);
}

/// The vertex of `mesh` at `position`.
const MeshVertex& vertexAt(const MeshData& mesh, const Vec3& position) {
    const auto found = std::ranges::find_if(
        mesh.vertices, [&](const MeshVertex& vertex) { return glm::length(vertex.position - position) < 1e-5f; });
    REQUIRE(found != mesh.vertices.end());
    return *found;
}

} // namespace

TEST_SUITE("FbxImporter") {
    TEST_CASE("an OBJ square becomes a node with a mesh of two triangles") {
        Directory directory;
        const GltfDocument document = importOrFail(directory.write("square.obj", squareObj()));
        REQUIRE(document.meshes.size() == 1);
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.triangleCount() == 2);
        CHECK(mesh.vertices.size() == 4);
        REQUIRE(mesh.submeshes.size() == 1);
        CHECK(mesh.submeshes[0].indexCount == 6);
        CHECK(mesh.bounds.min.x == doctest::Approx(0.0f));
        CHECK(mesh.bounds.max.x == doctest::Approx(1.0f));
        CHECK(mesh.bounds.max.y == doctest::Approx(1.0f));
        CHECK(validateMesh(mesh).has_value());
        REQUIRE(document.nodes.size() == 1);
        CHECK(document.nodes[0].mesh == 0u);
        CHECK(document.roots == std::vector<uint32_t>{0});
    }

    TEST_CASE("texture coordinates are turned to glTF's, whose origin is at the top of an image") {
        Directory directory;
        const GltfDocument document = importOrFail(directory.write("square.obj", squareObj()));
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(vertexAt(mesh, Vec3(0.0f, 0.0f, 0.0f)).uv0 == Vec2(0.0f, 1.0f));
        CHECK(vertexAt(mesh, Vec3(1.0f, 1.0f, 0.0f)).uv0 == Vec2(1.0f, 0.0f));
        // Normals are made where the file has none: the square faces +Z.
        CHECK(vertexAt(mesh, Vec3(1.0f, 0.0f, 0.0f)).normal.z == doctest::Approx(1.0f));
    }

    TEST_CASE("a material of an MTL file comes with its image, and both are listed as files") {
        Directory directory;
        directory.write("square.mtl", "newmtl Wall\nKd 0.8 0.2 0.1\nmap_Kd textures/wall.png\n");
        std::filesystem::create_directories(directory.temp.path() / "textures");
        test::writeSolidPng(directory.temp.path() / "textures" / "wall.png", Vec4(1.0f));
        const GltfDocument document = importOrFail(directory.write("square.obj", squareObj("square.mtl", "Wall")));

        REQUIRE(document.materials.size() == 1);
        const GltfMaterial& material = document.materials[0];
        CHECK(material.name == "Wall");
        CHECK(material.material.baseColor.r == doctest::Approx(0.8f));
        CHECK(material.material.baseColor.g == doctest::Approx(0.2f));
        CHECK(material.material.baseColor.b == doctest::Approx(0.1f));
        REQUIRE(material.images.baseColor.has_value());
        REQUIRE(document.images.size() == 1);
        CHECK(document.images[*material.images.baseColor].uri == "textures/wall.png");
        CHECK_FALSE(document.images[0].isEmbedded());
        CHECK(document.meshes[0].materials == std::vector<std::optional<uint32_t>>{0});
        CHECK(std::ranges::find(document.externalFiles, "square.mtl") != document.externalFiles.end());
        CHECK(std::ranges::find(document.externalFiles, "textures/wall.png") != document.externalFiles.end());
    }

    TEST_CASE("each material of a mesh is a submesh of its own") {
        Directory directory;
        directory.write("two.mtl", "newmtl Red\nKd 1 0 0\nnewmtl Blue\nKd 0 0 1\n");
        const GltfDocument document = importOrFail(
            directory.write("two.obj", "mtllib two.mtl\no Two\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\nv 2 1 0\n"
                                       "usemtl Red\nf 1 2 3 4\nusemtl Blue\nf 2 5 6 3\n"));
        REQUIRE(document.meshes.size() == 1);
        const GltfMesh& mesh = document.meshes[0];
        CHECK(mesh.mesh.submeshes.size() == 2);
        REQUIRE(mesh.materials.size() == 2);
        REQUIRE(mesh.materials[0].has_value());
        REQUIRE(mesh.materials[1].has_value());
        CHECK(document.materials[*mesh.materials[0]].name == "Red");
        CHECK(document.materials[*mesh.materials[1]].name == "Blue");
    }

    TEST_CASE("an MTL file outside the asset directory is not read") {
        Directory directory;
        const std::filesystem::path assets = directory.temp.path() / "assets";
        std::filesystem::create_directories(assets);
        directory.write("outside.mtl", "newmtl Wall\nKd 0 1 0\n");
        const GltfDocument document = importOrFail(
            directory.write("assets/square.obj", squareObj("../outside.mtl", "Wall")), {.rootDirectory = assets});
        CHECK(document.externalFiles.empty());
        // Not the green of the file outside.
        for (const GltfMaterial& material : document.materials) {
            CHECK(material.material.baseColor != Vec4(0.0f, 1.0f, 0.0f, 1.0f));
        }
    }

    TEST_CASE("images are named relative to the file, as glTF files name them") {
        Directory directory;
        directory.write("assets/square.mtl", "newmtl Wall\nKd 1 1 1\nmap_Kd ../textures/wall.png\n");
        const GltfDocument document =
            importOrFail(directory.write("assets/square.obj", squareObj("square.mtl", "Wall")),
                         {.rootDirectory = directory.temp.path() / "assets"});
        REQUIRE(document.images.size() == 1);
        CHECK(document.images[0].uri == "../textures/wall.png");
        CHECK(std::ranges::find(document.externalFiles, "../textures/wall.png") != document.externalFiles.end());
    }

    TEST_CASE("an image named only by where it was on another machine is not used") {
        Directory directory;
        directory.write("square.mtl", "newmtl Wall\nKd 1 1 1\nmap_Kd C:/Users/someone/textures/wall.png\n");
        const GltfDocument document = importOrFail(directory.write("square.obj", squareObj("square.mtl", "Wall")));
        CHECK(document.images.empty());
        REQUIRE(document.materials.size() == 1);
        CHECK_FALSE(document.materials[0].images.baseColor.has_value());
        CHECK(std::ranges::find(document.externalFiles, "square.mtl") != document.externalFiles.end());
        CHECK(document.externalFiles.size() == 1);
        CHECK_FALSE(document.warnings.empty());
    }

    TEST_CASE("an FBX file in centimetres comes in metres, placed as its node says") {
        Directory directory;
        const GltfDocument document = importOrFail(directory.write("triangle.fbx", triangleFbx(1)));
        REQUIRE(document.meshes.size() == 1);
        const MeshData& mesh = document.meshes[0].mesh;
        CHECK(mesh.triangleCount() == 1);
        CHECK(mesh.bounds.max.x == doctest::Approx(0.01f));
        CHECK(mesh.bounds.max.y == doctest::Approx(0.01f));

        REQUIRE(document.nodes.size() == 1);
        const GltfNode& node = document.nodes[0];
        CHECK(node.name == "Triangle");
        CHECK(node.mesh == 0u);
        CHECK(node.transform.position.y == doctest::Approx(0.02f));
        CHECK(node.transform.scale.x == doctest::Approx(2.0f));

        REQUIRE(document.materials.size() == 1);
        CHECK(document.materials[0].name == "Paint");
        CHECK(document.materials[0].material.baseColor.r == doctest::Approx(0.5f));
        CHECK(document.materials[0].material.baseColor.g == doctest::Approx(0.25f));
        CHECK(document.meshes[0].materials == std::vector<std::optional<uint32_t>>{0});
    }

    TEST_CASE("an FBX file with +Z up comes with +Y up") {
        Directory directory;
        const GltfDocument document = importOrFail(directory.write("triangle.fbx", triangleFbx(2)));
        REQUIRE(document.meshes.size() == 1);
        REQUIRE(document.nodes.size() == 1);
        // The corner that was up is up still: the triangle and its node are turned alike.
        const Transform& placement = document.nodes[0].transform;
        const Aabb placed = document.meshes[0].mesh.bounds.transformed(placement.toMatrix());
        CHECK(placed.max.y == doctest::Approx(0.04f).epsilon(0.001));
        CHECK(std::abs(placed.max.z) < 1e-4f);
    }

    TEST_CASE("without data, a file says what it holds without meshes to draw") {
        Directory directory;
        directory.write("square.mtl", "newmtl Wall\nKd 1 1 1\n");
        const GltfDocument document =
            importOrFail(directory.write("square.obj", squareObj("square.mtl", "Wall")), {.loadData = false});
        REQUIRE(document.meshes.size() == 1);
        CHECK(document.meshes[0].mesh.vertices.empty());
        CHECK(document.meshes[0].mesh.submeshes.size() == 1);
        CHECK(document.materials.size() == 1);
    }

    TEST_CASE("a file with more geometry than may be imported is refused") {
        Directory directory;
        const Result<GltfDocument> document = importFbx(directory.write("square.obj", squareObj()), {.maxVertices = 5});
        REQUIRE_FALSE(document.has_value());
        CHECK(document.error().code == ErrorCode::OutOfRange);
    }

    TEST_CASE("files that are not models are refused with a reason that names them") {
        Directory directory;
        SUBCASE("missing") {
            const Result<GltfDocument> document = importFbx(directory.temp.path() / "missing.fbx");
            REQUIRE_FALSE(document.has_value());
            CHECK(document.error().message.find("missing.fbx") != std::string::npos);
        }
        SUBCASE("another kind of file") {
            const Result<GltfDocument> document = importFbx(directory.write("notes.txt", "hello"));
            REQUIRE_FALSE(document.has_value());
            CHECK(document.error().code == ErrorCode::InvalidArgument);
        }
        SUBCASE("an FBX file of rubbish") {
            const Result<GltfDocument> document =
                importFbx(directory.write("rubbish.fbx", "Kaydara FBX Binary  \x01 rubbish"));
            REQUIRE_FALSE(document.has_value());
            CHECK(document.error().code == ErrorCode::ParseError);
            CHECK(document.error().message.find("rubbish.fbx") != std::string::npos);
        }
        SUBCASE("an FBX file cut short") {
            const std::string text = triangleFbx(1);
            const Result<GltfDocument> document =
                importFbx(directory.write("short.fbx", text.substr(0, text.size() / 2)));
            if (document) {
                CHECK(document->meshes.size() <= 1);
            }
        }
    }

    TEST_CASE("an empty OBJ file is a model of nothing") {
        Directory directory;
        const GltfDocument document = importOrFail(directory.write("empty.obj", ""));
        CHECK(document.meshes.empty());
        CHECK(document.nodes.empty());
    }

    TEST_CASE("model files of every kind are read by importModelFile") {
        Directory directory;
        CHECK(importModelFile(directory.write("square.obj", squareObj())).has_value());
        CHECK(importModelFile(directory.write("SQUARE.OBJ", squareObj())).has_value());
        CHECK(importModelFile(directory.write("triangle.fbx", triangleFbx(1))).has_value());
        test::GltfFile gltf;
        gltf.json["nodes"] = test::parseOrFail(R"([{"name": "Empty"}])");
        gltf.json["scenes"] = test::parseOrFail(R"([{"nodes": [0]}])");
        gltf.json["scene"] = 0;
        CHECK(importModelFile(gltf.write(directory.temp.path(), "empty")).has_value());
        const Result<GltfDocument> other = importModelFile(directory.write("notes.txt", "hello"));
        REQUIRE_FALSE(other.has_value());
        CHECK(other.error().code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("an OBJ model is an asset like a glTF model, and its image a texture asset") {
        Directory directory;
        directory.write("square.mtl", "newmtl Wall\nKd 1 1 1\nmap_Kd wall.png\n");
        test::writeSolidPng(directory.temp.path() / "wall.png", Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        directory.write("square.obj", squareObj("square.mtl", "Wall"));
        const std::unique_ptr<AssetManager> assets =
            test::unwrap(AssetManager::create({.rootDirectory = directory.temp.path()}));

        const AssetInfo* model = assets->database().findByReference("square.obj");
        REQUIRE(model != nullptr);
        CHECK(model->type == AssetType::Model);
        const std::shared_ptr<const ModelData> data = test::unwrap(assets->loadModel(model->id));
        REQUIRE(data->nodes.size() == 1);
        const std::shared_ptr<const MeshData> mesh = test::unwrap(assets->loadMesh(data->nodes[0].mesh));
        CHECK(mesh->triangleCount() == 2);
        REQUIRE(data->nodes[0].materials.size() == 1);
        const std::shared_ptr<const MaterialData> material =
            test::unwrap(assets->loadMaterial(data->nodes[0].materials[0]));
        const AssetInfo* texture = assets->database().findByReference("wall.png");
        REQUIRE(texture != nullptr);
        CHECK(material->baseColorTexture == texture->id);
    }
}
