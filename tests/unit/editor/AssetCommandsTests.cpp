#include "cordite/editor/AssetCommands.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"
#include "cordite/editor/EditorCommands.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// A project with a texture, a material, a scene file and a model, and an editor
/// document with every command registered on it.
///
/// The model, models/cart.gltf:
///
///     Body          (mesh "Body": two submeshes, materials "Paint" and none)
///       Wheel       (mesh "Wheel")
///       Mirror      (camera)
struct AssetFixture {
    TempDirectory directory;
    std::filesystem::path root = directory.path() / "assets";
    ComponentRegistry components;
    World world;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<EditorContext> context;
    CommandRegistry commands;

    AssetFixture() {
        writeTexture("textures/wall.png");
        writeModel("models/cart.gltf");
        writeText("materials/paint.material.json",
                  R"({"format": "cordite.material", "version": 1, "roughness": 0.25})");
        writeText("scenes/yard.scene.json",
                  R"({"format": "cordite.scene", "version": 1, "name": "Yard", "entities": []})");

        REQUIRE(registerEngineComponents(components).has_value());
        assets = unwrap(AssetManager::create({.rootDirectory = root}));
        context = std::make_unique<EditorContext>(world, components, *assets);
        REQUIRE(registerEditorCommands(commands, *context).has_value());
    }

    void writeText(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(root / pathFromUtf8(relativePath), text).has_value());
    }

    void writeTexture(std::string_view relativePath) const {
        Image image = unwrap(Image::create(8, 4, ImageFormat::Rgba8));
        REQUIRE(image.savePng(root / pathFromUtf8(relativePath)).has_value());
    }

    void writeModel(std::string_view relativePath) const {
        GltfDocument document;
        document.images.push_back({.name = "Wall", .uri = "../textures/wall.png"});
        GltfMaterial paint;
        paint.name = "Paint";
        paint.images.baseColor = 0;
        document.materials.push_back(paint);

        GltfMesh body;
        body.mesh.name = "Body";
        const MeshData box = makeBox(Vec3(2.0f, 1.0f, 4.0f));
        const MeshData lamp = makeSphere(0.2f, 8, 4);
        appendSubmesh(body.mesh, box.vertices, box.indices);
        appendSubmesh(body.mesh, lamp.vertices, lamp.indices);
        body.materials = {0, std::nullopt};
        GltfMesh wheel;
        wheel.mesh = makeCylinder(0.5f, 0.3f, 12);
        wheel.mesh.name = "Wheel";
        wheel.materials = {std::nullopt};
        document.meshes = {body, wheel};

        GltfNode bodyNode;
        bodyNode.name = "Body";
        bodyNode.mesh = 0;
        bodyNode.children = {1, 2};
        bodyNode.transform.position = Vec3(0.0f, 1.0f, 0.0f);
        GltfNode wheelNode;
        wheelNode.name = "Wheel";
        wheelNode.parent = 0;
        wheelNode.mesh = 1;
        GltfNode mirror;
        mirror.name = "Mirror";
        mirror.parent = 0;
        mirror.camera = ModelCamera{.verticalFov = 0.5f, .nearPlane = 0.1f, .farPlane = 50.0f};
        document.nodes = {bodyNode, wheelNode, mirror};
        document.roots = {0};
        REQUIRE(exportGltf(document, root / pathFromUtf8(relativePath)).has_value());
    }

    /// Runs a command that is expected to succeed.
    Json run(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));
        return *result;
    }

    /// Runs a command that is expected to fail and returns its error.
    Error fail(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_FALSE(result.has_value());
        return result.error();
    }

    [[nodiscard]] std::string id(std::string_view reference) const {
        const AssetInfo* asset = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
        return asset->id.toString();
    }

    /// Entity names in hierarchy order, indented by depth with dots.
    [[nodiscard]] std::vector<std::string> outline() const {
        std::vector<std::string> lines;
        world.traverse(
            [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
        return lines;
    }
};

std::vector<std::string> pathsOf(const Json& assets) {
    std::vector<std::string> paths;
    for (const Json& asset : assets) {
        paths.push_back(asset["path"].get<std::string>());
    }
    return paths;
}

} // namespace

TEST_SUITE("AssetCommands") {
    TEST_CASE("an asset is found by its id or by its reference") {
        const AssetFixture fixture;
        const EditorContext& context = *fixture.context;

        const AssetInfo* byReference = unwrap(context.resolveAsset("models/cart.gltf#mesh/0"));
        CHECK(byReference->name == "Body");
        CHECK(unwrap(context.resolveAsset(byReference->id.toString())) == byReference);
        // Ids are found whatever their case; surrounding blanks do not matter.
        CHECK(unwrap(context.resolveAsset("  " + fixture.id("textures/wall.png") + " "))->type == AssetType::Texture);
        CHECK(unwrap(context.resolveAsset("builtin#mesh/cube"))->id == builtinMeshId(BuiltinMesh::Cube));

        const auto unknownReference = context.resolveAsset("models/missing.gltf");
        REQUIRE_FALSE(unknownReference.has_value());
        CHECK(unknownReference.error().code == ErrorCode::NotFound);
        CHECK(unknownReference.error().message.find("there is no asset 'models/missing.gltf'") != std::string::npos);

        const auto unknownId = context.resolveAsset("11111111-1111-4111-8111-111111111111");
        REQUIRE_FALSE(unknownId.has_value());
        CHECK(unknownId.error().message == "no asset has the id 11111111-1111-4111-8111-111111111111");

        CHECK(context.resolveAsset("  ").error().code == ErrorCode::InvalidArgument);
        // References are exact: a name alone is not one.
        CHECK_FALSE(context.resolveAsset("Body").has_value());
    }

    TEST_CASE("asset.list shows what exists, and how to refer to it") {
        AssetFixture fixture;
        const Json result = fixture.run("asset.list");

        CHECK(result["directory"] == pathToUtf8(fixture.root));
        CHECK(result["problems"].empty());
        const std::vector<std::string> paths = pathsOf(result["assets"]);
        // The built-in assets first, then the project's.
        CHECK(paths.front() == "builtin#mesh/cube");
        CHECK(std::vector<std::string>(paths.begin() + 8, paths.end()) == std::vector<std::string>{
                                                                              "materials/paint.material.json",
                                                                              "models/cart.gltf",
                                                                              "models/cart.gltf#mesh/0",
                                                                              "models/cart.gltf#mesh/1",
                                                                              "models/cart.gltf#material/0",
                                                                              "scenes/yard.scene.json",
                                                                              "textures/wall.png",
                                                                          });

        const Json& model = result["assets"][9];
        CHECK(model["type"] == "model");
        CHECK(model["name"] == "cart");
        CHECK(model["id"] == fixture.id("models/cart.gltf"));
    }

    TEST_CASE("asset.list can be limited to one type") {
        AssetFixture fixture;
        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "texture"})")["assets"]) ==
              std::vector<std::string>{"textures/wall.png"});
        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "Scene"})")["assets"]) ==
              std::vector<std::string>{"scenes/yard.scene.json"});
        // Seven built-in meshes and the two of the model.
        CHECK(fixture.run("asset.list", R"({"type": "mesh"})")["assets"].size() == 9);

        const Error error = fixture.fail("asset.list", R"({"type": "sound"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message.find("'sound' is not an asset type") != std::string::npos);
    }

    TEST_CASE("asset.list reports what is wrong with the asset directory") {
        AssetFixture fixture;
        fixture.writeText("textures/gone.png.meta",
                          R"({"format": "cordite.asset", "version": 1, "id": "11111111-1111-4111-8111-111111111111"})");
        fixture.run("asset.refresh");
        const Json problems = fixture.run("asset.list")["problems"];
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].get<std::string>().find("textures/gone.png.meta") != std::string::npos);
    }

    TEST_CASE("asset.get describes a mesh") {
        AssetFixture fixture;
        const Json mesh = fixture.run("asset.get", R"({"asset": "models/cart.gltf#mesh/0"})");
        CHECK(mesh["type"] == "mesh");
        CHECK(mesh["name"] == "Body");
        CHECK(mesh["path"] == "models/cart.gltf#mesh/0");
        CHECK(mesh["dependencies"].empty());
        CHECK(mesh["warnings"].empty());
        // What someone placing it needs to know: how big it is, and how many materials it takes.
        CHECK(mesh["content"]["submeshes"] == 2);
        CHECK(mesh["content"]["triangles"] == 12 + 48);
        CHECK(mesh["content"]["vertices"] == 24 + 43);
        CHECK(mesh["content"]["bounds"]["max"] == json("[1.0, 0.5, 2.0]"));

        // By id as well.
        const Json cube =
            fixture.run("asset.get", fmt::format(R"({{"asset": "{}"}})", builtinMeshId(BuiltinMesh::Cube).toString()));
        CHECK(cube["path"] == "builtin#mesh/cube");
        CHECK(cube["content"]["triangles"] == 12);
        CHECK(cube["content"]["submeshes"] == 1);
    }

    TEST_CASE("asset.get describes a material, a texture, a model and a scene") {
        AssetFixture fixture;

        const Json material = fixture.run("asset.get", R"({"asset": "models/cart.gltf#material/0"})");
        CHECK(material["name"] == "Paint");
        REQUIRE(material["dependencies"].size() == 1);
        CHECK(material["dependencies"][0]["path"] == "textures/wall.png");
        CHECK(material["content"]["properties"]["baseColorTexture"] == fixture.id("textures/wall.png"));
        CHECK(material["content"]["properties"]["alphaMode"] == "opaque");

        const Json file = fixture.run("asset.get", R"({"asset": "materials/paint.material.json"})");
        CHECK(file["content"]["properties"]["roughness"] == 0.25);

        const Json texture = fixture.run("asset.get", R"({"asset": "textures/wall.png"})");
        CHECK(texture["content"]["width"] == 8);
        CHECK(texture["content"]["height"] == 4);
        CHECK(texture["content"]["format"] == "Rgba8");

        const Json model = fixture.run("asset.get", R"({"asset": "models/cart.gltf"})");
        CHECK(model["dependencies"].size() == 3);
        REQUIRE(model["content"]["nodes"].size() == 3);
        CHECK(model["content"]["nodes"][0]["name"] == "Body");
        CHECK(model["content"]["nodes"][0]["parent"].is_null());
        CHECK(model["content"]["nodes"][0]["mesh"] == fixture.id("models/cart.gltf#mesh/0"));
        CHECK(model["content"]["nodes"][1]["parent"] == 0);
        CHECK(model["content"]["nodes"][2]["mesh"].is_null());
        CHECK(model["content"]["nodes"][2]["camera"] == true);
        CHECK(model["content"]["roots"] == json("[0]"));

        const Json scene = fixture.run("asset.get", R"({"asset": "scenes/yard.scene.json"})");
        CHECK(scene["type"] == "scene");
        CHECK(scene["content"].empty());
    }

    TEST_CASE("asset.get describes a skinned mesh, its skeleton and the animations that move it") {
        AssetFixture fixture;
        REQUIRE(exportGltf(test::makeSkinnedBar(), fixture.root / "models" / "bar.gltf").has_value());
        fixture.run("asset.refresh");

        // Listed by their types, like every other asset.
        const Json skeletons = fixture.run("asset.list", R"({"type": "skeleton"})");
        REQUIRE(skeletons["assets"].size() == 1);
        CHECK(skeletons["assets"][0]["path"] == "models/bar.gltf#skeleton/0");
        CHECK(skeletons["assets"][0]["name"] == "Bar skin");
        const Json animations = fixture.run("asset.list", R"({"type": "animation"})");
        REQUIRE(animations["assets"].size() == 2);
        CHECK(animations["assets"][1]["name"] == "Sway");

        const Json mesh = fixture.run("asset.get", R"({"asset": "models/bar.gltf#mesh/0"})");
        CHECK(mesh["content"]["skinned"] == true);
        CHECK(mesh["content"]["joints"] == 3);
        // A rigid mesh says so, and needs no joints.
        const Json rigid = fixture.run("asset.get", R"({"asset": "models/cart.gltf#mesh/0"})");
        CHECK(rigid["content"]["skinned"] == false);
        CHECK(rigid["content"]["joints"] == 0);

        const Json skeleton = fixture.run("asset.get", R"({"asset": "models/bar.gltf#skeleton/0"})");
        CHECK(skeleton["type"] == "skeleton");
        REQUIRE(skeleton["content"]["joints"].size() == 3);
        CHECK(skeleton["content"]["joints"][0]["name"] == "Root");
        CHECK(skeleton["content"]["joints"][0]["parent"].is_null());
        CHECK(skeleton["content"]["joints"][2]["name"] == "Tip");
        CHECK(skeleton["content"]["joints"][2]["parent"] == 1);

        const Json bend = fixture.run("asset.get", R"({"asset": "models/bar.gltf#animation/0"})");
        CHECK(bend["type"] == "animation");
        CHECK(bend["name"] == "Bend");
        CHECK(bend["content"]["duration"] == 1.0);
        CHECK(bend["content"]["joints"] == json(R"(["Middle"])"));

        // The model names the skeleton of the node that is drawn with one, and its clips.
        const Json model = fixture.run("asset.get", R"({"asset": "models/bar.gltf"})");
        CHECK(model["content"]["nodes"][0]["skeleton"] == fixture.id("models/bar.gltf#skeleton/0"));
        CHECK(model["content"]["nodes"][1]["skeleton"].is_null());
        // The nodes that are joints say so: placing the model makes no entities of them.
        CHECK(model["content"]["nodes"][0]["joint"] == false);
        CHECK(model["content"]["nodes"][1]["joint"] == true);
        CHECK(model["content"]["nodes"][3]["joint"] == true);
        CHECK(model["content"]["animations"] ==
              Json::array({fixture.id("models/bar.gltf#animation/0"), fixture.id("models/bar.gltf#animation/1")}));
    }

    TEST_CASE("asset.get fails with the reason for an asset that is unknown or cannot be loaded") {
        AssetFixture fixture;
        CHECK(fixture.fail("asset.get", R"({"asset": "models/none.gltf"})").code == ErrorCode::NotFound);
        CHECK(fixture.fail("asset.get").code == ErrorCode::InvalidArgument);

        fixture.writeText("materials/typo.material.json",
                          R"({"format": "cordite.material", "version": 1, "rougness": 1})");
        fixture.run("asset.refresh");
        const Error error = fixture.fail("asset.get", R"({"asset": "materials/typo.material.json"})");
        CHECK(error.message.find("rougness") != std::string::npos);
    }

    TEST_CASE("asset.get lists a dependency that is missing, rather than hiding it") {
        AssetFixture fixture;
        fixture.writeText("materials/lost.material.json", R"({
            "format": "cordite.material", "version": 1, "normalTexture": "11111111-1111-4111-8111-111111111111"
        })");
        fixture.run("asset.refresh");
        const Json material = fixture.run("asset.get", R"({"asset": "materials/lost.material.json"})");
        REQUIRE(material["dependencies"].size() == 1);
        CHECK(material["dependencies"][0]["id"] == "11111111-1111-4111-8111-111111111111");
        CHECK(material["dependencies"][0]["missing"] == true);
    }

    TEST_CASE("asset.refresh picks up files that were added or changed") {
        AssetFixture fixture;
        const size_t before = fixture.run("asset.list")["assets"].size();

        fixture.writeTexture("textures/floor.png");
        fixture.writeText("materials/paint.material.json",
                          R"({"format": "cordite.material", "version": 1, "roughness": 0.75})");
        // Not seen until asked for.
        CHECK(fixture.run("asset.list")["assets"].size() == before);

        const Json refreshed = fixture.run("asset.refresh");
        CHECK(refreshed["assetCount"] == before + 1);
        CHECK(refreshed["problems"].empty());
        CHECK(fixture.run("asset.get",
                          R"({"asset": "materials/paint.material.json"})")["content"]["properties"]["roughness"] ==
              0.75);
    }

    TEST_CASE("asset.instantiate places a model as a group of entities and selects its root") {
        AssetFixture fixture;
        const Json placed = fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");

        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror"});
        CHECK(placed["name"] == "cart");
        CHECK(placed["path"] == "cart");
        CHECK(placed["entityCount"] == 4);

        const World& world = fixture.world;
        const Entity root = world.findByName("cart");
        CHECK(placed["id"] == world.id(root).toString());
        CHECK(fixture.context->selection() == std::vector<Entity>{root});

        const Entity body = world.findByName("Body");
        CHECK(world.transform(body).position == Vec3(0.0f, 1.0f, 0.0f));
        const auto* renderer = world.tryGet<MeshRendererComponent>(body);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh.toString() == fixture.id("models/cart.gltf#mesh/0"));
        REQUIRE(renderer->materials.size() == 2);
        CHECK(renderer->materials[0].toString() == fixture.id("models/cart.gltf#material/0"));
        CHECK(renderer->materials[1].isNil());
        CHECK(world.has<CameraComponent>(world.findByName("Mirror")));

        // What was placed is a scene like any other: valid, and saved with the rest.
        CHECK(fixture.run("scene.validate")["valid"] == true);
        CHECK(fixture.context->hasUnsavedChanges());
    }

    TEST_CASE("asset.instantiate takes a parent and a name") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Yard"})");
        const Json placed =
            fixture.run("asset.instantiate", fmt::format(R"({{"asset": "{}", "parent": "Yard", "name": "Cart 7"}})",
                                                         fixture.id("models/cart.gltf")));
        CHECK(placed["path"] == "Yard/Cart 7");
        CHECK(fixture.outline() == std::vector<std::string>{"Yard", ".Cart 7", "..Body", "...Wheel", "...Mirror"});
    }

    TEST_CASE("placing a model is one step in the history") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Yard"})");
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf", "parent": "Yard"})");
        const Json placedScene = serializeScene(fixture.world, fixture.components);
        CHECK(fixture.context->history().undoLabel() == "Place 'cart'");

        fixture.run("editor.undo");
        CHECK(fixture.outline() == std::vector<std::string>{"Yard"});

        // Redoing brings back the very same entities, ids included.
        fixture.run("editor.redo");
        CHECK(serializeScene(fixture.world, fixture.components) == placedScene);

        // And later edits to them can still be undone and redone across that.
        fixture.run("entity.rename", R"({"entity": "Yard/cart/Body", "name": "Chassis"})");
        fixture.run("editor.undo");
        fixture.run("editor.undo");
        fixture.run("editor.redo");
        fixture.run("editor.redo");
        CHECK(fixture.outline() == std::vector<std::string>{"Yard", ".cart", "..Chassis", "...Wheel", "...Mirror"});
    }

    TEST_CASE("a model can be placed any number of times") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        CHECK(fixture.world.entityCount() == 8);
        // Two roots with the same name: a name alone no longer says which.
        CHECK(fixture.fail("entity.get", R"({"entity": "cart"})").code == ErrorCode::InvalidArgument);
    }

    TEST_CASE("asset.instantiate refuses what it cannot place, and changes nothing") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Yard"})");
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t history = fixture.context->history().stateId();

        SUBCASE("something that is not a model") {
            const Error error = fixture.fail("asset.instantiate", R"({"asset": "textures/wall.png"})");
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message.find("textures/wall.png is a texture, not a model") != std::string::npos);
        }
        SUBCASE("an asset that does not exist") {
            CHECK(fixture.fail("asset.instantiate", R"({"asset": "models/none.gltf"})").code == ErrorCode::NotFound);
        }
        SUBCASE("a parent that does not exist") {
            CHECK(fixture.fail("asset.instantiate", R"({"asset": "models/cart.gltf", "parent": "Nowhere"})").code ==
                  ErrorCode::NotFound);
        }
        SUBCASE("a name an entity may not have") {
            const Error error = fixture.fail("asset.instantiate", R"({"asset": "models/cart.gltf", "name": "a/b"})");
            CHECK(error.code == ErrorCode::InvalidArgument);
        }
        SUBCASE("a model that cannot be loaded") {
            // Indexed while it was whole; its geometry is gone by the time it is placed.
            std::filesystem::remove(fixture.root / "models" / "cart.bin");
            const Error error = fixture.fail("asset.instantiate", R"({"asset": "models/cart.gltf"})");
            CHECK(error.message.find("cart.bin") != std::string::npos);
        }
        SUBCASE("no asset at all") {
            CHECK(fixture.fail("asset.instantiate").code == ErrorCode::InvalidArgument);
        }

        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == history);
    }

    TEST_CASE("scene.validate finds references that lead nowhere or to the wrong kind of asset") {
        AssetFixture fixture;
        const std::string cube = builtinMeshId(BuiltinMesh::Cube).toString();
        const std::string wall = fixture.id("textures/wall.png");

        fixture.run("entity.create", fmt::format(R"({{"name": "Good", "components": {{"MeshRenderer": {{
            "mesh": "{}", "materials": ["{}"]}}}}}})",
                                                 cube, fixture.id("materials/paint.material.json")));
        Json result = fixture.run("scene.validate");
        CHECK(result["valid"] == true);
        CHECK(result["problems"].empty());
        // The assets the scene uses, each once.
        CHECK(pathsOf(result["assets"]) ==
              std::vector<std::string>{"builtin#mesh/cube", "materials/paint.material.json"});

        fixture.run("entity.create",
                    fmt::format(R"({{"name": "Bad", "parent": "Good", "components": {{"MeshRenderer": {{
            "mesh": "{}", "materials": ["11111111-1111-4111-8111-111111111111"]}}}}}})",
                                wall));
        result = fixture.run("scene.validate");
        CHECK(result["valid"] == false);
        REQUIRE(result["problems"].size() == 2);
        CHECK(result["problems"][0]["entity"] == "Good/Bad");
        CHECK(result["problems"][0]["problem"] ==
              "MeshRenderer.mesh refers to textures/wall.png, which is a texture, not a mesh");
        CHECK(result["problems"][1]["problem"].get<std::string>().find("which does not exist") != std::string::npos);
        CHECK(result["problems"][1]["id"] == fixture.world.id(fixture.world.findByName("Bad")).toString());

        // The image of an environment is checked like any other reference: a texture is
        // one of the assets the scene uses, a mesh is no image.
        fixture.run("entity.delete", R"({"entities": ["Good/Bad"]})");
        fixture.run("entity.create",
                    fmt::format(R"({{"name": "Sky", "components": {{"Environment": {{"texture": "{}"}}}}}})", wall));
        result = fixture.run("scene.validate");
        CHECK(result["valid"] == true);
        CHECK(pathsOf(result["assets"]) ==
              std::vector<std::string>{"builtin#mesh/cube", "materials/paint.material.json", "textures/wall.png"});
        fixture.run("component.set", fmt::format(R"({{"entity": "Sky", "component": "Environment",
            "properties": {{"texture": "{}"}}}})",
                                                 cube));
        result = fixture.run("scene.validate");
        CHECK(result["valid"] == false);
        REQUIRE(result["problems"].size() == 1);
        CHECK(result["problems"][0]["entity"] == "Sky");
        CHECK(result["problems"][0]["problem"] ==
              "Environment.texture refers to builtin#mesh/cube, which is a mesh, not a texture");
    }

    TEST_CASE("asset references are set like any other property") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Crate"})");
        fixture.run("component.add", R"({"entity": "Crate", "component": "MeshRenderer"})");

        const std::string cube = builtinMeshId(BuiltinMesh::Cube).toString();
        const std::string paint = fixture.id("materials/paint.material.json");
        fixture.run("component.set", fmt::format(R"({{"entity": "Crate", "component": "MeshRenderer",
                                     "properties": {{"mesh": "{}", "materials": ["{}", null]}}}})",
                                                 cube, paint));

        const auto* renderer = fixture.world.tryGet<MeshRendererComponent>(fixture.world.findByName("Crate"));
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh == builtinMeshId(BuiltinMesh::Cube));
        REQUIRE(renderer->materials.size() == 2);
        CHECK(renderer->materials[0].toString() == paint);
        CHECK(renderer->materials[1].isNil());

        const Json properties = fixture.run("component.get", R"({"entity": "Crate", "component": "MeshRenderer"})");
        CHECK(properties["mesh"] == cube);
        CHECK(properties["materials"] == json(fmt::format(R"(["{}", null])", paint)));

        // An id is required; a path is not one.
        const Error error = fixture.fail(
            "component.set",
            R"({"entity": "Crate", "component": "MeshRenderer", "properties": {"mesh": "models/cart.gltf#mesh/0"}})");
        CHECK(error.message.find("mesh") != std::string::npos);
        CHECK(renderer->mesh == builtinMeshId(BuiltinMesh::Cube));
    }

    TEST_CASE("scene.open finds a scene of the project by its reference") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Leftover"})");

        const Json opened = fixture.run("scene.open", R"({"path": "scenes/yard.scene.json"})");
        CHECK(opened["name"] == "Yard");
        CHECK(fixture.world.entityCount() == 0);
        CHECK(fixture.context->scenePath() == fixture.root / "scenes" / "yard.scene.json");

        // A file path still works, for scenes outside the project.
        const std::filesystem::path elsewhere = fixture.directory.path() / "other.scene.json";
        REQUIRE(
            writeTextFile(elsewhere, R"({"format": "cordite.scene", "version": 1, "name": "Other", "entities": []})")
                .has_value());
        CHECK(fixture.run("scene.open", fmt::format(R"({{"path": {}}})", Json(pathToUtf8(elsewhere)).dump()))["name"] ==
              "Other");

        // An asset that is not a scene is not opened as one.
        CHECK_FALSE(fixture.commands.execute("scene.open", json(R"({"path": "textures/wall.png"})")).has_value());
    }
}
