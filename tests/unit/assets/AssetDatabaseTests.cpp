#include "cordite/assets/AssetDatabase.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Prefab.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/Json.h"

#include "AssetTestSupport.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// An asset directory in a temporary place, and the files tests put into it.
class AssetDirectory {
public:
    AssetDirectory() : m_root(m_directory.path() / "assets") { REQUIRE(ensureDirectory(m_root).has_value()); }

    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

    [[nodiscard]] std::filesystem::path file(std::string_view relativePath) const {
        return m_root / pathFromUtf8(relativePath);
    }

    void writeText(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(file(relativePath), text).has_value());
    }

    void writeTexture(std::string_view relativePath, const Vec4& color = Vec4(1.0f)) const {
        test::writeSolidPng(file(relativePath), color);
    }

    void writeMeta(std::string_view assetPath, std::string_view id) const {
        writeText(fmt::format("{}.meta", assetPath),
                  fmt::format(R"({{"format": "cordite.asset", "version": 1, "id": "{}"}})", id));
    }

    /// A model with one mesh (two submeshes), two materials, an image in a file of its
    /// own at `textureUri` (relative to the model) and one embedded image.
    void writeModel(std::string_view relativePath, std::string_view textureUri = "../textures/wall.png") const {
        GltfDocument document;
        document.images.push_back({.name = "Wall", .uri = std::string(textureUri)});
        document.images.push_back(
            {.name = "Label", .encoded = test::encodeSolidPng(2, 2, Vec4(0.0f, 1.0f, 0.0f, 1.0f))});

        GltfMaterial wood;
        wood.name = "Wood";
        wood.images.baseColor = 0;
        wood.images.normal = 1;
        document.materials.push_back(wood);
        GltfMaterial metal;
        metal.name = "Metal";
        metal.material.metallic = 1.0f;
        metal.images.emissive = 1;
        document.materials.push_back(metal);

        GltfMesh mesh;
        mesh.mesh.name = "Body";
        const MeshData box = makeBox();
        const MeshData knob = makeSphere(0.1f, 8, 4);
        appendSubmesh(mesh.mesh, box.vertices, box.indices);
        appendSubmesh(mesh.mesh, knob.vertices, knob.indices);
        mesh.materials = {0, 1};
        document.meshes.push_back(mesh);

        document.nodes.push_back({.name = "Crate", .mesh = 0});
        document.roots = {0};
        REQUIRE(exportGltf(document, file(relativePath)).has_value());
    }

    [[nodiscard]] AssetDatabase open(bool writeMetaFiles = false) const {
        AssetDatabase database({.rootDirectory = m_root, .writeMetaFiles = writeMetaFiles});
        REQUIRE(database.refresh().has_value());
        return database;
    }

private:
    TempDirectory m_directory;
    std::filesystem::path m_root;
};

/// The references of every asset that is not built in, in the order the database lists them.
std::vector<std::string> projectAssets(const AssetDatabase& database) {
    std::vector<std::string> references;
    for (const AssetInfo& asset : database.assets()) {
        if (!asset.isBuiltin()) {
            references.push_back(asset.reference());
        }
    }
    return references;
}

const AssetInfo& require(const AssetDatabase& database, std::string_view reference) {
    const AssetInfo* asset = database.findByReference(reference);
    REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
    return *asset;
}

bool hasProblem(const AssetDatabase& database, std::string_view fragment) {
    return std::ranges::any_of(database.problems(),
                               [&](const std::string& problem) { return problem.find(fragment) != std::string::npos; });
}

/// Stands in for what the engine reads scenes and prefabs with: the ids in a document's
/// "uses", or the failure its "fail" says. Remembers what it was asked.
struct FakeReader {
    std::shared_ptr<std::vector<std::pair<AssetType, Json>>> calls =
        std::make_shared<std::vector<std::pair<AssetType, Json>>>();

    [[nodiscard]] AssetReferenceReader reader() const {
        return [calls = calls](AssetType type, const Json& document) -> Result<std::vector<Uuid>> {
            calls->emplace_back(type, document);
            if (document.contains("fail")) {
                return makeError(ErrorCode::ParseError, "{}", document["fail"].get<std::string>());
            }
            std::vector<Uuid> ids;
            for (const Json& id : document.at("uses")) {
                ids.push_back(*Uuid::parse(id.get<std::string>()));
            }
            return ids;
        };
    }

    [[nodiscard]] AssetDatabase open(const std::filesystem::path& root) const {
        AssetDatabase database({.rootDirectory = root, .readReferences = reader()});
        REQUIRE(database.refresh().has_value());
        return database;
    }
};

/// A document the fake reader says uses `ids`.
std::string usesDocument(std::initializer_list<Uuid> ids) {
    Json uses = Json::array();
    for (const Uuid& id : ids) {
        uses.push_back(id.toString());
    }
    Json document = Json::object();
    document["uses"] = std::move(uses);
    return document.dump();
}

/// An id that no asset has.
const Uuid kMissingAsset = *Uuid::parse("11111111-1111-4111-8111-111111111111");

bool dependsOn(const AssetInfo& asset, const Uuid& id) {
    return std::ranges::find(asset.dependencies, id) != asset.dependencies.end();
}

} // namespace

TEST_SUITE("AssetDatabase") {
    TEST_CASE("a file is an asset by how its name ends, whatever the case, and not by that alone") {
        CHECK(assetTypeOfFile("prefabs/cart.prefab.json") == AssetType::Prefab);
        CHECK(assetTypeOfFile("cart.PREFAB.Json") == AssetType::Prefab);
        CHECK(assetTypeOfFile("levels/yard.scene.json") == AssetType::Scene);
        CHECK(assetTypeOfFile("materials/paint.material.json") == AssetType::Material);
        CHECK(assetTypeOfFile("models/crate.gltf") == AssetType::Model);
        CHECK(assetTypeOfFile("models/crate.GLB") == AssetType::Model);
        CHECK(assetTypeOfFile("a.png") == AssetType::Texture);
        CHECK(assetTypeOfFile("scripts/door.lua") == AssetType::Script);
        CHECK(assetTypeOfFile("sounds/hop.flac") == AssetType::Sound);

        // No asset: another ending, none, some JSON, half an ending, what describes an asset.
        for (const char* path :
             {"notes.txt", "prefabs/cart", "settings.json", "prefabs/cart.prefab", "a.png.meta", ""}) {
            CAPTURE(std::string(path));
            CHECK_FALSE(assetTypeOfFile(path).has_value());
        }
        // Nor a file that is called nothing but such an ending, wherever it is.
        for (const char* path : {".png", "textures/.png", "prefabs/.prefab.json", "a.b/.lua"}) {
            CAPTURE(std::string(path));
            CHECK_FALSE(assetTypeOfFile(path).has_value());
        }
        // A directory may be called anything.
        CHECK(assetTypeOfFile("old.png/wall.tga") == AssetType::Texture);
        CHECK_FALSE(assetTypeOfFile("old.png/notes").has_value());
    }

    TEST_CASE("the names of asset types") {
        CHECK(nameOf(AssetType::Texture) == "texture");
        CHECK(nameOf(AssetType::Mesh) == "mesh");
        CHECK(nameOf(AssetType::Material) == "material");
        CHECK(nameOf(AssetType::Model) == "model");
        CHECK(nameOf(AssetType::Scene) == "scene");
        CHECK(nameOf(AssetType::Skeleton) == "skeleton");
        CHECK(nameOf(AssetType::Animation) == "animation");
        CHECK(nameOf(AssetType::Script) == "script");
        CHECK(nameOf(AssetType::Sound) == "sound");
        CHECK(nameOf(AssetType::Prefab) == "prefab");
        CHECK(displayNameOf(AssetType::Texture) == "Texture");
        CHECK(displayNameOf(AssetType::Mesh) == "Mesh");
        CHECK(displayNameOf(AssetType::Skeleton) == "Skeleton");
        CHECK(displayNameOf(AssetType::Animation) == "Animation");
        CHECK(displayNameOf(AssetType::Script) == "Script");
        CHECK(displayNameOf(AssetType::Sound) == "Sound");
        CHECK(displayNameOf(AssetType::Prefab) == "Prefab");
        // In a sentence a type takes the article its name asks for.
        CHECK(withArticle(AssetType::Texture) == "a texture");
        CHECK(withArticle(AssetType::Mesh) == "a mesh");
        CHECK(withArticle(AssetType::Material) == "a material");
        CHECK(withArticle(AssetType::Model) == "a model");
        CHECK(withArticle(AssetType::Scene) == "a scene");
        CHECK(withArticle(AssetType::Skeleton) == "a skeleton");
        CHECK(withArticle(AssetType::Animation) == "an animation");
        CHECK(withArticle(AssetType::Script) == "a script");
        CHECK(withArticle(AssetType::Sound) == "a sound");
        CHECK(withArticle(AssetType::Prefab) == "a prefab");
        // Every type is found by its name, and has a name of its own.
        CHECK(kAssetTypes.size() == 10);
        for (const AssetType type : kAssetTypes) {
            CHECK(assetTypeFromName(nameOf(type)) == type);
            CHECK(nameOf(type) != "unknown");
            CHECK(displayNameOf(type) != "Unknown");
            // The name is the last word of the name with its article.
            CHECK(withArticle(type).ends_with(std::string(" ") + std::string(nameOf(type))));
        }
        CHECK(assetTypeFromName("Skeleton") == AssetType::Skeleton);

        CHECK(assetTypeFromName("mesh") == AssetType::Mesh);
        CHECK(assetTypeFromName("Material") == AssetType::Material);
        CHECK(assetTypeFromName("SCENE") == AssetType::Scene);
        CHECK(assetTypeFromName("Sound") == AssetType::Sound);
        CHECK_FALSE(assetTypeFromName("font").has_value());
        CHECK_FALSE(assetTypeFromName("sounds").has_value());
        CHECK_FALSE(assetTypeFromName("").has_value());
    }

    TEST_CASE("every database has the built-in assets, under ids that never change") {
        const AssetDatabase database;
        REQUIRE(database.assets().size() == kBuiltinMeshes.size() + 1);
        CHECK(database.problems().empty());

        // Scenes refer to these by id. Changing one breaks every scene that uses it.
        CHECK(builtinMeshId(BuiltinMesh::Cube).toString() == "0a56c2f1-21e0-819d-8bac-567d1e1c4345");
        CHECK(builtinMeshId(BuiltinMesh::Sphere).toString() == "aa35bb57-6b9a-8ecb-9eda-83a2a1be3014");
        CHECK(builtinMeshId(BuiltinMesh::Plane).toString() == "8db36b6b-2606-8b2d-b7f0-6bd7af0a9d8c");
        CHECK(builtinMeshId(BuiltinMesh::Cylinder).toString() == "2c51a473-c570-8ef1-a45b-3b3ba054e8e5");
        CHECK(builtinMeshId(BuiltinMesh::Cone).toString() == "da2fbb43-a0e2-821e-af4c-1593d5470916");
        CHECK(builtinMeshId(BuiltinMesh::Capsule).toString() == "6bb53bca-6ca5-84c3-8341-8ac61212f240");
        CHECK(builtinMeshId(BuiltinMesh::Torus).toString() == "db82bc76-e6a3-8825-9377-4d3b4224e8e2");
        CHECK(defaultMaterialId().toString() == "ac42fa5f-d2ce-8a5f-a3ed-eb69eca3d3b9");

        const AssetInfo* cube = database.find(builtinMeshId(BuiltinMesh::Cube));
        REQUIRE(cube != nullptr);
        CHECK(cube->type == AssetType::Mesh);
        CHECK(cube->name == "Cube");
        CHECK(cube->isBuiltin());
        CHECK(cube->reference() == "builtin#mesh/cube");
        CHECK(database.findByReference("builtin#mesh/cube") == cube);
        CHECK(database.sourceFile(*cube).empty());

        const AssetInfo* material = database.find(defaultMaterialId());
        REQUIRE(material != nullptr);
        CHECK(material->type == AssetType::Material);
        CHECK(material->reference() == "builtin#material/default");

        for (const BuiltinMesh mesh : kBuiltinMeshes) {
            CAPTURE(nameOf(mesh));
            const AssetInfo* asset = database.find(builtinMeshId(mesh));
            REQUIRE(asset != nullptr);
            CHECK(asset->reference() == fmt::format("builtin#mesh/{}", nameOf(mesh)));
        }
    }

    TEST_CASE("a database without a directory refreshes to the built-in assets") {
        AssetDatabase database;
        const uint64_t before = database.revision();
        CHECK(database.refresh().has_value());
        CHECK(database.assets().size() == kBuiltinMeshes.size() + 1);
        CHECK(database.revision() != before);
    }

    TEST_CASE("a directory that does not exist is an error") {
        const TempDirectory directory;
        AssetDatabase database({.rootDirectory = directory.path() / "nowhere"});
        const Status status = database.refresh();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::NotFound);
        // The built-in assets are still there.
        CHECK(database.find(builtinMeshId(BuiltinMesh::Cube)) != nullptr);
    }

    TEST_CASE("files are recognized by their extension, in the order of their paths") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeTexture("textures/Sky.HDR.PNG");
        project.writeTexture("a.jpg");
        project.writeText("materials/paint.material.json", R"({"format": "cordite.material", "version": 1})");
        project.writeText("scenes/main.scene.json", "{}");
        project.writeText("prefabs/cart.prefab.json", "{}");
        project.writeText("prefabs/Old.Cart.PREFAB.JSON", "not even JSON, and nobody has asked yet");
        project.writeText("scripts/door.lua", "return {}");
        project.writeText("scripts/Open.Slowly.LUA", "this is not Lua, and nobody has asked yet");
        project.writeText("sounds/click.wav", "this is no sound either");
        project.writeText("sounds/Steps.FLAC", "");
        project.writeText("sounds/theme.mp3", "");
        project.writeModel("models/crate.gltf");
        // None of these is an asset.
        project.writeText("scripts/door.lua.bak", "return {}");
        project.writeText("scripts/notes.luau", "");
        project.writeText("sounds/click.wave", "");
        project.writeText("sounds/click.wav.txt", "");
        project.writeText("sounds/theme.ogg", "a kind of sound file that is not read");
        project.writeText("notes.txt", "remember the milk");
        project.writeText("settings.json", "{}");
        project.writeText("prefabs/cart.prefab", "{}");
        project.writeText("prefabs/prefab.json", "{}");
        // A file that is called nothing but what makes an asset of others, here and at the top.
        project.writeText("prefabs/.prefab.json", "{}");
        project.writeText("textures/.png", "");
        project.writeText(".lua", "");
        project.writeText("models/crate.blend", "");
        project.writeText(".png", "a file that is nothing but an extension");
        project.writeTexture(".cache/thumbnail.png");
        project.writeTexture("textures/.hidden/secret.png");

        const AssetDatabase database = project.open();
        CHECK(projectAssets(database) == std::vector<std::string>{
                                             "a.jpg",
                                             "materials/paint.material.json",
                                             "models/crate.gltf",
                                             "models/crate.gltf#mesh/0",
                                             "models/crate.gltf#material/0",
                                             "models/crate.gltf#material/1",
                                             "models/crate.gltf#image/1",
                                             "prefabs/Old.Cart.PREFAB.JSON",
                                             "prefabs/cart.prefab.json",
                                             "scenes/main.scene.json",
                                             "scripts/Open.Slowly.LUA",
                                             "scripts/door.lua",
                                             "sounds/Steps.FLAC",
                                             "sounds/click.wav",
                                             "sounds/theme.mp3",
                                             "textures/Sky.HDR.PNG",
                                             "textures/wall.png",
                                         });
        CHECK(database.problems().empty());

        CHECK(require(database, "a.jpg").type == AssetType::Texture);
        CHECK(require(database, "a.jpg").name == "a");
        CHECK(require(database, "materials/paint.material.json").type == AssetType::Material);
        CHECK(require(database, "materials/paint.material.json").name == "paint");
        CHECK(require(database, "scenes/main.scene.json").type == AssetType::Scene);
        CHECK(require(database, "scenes/main.scene.json").name == "main");
        // A prefab is a file of its own kind, not some .json: its name is what is before both.
        CHECK(require(database, "prefabs/cart.prefab.json").type == AssetType::Prefab);
        CHECK(require(database, "prefabs/cart.prefab.json").name == "cart");
        CHECK(require(database, "prefabs/cart.prefab.json").dependencies.empty());
        CHECK(require(database, "prefabs/Old.Cart.PREFAB.JSON").type == AssetType::Prefab);
        CHECK(require(database, "prefabs/Old.Cart.PREFAB.JSON").name == "Old.Cart");
        CHECK(require(database, "models/crate.gltf").type == AssetType::Model);
        CHECK(require(database, "models/crate.gltf").name == "crate");
        // A script is its text; whether that is Lua is for whoever runs it to find out.
        CHECK(require(database, "scripts/door.lua").type == AssetType::Script);
        CHECK(require(database, "scripts/door.lua").name == "door");
        CHECK(require(database, "scripts/door.lua").dependencies.empty());
        CHECK(require(database, "scripts/Open.Slowly.LUA").type == AssetType::Script);
        CHECK(require(database, "scripts/Open.Slowly.LUA").name == "Open.Slowly");
        // And a sound is what a file of three kinds holds; nothing is decoded to list it.
        CHECK(require(database, "sounds/click.wav").type == AssetType::Sound);
        CHECK(require(database, "sounds/click.wav").name == "click");
        CHECK(require(database, "sounds/click.wav").dependencies.empty());
        CHECK(require(database, "sounds/Steps.FLAC").type == AssetType::Sound);
        CHECK(require(database, "sounds/Steps.FLAC").name == "Steps");
        CHECK(require(database, "sounds/theme.mp3").type == AssetType::Sound);
        // Only the last extension is the type; what is before it is part of the name.
        CHECK(require(database, "textures/Sky.HDR.PNG").type == AssetType::Texture);
        CHECK(require(database, "textures/Sky.HDR.PNG").name == "Sky.HDR");

        const AssetInfo& wall = require(database, "textures/wall.png");
        CHECK(database.sourceFile(wall) == project.file("textures/wall.png"));
        CHECK(database.find(wall.id) == &wall);
        CHECK(wall.subPath.empty());
        CHECK_FALSE(wall.isBuiltin());
    }

    TEST_CASE("the ending of an asset file says its kind, and its .meta is beside it") {
        CHECK(assetFileExtension("paint.Material.JSON") == ".material.json");
        CHECK(assetFileExtension("models/crate.GLTF") == ".gltf");
        CHECK(assetFileExtension("wall.png") == ".png");
        CHECK(assetFileExtension("notes.json").empty());
        CHECK(assetFileExtension(".png").empty());
        CHECK(assetMetaPath(pathFromUtf8("models/wall.png")) == pathFromUtf8("models/wall.png.meta"));

        // What an editor writes for a file that has none is what the database reads.
        const AssetDirectory project;
        project.writeTexture("wall.png");
        const Uuid id = *Uuid::parse("22222222-2222-4222-8222-222222222222");
        REQUIRE(writeAssetMeta(project.file("wall.png"), id).has_value());
        CHECK(require(project.open(), "wall.png").id == id);
    }

    TEST_CASE("references distinguish upper and lower case, and unknown ones are not found") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        const AssetDatabase database = project.open();

        CHECK(database.findByReference("textures/wall.png") != nullptr);
        CHECK(database.findByReference("textures/Wall.png") == nullptr);
        CHECK(database.findByReference("Textures/wall.png") == nullptr);
        CHECK(database.findByReference("textures/wall.png#mesh/0") == nullptr);
        CHECK(database.findByReference("") == nullptr);
        CHECK(database.find(Uuid()) == nullptr);
        CHECK(database.find(Uuid::generate()) == nullptr);
    }

    TEST_CASE("without .meta files, ids are derived from paths and nothing is written") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeTexture("textures/floor.png");

        const AssetDatabase first = project.open();
        const AssetDatabase second = project.open();
        const Uuid wall = require(first, "textures/wall.png").id;
        const Uuid floor = require(first, "textures/floor.png").id;

        CHECK_FALSE(wall.isNil());
        CHECK(wall != floor);
        CHECK(require(second, "textures/wall.png").id == wall);
        // The same path gives the same id in every project, on every machine.
        CHECK(wall.toString() == "4490264e-e5b1-818f-8f93-a383bf880ee3");

        CHECK_FALSE(fileExists(project.file("textures/wall.png.meta")));
        CHECK(first.problems().empty());
    }

    TEST_CASE("an editor writes a .meta file for every asset, with the id it would have had anyway") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeModel("models/crate.gltf");
        const Uuid derived = require(project.open(), "textures/wall.png").id;

        const AssetDatabase database = project.open(true);
        CHECK(require(database, "textures/wall.png").id == derived);
        CHECK(database.problems().empty());

        const std::string meta = unwrap(readTextFile(project.file("textures/wall.png.meta")));
        CHECK(meta ==
              fmt::format("{{\n    \"format\": \"cordite.asset\",\n    \"version\": 1,\n    \"id\": \"{}\"\n}}\n",
                          derived.toString()));
        CHECK(fileExists(project.file("models/crate.gltf.meta")));
        // Things inside a file have no description of their own; neither does the buffer.
        CHECK_FALSE(fileExists(project.file("models/crate.bin.meta")));

        // Reading again changes nothing, with or without permission to write.
        const std::string before = meta;
        CHECK(require(project.open(true), "textures/wall.png").id == derived);
        CHECK(require(project.open(false), "textures/wall.png").id == derived);
        CHECK(unwrap(readTextFile(project.file("textures/wall.png.meta"))) == before);
        CHECK(project.open(true).problems().empty());
    }

    TEST_CASE("an asset keeps its id when it is moved together with its .meta file") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        const Uuid id = require(project.open(true), "textures/wall.png").id;

        REQUIRE(ensureDirectory(project.file("old")).has_value());
        std::filesystem::rename(project.file("textures/wall.png"), project.file("old/bricks.png"));
        std::filesystem::rename(project.file("textures/wall.png.meta"), project.file("old/bricks.png.meta"));

        const AssetDatabase database = project.open();
        CHECK(database.findByReference("textures/wall.png") == nullptr);
        const AssetInfo& moved = require(database, "old/bricks.png");
        CHECK(moved.id == id);
        CHECK(moved.name == "bricks");
        CHECK(database.find(id) == &moved);
    }

    TEST_CASE("a new file at the old path of a moved asset gets an id of its own") {
        const AssetDirectory project;
        project.writeTexture("a/wall.png");
        const Uuid original = require(project.open(true), "a/wall.png").id;

        // The asset moves to a path that sorts before its old one, and keeps its id.
        std::filesystem::rename(project.file("a/wall.png"), project.file("a/moved.png"));
        std::filesystem::rename(project.file("a/wall.png.meta"), project.file("a/moved.png.meta"));
        // Something else appears where it used to be. Its path would derive the same id.
        project.writeTexture("a/wall.png", Vec4(0.0f, 0.0f, 1.0f, 1.0f));

        SUBCASE("read-only: a temporary id, and a note about it") {
            const AssetDatabase database = project.open(false);
            CHECK(require(database, "a/moved.png").id == original);
            CHECK(require(database, "a/wall.png").id != original);
            CHECK(hasProblem(database, "a/wall.png: it has no .meta file and gets a temporary id"));
            CHECK_FALSE(fileExists(project.file("a/wall.png.meta")));
        }
        SUBCASE("in an editor: a permanent id") {
            const AssetDatabase database = project.open(true);
            const Uuid fresh = require(database, "a/wall.png").id;
            CHECK(fresh != original);
            CHECK(database.problems().empty());
            CHECK(require(project.open(false), "a/wall.png").id == fresh);
        }
    }

    TEST_CASE("an asset copied together with its .meta file does not get to share the id") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        const Uuid original = require(project.open(true), "wall.png").id;
        std::filesystem::copy_file(project.file("wall.png"), project.file("wall copy.png"));
        std::filesystem::copy_file(project.file("wall.png.meta"), project.file("wall copy.png.meta"));

        SUBCASE("read-only: the copy is ignored") {
            const LogCapture log;
            const AssetDatabase database = project.open(false);
            // "wall copy.png" sorts first, so it is the one that keeps the id.
            CHECK(require(database, "wall copy.png").id == original);
            CHECK(database.findByReference("wall.png") == nullptr);
            CHECK(hasProblem(database, "wall.png: it has the same id as wall copy.png and is ignored"));
            CHECK(log.contains("is ignored"));
        }
        SUBCASE("in an editor: the copy gets a new id, written to its .meta") {
            const AssetDatabase database = project.open(true);
            const Uuid kept = require(database, "wall copy.png").id;
            const Uuid fresh = require(database, "wall.png").id;
            CHECK(kept == original);
            CHECK(fresh != original);
            CHECK(hasProblem(database, "wall.png: it had the same id as wall copy.png and was given a new one"));

            // From now on there is nothing left to resolve.
            const AssetDatabase again = project.open(true);
            CHECK(again.problems().empty());
            CHECK(require(again, "wall.png").id == fresh);
        }
    }

    TEST_CASE("an unreadable .meta file takes its asset out of the project, and is not overwritten") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        project.writeTexture("floor.png");

        std::string broken;
        std::string expected;
        SUBCASE("not JSON") {
            broken = "<<<<<<< HEAD";
            expected = "wall.png.meta: ";
        }
        SUBCASE("another kind of document") {
            broken = R"({"format": "cordite.scene", "version": 1, "id": "11111111-1111-4111-8111-111111111111"})";
            expected = "wall.png.meta: not a Cordite asset description";
        }
        SUBCASE("a newer version") {
            broken = R"({"format": "cordite.asset", "version": 2, "id": "11111111-1111-4111-8111-111111111111"})";
            expected = "wall.png.meta: the asset description has format version 2";
        }
        SUBCASE("no id") {
            broken = R"({"format": "cordite.asset", "version": 1})";
            expected = "wall.png.meta: missing required field 'id'";
        }
        SUBCASE("an id that is not one") {
            broken = R"({"format": "cordite.asset", "version": 1, "id": "wall"})";
            expected = "wall.png.meta: 'wall' is not a valid asset id";
        }
        SUBCASE("the nil id") {
            broken = R"({"format": "cordite.asset", "version": 1, "id": "00000000-0000-0000-0000-000000000000"})";
            expected = "is not a valid asset id";
        }
        SUBCASE("something that does not belong there") {
            broken =
                R"({"format": "cordite.asset", "version": 1, "id": "11111111-1111-4111-8111-111111111111", "x": 1})";
            expected = "wall.png.meta: unknown member 'x'";
        }
        project.writeText("wall.png.meta", broken);

        // Even an editor leaves the file alone: a new id would break every reference.
        const AssetDatabase database = project.open(true);
        CHECK(database.findByReference("wall.png") == nullptr);
        CHECK(database.findByReference("floor.png") != nullptr);
        CHECK(hasProblem(database, expected));
        CHECK(hasProblem(database, "the asset is ignored until this is fixed"));
        CHECK(unwrap(readTextFile(project.file("wall.png.meta"))) == broken);
    }

    TEST_CASE("a .meta file without its asset is reported") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        project.writeMeta("deleted.png", "11111111-1111-4111-8111-111111111111");
        project.writeMeta("notes.txt", "22222222-2222-4222-8222-222222222222");

        const AssetDatabase database = project.open();
        CHECK(hasProblem(database, "deleted.png.meta: there is no asset file for this description"));
        CHECK(hasProblem(database, "notes.txt.meta: there is no asset file for this description"));
        CHECK(database.problems().size() == 2);
    }

    TEST_CASE("an id stored in a .meta file is the asset's id") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        project.writeMeta("wall.png", "ABCDEF01-2345-4789-8ABC-DEF012345678");
        const AssetDatabase database = project.open();
        CHECK(require(database, "wall.png").id.toString() == "abcdef01-2345-4789-8abc-def012345678");
    }

    TEST_CASE("what a model contains is listed with ids derived from the model's") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeModel("models/crate.gltf");
        const AssetDatabase database = project.open();
        CHECK(database.problems().empty());

        const AssetInfo& model = require(database, "models/crate.gltf");
        const AssetInfo& mesh = require(database, "models/crate.gltf#mesh/0");
        const AssetInfo& wood = require(database, "models/crate.gltf#material/0");
        const AssetInfo& metal = require(database, "models/crate.gltf#material/1");
        const AssetInfo& label = require(database, "models/crate.gltf#image/1");
        const AssetInfo& wall = require(database, "textures/wall.png");

        CHECK(mesh.type == AssetType::Mesh);
        CHECK(mesh.name == "Body");
        CHECK(mesh.path == "models/crate.gltf");
        CHECK(mesh.subPath == "mesh/0");
        CHECK(mesh.id == subAssetId(model.id, "mesh/0"));
        CHECK(database.sourceFile(mesh) == project.file("models/crate.gltf"));

        CHECK(wood.type == AssetType::Material);
        CHECK(wood.name == "Wood");
        CHECK(wood.id == subAssetId(model.id, "material/0"));
        CHECK(metal.name == "Metal");
        CHECK(label.type == AssetType::Texture);
        CHECK(label.name == "Label");
        CHECK(label.id == subAssetId(model.id, "image/1"));
        // The image that is a file of its own is that file's asset, not one of the model's.
        CHECK(database.findByReference("models/crate.gltf#image/0") == nullptr);

        // A model needs its meshes and materials; a material needs its textures.
        CHECK(model.dependencies == std::vector<Uuid>{mesh.id, wood.id, metal.id});
        CHECK(wood.dependencies.size() == 2);
        CHECK(dependsOn(wood, wall.id));
        CHECK(dependsOn(wood, label.id));
        CHECK(metal.dependencies == std::vector<Uuid>{label.id});
        CHECK(mesh.dependencies.empty());
    }

    TEST_CASE("the ids of a model's contents follow the model, not its path") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeModel("models/crate.gltf", "../textures/wall.png");
        const AssetDatabase before = project.open(true);
        const Uuid mesh = require(before, "models/crate.gltf#mesh/0").id;

        // Renamed in place, so that its reference to the texture still holds.
        for (const char* extension : {".gltf", ".gltf.meta"}) {
            std::filesystem::rename(project.file(fmt::format("models/crate{}", extension)),
                                    project.file(fmt::format("models/box{}", extension)));
        }
        const AssetDatabase after = project.open();
        CHECK(require(after, "models/box.gltf#mesh/0").id == mesh);
        CHECK(after.problems().empty());
    }

    TEST_CASE("a model that refers to a texture which is not an asset is reported") {
        const AssetDirectory project;
        SUBCASE("a file that does not exist") {
            project.writeModel("models/crate.gltf", "../textures/missing.png");
            const AssetDatabase database = project.open();
            CHECK(hasProblem(database, "models/crate.gltf#material/0: the image '../textures/missing.png' of material "
                                       "'Wood' is not a texture in the asset directory"));
            // The material is still there, with the textures that do exist.
            CHECK(require(database, "models/crate.gltf#material/0").dependencies.size() == 1);
        }
        SUBCASE("a file outside the asset directory") {
            project.writeModel("crate.gltf", "../../elsewhere.png");
            CHECK(hasProblem(project.open(), "the image '../../elsewhere.png' of material 'Wood' is not a texture"));
        }
        SUBCASE("a file whose name differs in case") {
            project.writeTexture("textures/wall.png");
            project.writeModel("models/crate.gltf", "../textures/Wall.png");
            CHECK(hasProblem(project.open(), "the image '../textures/Wall.png' of material 'Wood' is not a texture"));
        }
        SUBCASE("something that is an asset, but not a texture") {
            project.writeText("textures/wall.material.json", R"({"format": "cordite.material", "version": 1})");
            project.writeModel("models/crate.gltf", "../textures/wall.material.json");
            CHECK(hasProblem(project.open(), "is not a texture in the asset directory"));
        }
    }

    TEST_CASE("a model that cannot be read is listed alone, with the reason") {
        const AssetDirectory project;
        project.writeText("models/broken.gltf", "this is not a glTF file, whatever its name says");
        project.writeTexture("wall.png");

        const AssetDatabase database = project.open();
        CHECK(projectAssets(database) == std::vector<std::string>{"models/broken.gltf", "wall.png"});
        CHECK(hasProblem(database, "models/broken.gltf"));
        CHECK(require(database, "models/broken.gltf").dependencies.empty());
    }

    TEST_CASE("what the importer leaves out of a model is noted on the model") {
        const AssetDirectory project;
        project.writeText("lines.gltf", R"({
            "asset": {"version": "2.0"},
            "buffers": [{"byteLength": 36, "uri": "lines.bin"}],
            "bufferViews": [{"buffer": 0, "byteLength": 36}],
            "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],
            "meshes": [{"name": "Wire", "primitives": [{"attributes": {"POSITION": 0}, "mode": 1}]}]
        })");
        const AssetDatabase database = project.open();
        const AssetInfo& model = require(database, "lines.gltf");
        REQUIRE(model.warnings.size() == 1);
        CHECK(model.warnings[0] == "mesh 'Wire' contains points or lines, which are not imported");
        // Nothing of the mesh can be drawn, so it is not an asset.
        CHECK(database.findByReference("lines.gltf#mesh/0") == nullptr);
        // Not a problem with the project: the file is fine, it just holds more than is used.
        CHECK(database.problems().empty());
    }

    TEST_CASE("a material file depends on its parent and on the textures it names") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        project.writeText("base.material.json", R"({"format": "cordite.material", "version": 1, "roughness": 0.9})");
        const AssetDatabase first = project.open(true);
        const Uuid wall = require(first, "wall.png").id;
        const Uuid base = require(first, "base.material.json").id;

        project.writeText("paint.material.json", fmt::format(R"({{
            "format": "cordite.material",
            "version": 1,
            "parent": "{}",
            "baseColorTexture": "{}",
            "normalTexture": "{}"
        }})",
                                                             base.toString(), wall.toString(), wall.toString()));

        const AssetDatabase database = project.open(true);
        CHECK(database.problems().empty());
        CHECK(require(database, "paint.material.json").dependencies == std::vector<Uuid>{base, wall});
        CHECK(require(database, "base.material.json").dependencies.empty());
    }

    TEST_CASE("references that lead nowhere are reported") {
        const AssetDirectory project;
        project.writeText("paint.material.json", R"({
            "format": "cordite.material",
            "version": 1,
            "baseColorTexture": "11111111-1111-4111-8111-111111111111"
        })");
        const AssetDatabase database = project.open();
        CHECK(hasProblem(database, "paint.material.json: it refers to the asset 11111111-1111-4111-8111-111111111111, "
                                   "which does not exist"));
        // The reference is kept: it is still what the file says.
        CHECK(require(database, "paint.material.json").dependencies.size() == 1);
    }

    TEST_CASE("a material file that cannot be read is listed, with the reason") {
        const AssetDirectory project;
        project.writeText("typo.material.json", R"({"format": "cordite.material", "version": 1, "rougness": 1})");
        project.writeText("junk.material.json", "not json");

        const AssetDatabase database = project.open();
        CHECK(database.findByReference("typo.material.json") != nullptr);
        CHECK(database.findByReference("junk.material.json") != nullptr);
        CHECK(hasProblem(database, "typo.material.json: "));
        CHECK(hasProblem(database, "rougness"));
        CHECK(hasProblem(database, "junk.material.json: "));
    }

    TEST_CASE("refreshing picks up what changed on disk") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        AssetDatabase database = project.open();
        const uint64_t revision = database.revision();
        const Uuid wall = require(database, "wall.png").id;

        project.writeTexture("floor.png");
        std::filesystem::remove(project.file("wall.png"));
        REQUIRE(database.refresh().has_value());

        CHECK(database.revision() != revision);
        CHECK(database.findByReference("wall.png") == nullptr);
        CHECK(database.find(wall) == nullptr);
        CHECK(database.findByReference("floor.png") != nullptr);
        CHECK(database.find(builtinMeshId(BuiltinMesh::Cube)) != nullptr);
    }

    TEST_CASE("problems are those of the last refresh only") {
        const AssetDirectory project;
        project.writeMeta("gone.png", "11111111-1111-4111-8111-111111111111");
        AssetDatabase database = project.open();
        CHECK(database.problems().size() == 1);

        std::filesystem::remove(project.file("gone.png.meta"));
        REQUIRE(database.refresh().has_value());
        CHECK(database.problems().empty());
    }

    TEST_CASE("paths in a model are resolved against the model's own directory") {
        CHECK(resolveAssetReference("models/crate.gltf", "wall.png") == "models/wall.png");
        CHECK(resolveAssetReference("models/crate.gltf", "../textures/wall.png") == "textures/wall.png");
        CHECK(resolveAssetReference("models/crate.gltf", "./maps/../maps/wall.png") == "models/maps/wall.png");
        CHECK(resolveAssetReference("crate.gltf", "textures/wall.png") == "textures/wall.png");
        CHECK(resolveAssetReference("a/b/c.gltf", "../../x/./y.png") == "x/y.png");
        CHECK(resolveAssetReference("a/b/c.gltf", "..//d.png") == "a/d.png");

        // Nothing outside the asset directory, however it is spelled.
        CHECK(resolveAssetReference("crate.gltf", "../wall.png").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "../../wall.png").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "/etc/passwd").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "C:/Windows/wall.png").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "..\\wall.png").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "https://example.com/wall.png").empty());
        CHECK(resolveAssetReference("models/crate.gltf", "").empty());
    }

    TEST_CASE("ids of things inside a file depend on the file and on what they are") {
        const Uuid first = *Uuid::parse("11111111-1111-4111-8111-111111111111");
        const Uuid second = *Uuid::parse("22222222-2222-4222-8222-222222222222");
        CHECK(subAssetId(first, "mesh/0") == subAssetId(first, "mesh/0"));
        CHECK(subAssetId(first, "mesh/0") != subAssetId(first, "mesh/1"));
        CHECK(subAssetId(first, "mesh/0") != subAssetId(first, "material/0"));
        CHECK(subAssetId(first, "mesh/0") != subAssetId(second, "mesh/0"));
        // Stored in scenes, so it must never change.
        CHECK(subAssetId(first, "mesh/0").toString() == "7c87221f-1dd6-8d48-93da-7d5594886ee3");
    }
    TEST_CASE("scenes and prefabs depend on what the reader they are read with finds in them") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeText("materials/paint.material.json", R"({"format": "cordite.material", "version": 1})");
        project.writeText("scripts/door.lua", "return {}\n");
        const AssetDatabase plain = project.open();
        const Uuid wall = require(plain, "textures/wall.png").id;
        const Uuid paint = require(plain, "materials/paint.material.json").id;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        project.writeText("scenes/yard.scene.json", usesDocument({paint, wall, cube}));
        project.writeText("prefabs/crate.prefab.json", usesDocument({paint}));

        const FakeReader fake;
        const AssetDatabase database = fake.open(project.root());
        CHECK(database.problems().empty());
        CHECK(require(database, "scenes/yard.scene.json").dependencies == std::vector<Uuid>{paint, wall, cube});
        CHECK(require(database, "prefabs/crate.prefab.json").dependencies == std::vector<Uuid>{paint});
        // Asked about scenes and prefabs only, in the order of their paths, with what their files hold.
        REQUIRE(fake.calls->size() == 2);
        CHECK(fake.calls->at(0).first == AssetType::Prefab);
        CHECK(fake.calls->at(0).second == unwrap(parseJson(usesDocument({paint}))));
        CHECK(fake.calls->at(1).first == AssetType::Scene);
        CHECK(require(database, "scripts/door.lua").dependencies.empty());

        // Without a reader, they need nothing that can be told.
        const AssetDatabase without = project.open();
        CHECK(require(without, "scenes/yard.scene.json").dependencies.empty());
        CHECK(without.dependents(paint).empty());
    }

    TEST_CASE("a scene or a prefab that cannot be read for what it uses is listed, with the reason") {
        const AssetDirectory project;
        project.writeText("scenes/broken.scene.json", R"({"fail": "no entities here"})");
        project.writeText("scenes/junk.scene.json", "not json");
        // One byte too many, which is not even read.
        project.writeText("prefabs/huge.prefab.json", std::string(kMaxPrefabFileBytes + 1, ' '));

        const FakeReader fake;
        const AssetDatabase database = fake.open(project.root());
        CHECK(hasProblem(database, "scenes/broken.scene.json: no entities here"));
        CHECK(hasProblem(database, "scenes/junk.scene.json: "));
        CHECK(
            hasProblem(database, fmt::format("prefabs/huge.prefab.json: it has {} bytes; a prefab file has {} at most",
                                             kMaxPrefabFileBytes + 1, kMaxPrefabFileBytes)));
        CHECK(database.problems().size() == 3);
        for (const std::string_view reference :
             {"scenes/broken.scene.json", "scenes/junk.scene.json", "prefabs/huge.prefab.json"}) {
            CHECK(require(database, reference).dependencies.empty());
        }
        REQUIRE(fake.calls->size() == 1);
        CHECK(fake.calls->front().first == AssetType::Scene);
    }

    TEST_CASE("what needs an asset is listed with it, each once, in the order of the assets") {
        const AssetDirectory project;
        project.writeTexture("textures/wall.png");
        project.writeModel("models/crate.gltf");
        const AssetDatabase first = project.open(true);
        const Uuid wall = require(first, "textures/wall.png").id;
        const Uuid body = require(first, "models/crate.gltf#mesh/0").id;
        const Uuid wood = require(first, "models/crate.gltf#material/0").id;
        const Uuid label = require(first, "models/crate.gltf#image/1").id;
        const Uuid metal = require(first, "models/crate.gltf#material/1").id;
        project.writeText("materials/paint.material.json",
                          fmt::format(R"({{"format": "cordite.material", "version": 1, "baseColorTexture": "{0}",
                                         "normalTexture": "{0}"}})",
                                      wall.toString()));
        // Named twice by the scene, which is still one user of it.
        project.writeText("level.scene.json", usesDocument({body, wall, kMissingAsset, wall}));

        const FakeReader fake;
        const AssetDatabase database = fake.open(project.root());
        const Uuid level = require(database, "level.scene.json").id;
        const Uuid paint = require(database, "materials/paint.material.json").id;
        CHECK(std::vector<Uuid>(database.dependents(wall).begin(), database.dependents(wall).end()) ==
              std::vector<Uuid>{level, paint, wood});
        // The model needs what is in it, which is no use of it: the mesh is used by the scene
        // alone, and what is in the model and nowhere else by nothing.
        CHECK(std::vector<Uuid>(database.dependents(body).begin(), database.dependents(body).end()) ==
              std::vector<Uuid>{level});
        CHECK(database.dependents(metal).empty());
        // The image in the model is used by a material in the same file: that is a use.
        CHECK(std::vector<Uuid>(database.dependents(label).begin(), database.dependents(label).end()) ==
              std::vector<Uuid>{wood, metal});
        CHECK(database.dependents(level).empty());
        // What is referred to and is not there has its users too, and is a problem of each.
        CHECK(std::vector<Uuid>(database.dependents(kMissingAsset).begin(), database.dependents(kMissingAsset).end()) ==
              std::vector<Uuid>{level});
        CHECK(hasProblem(database, fmt::format("level.scene.json: it refers to the asset {}, which does not exist",
                                               kMissingAsset.toString())));
        CHECK(database.dependents(Uuid::generate()).empty());
    }

    TEST_CASE("a refresh that finds no directory forgets what needed what") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        const Uuid wall = require(project.open(), "wall.png").id;
        project.writeText("yard.scene.json", usesDocument({wall}));
        const FakeReader fake;
        AssetDatabase database = fake.open(project.root());
        REQUIRE(database.dependents(wall).size() == 1);

        std::error_code removed;
        std::filesystem::remove_all(project.root(), removed);
        REQUIRE_FALSE(removed);
        CHECK_FALSE(database.refresh().has_value());
        CHECK(database.findByReference("yard.scene.json") == nullptr);
        CHECK(database.dependents(wall).empty());
    }

    TEST_CASE("what a scene refers to is read again on its own after its file was written") {
        const AssetDirectory project;
        project.writeTexture("wall.png");
        project.writeText("paint.material.json", R"({"format": "cordite.material", "version": 1})");
        const AssetDatabase plain = project.open();
        const Uuid wall = require(plain, "wall.png").id;
        const Uuid paint = require(plain, "paint.material.json").id;
        project.writeText("yard.scene.json", usesDocument({wall}));

        const FakeReader fake;
        AssetDatabase database = fake.open(project.root());
        const Uuid yard = require(database, "yard.scene.json").id;
        const uint64_t revision = database.revision();
        CHECK(database.dependents(wall).size() == 1);

        project.writeText("yard.scene.json", usesDocument({paint}));
        REQUIRE(database.rereadDependencies(yard).has_value());
        CHECK(require(database, "yard.scene.json").dependencies == std::vector<Uuid>{paint});
        CHECK(database.dependents(wall).empty());
        CHECK(std::vector<Uuid>(database.dependents(paint).begin(), database.dependents(paint).end()) ==
              std::vector<Uuid>{yard});
        // Nothing else was read, and nothing that caches by the revision has to start again.
        CHECK(database.revision() == revision);
        CHECK(fake.calls->size() == 2);

        // What cannot be read again leaves the database as it was.
        project.writeText("yard.scene.json", R"({"fail": "half written"})");
        const Status failed = database.rereadDependencies(yard);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().describe().find("yard.scene.json") != std::string::npos);
        CHECK(failed.error().describe().find("half written") != std::string::npos);
        CHECK(require(database, "yard.scene.json").dependencies == std::vector<Uuid>{paint});

        // Only scenes and prefabs are read on their own; what is no asset is not there.
        const Status material = database.rereadDependencies(paint);
        REQUIRE_FALSE(material.has_value());
        CHECK(material.error().code == ErrorCode::InvalidArgument);
        const Status unknown = database.rereadDependencies(kMissingAsset);
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::NotFound);

        // Without a reader there is nothing to read again, and nothing changes.
        AssetDatabase without({.rootDirectory = project.root()});
        REQUIRE(without.refresh().has_value());
        CHECK(without.rereadDependencies(yard).has_value());
        CHECK(require(without, "yard.scene.json").dependencies.empty());
    }
}
