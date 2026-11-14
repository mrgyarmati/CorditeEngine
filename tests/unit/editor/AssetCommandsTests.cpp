#include "cordite/editor/AssetCommands.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Prefab.h"
#include "cordite/assets/Primitives.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/FileSystem.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/editor/AssetDrop.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorCommands.h"
#include "cordite/editor/PosedMesh.h"
#include "cordite/editor/PrefabLinks.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "support/MathTestSupport.h"
#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// The id that `text`, as a command returns ids, is.
Uuid parsedId(std::string_view text) {
    const std::optional<Uuid> id = Uuid::parse(text);
    REQUIRE_MESSAGE(id.has_value(), "no id: " << std::string(text));
    return *id;
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
    /// What an Assets panel would list, for the command that asks it.
    AssetBrowser browser;
    CommandRegistry commands;

    AssetFixture() {
        writeTexture("textures/wall.png");
        writeModel("models/cart.gltf");
        writeText("materials/paint.material.json",
                  R"({"format": "cordite.material", "version": 1, "roughness": 0.25})");
        writeText("scenes/yard.scene.json",
                  R"({"format": "cordite.scene", "version": 1, "name": "Yard", "entities": []})");
        writeText("scripts/gate.lua", "-- Opens.\nlocal Gate = {}\nreturn Gate\n");
        writeSound("sounds/creak.wav");

        REQUIRE(registerEngineComponents(components).has_value());
        // As an engine reads them: scenes and prefabs say what they use.
        assets =
            unwrap(AssetManager::create({.rootDirectory = root, .readReferences = assetReferenceReader(components)}));
        context = std::make_unique<EditorContext>(world, components, *assets);
        REQUIRE(registerEditorCommands(commands, *context).has_value());
        REQUIRE(registerAssetBrowserCommands(commands, *context, browser).has_value());
    }

    void writeText(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(root / pathFromUtf8(relativePath), text).has_value());
    }

    /// Half a second of stereo at 8 kHz, growing louder.
    void writeSound(std::string_view relativePath) const {
        SoundData sound{.sampleRate = 8000, .channels = 2};
        for (int sample = 0; sample < 8000; ++sample) {
            sound.samples.push_back(static_cast<float>(sample) / 8000.0f);
        }
        REQUIRE(writeBinaryFile(root / pathFromUtf8(relativePath), unwrap(encodeWav(sound))).has_value());
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

/// The rows `assets.browse` returns, one line each: indented by depth, a directory with a
/// slash, an asset with its type; "..." for a row that is closed over what is below it,
/// "-" for a file that is listed only for what is inside it, "*" for the chosen asset.
std::vector<std::string> browserLines(const Json& rows) {
    std::vector<std::string> lines;
    for (const Json& row : rows) {
        std::string line(row["depth"].get<size_t>() * 2, ' ');
        line += row["name"].get<std::string>();
        if (row["kind"] == "directory") {
            line += "/";
        } else {
            line += fmt::format(" ({})", row["type"].get<std::string>());
        }
        if (row.contains("open") && row["open"] == false) {
            line += " ...";
        }
        if (row.contains("matches") && row["matches"] == false) {
            line += " -";
        }
        if (row.contains("selected")) {
            line += " *";
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

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
                                                                              "scripts/gate.lua",
                                                                              "sounds/creak.wav",
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
        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "script"})")["assets"]) ==
              std::vector<std::string>{"scripts/gate.lua"});
        // Seven built-in meshes and the two of the model.
        CHECK(fixture.run("asset.list", R"({"type": "mesh"})")["assets"].size() == 9);

        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "sound"})")["assets"]) ==
              std::vector<std::string>{"sounds/creak.wav"});

        // What is no type is answered with the types there are - all of them.
        const Error error = fixture.fail("asset.list", R"({"type": "font"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "asset.list: 'font' is not an asset type; use texture, mesh, material, model, scene, "
                               "skeleton, animation, script, sound, prefab, renderTarget or sequence");
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

    TEST_CASE("asset.get shows a script: how long it is, and what it says") {
        AssetFixture fixture;
        const Json script = fixture.run("asset.get", R"({"asset": "scripts/gate.lua"})");
        CHECK(script["type"] == "script");
        CHECK(script["name"] == "gate");
        CHECK(script["dependencies"].empty());
        CHECK(script["content"]["bytes"] == 38);
        CHECK(script["content"]["lines"] == 3);
        CHECK(script["content"]["text"] == "-- Opens.\nlocal Gate = {}\nreturn Gate\n");

        // A last line that does not end is a line all the same; no text is no line.
        fixture.writeText("scripts/gate.lua", "return {}");
        fixture.writeText("scripts/empty.lua", "");
        static_cast<void>(fixture.run("asset.refresh"));
        CHECK(fixture.run("asset.get", R"({"asset": "scripts/gate.lua"})")["content"]["lines"] == 1);
        const Json empty = fixture.run("asset.get", R"({"asset": "scripts/empty.lua"})");
        CHECK(empty["content"]["lines"] == 0);
        CHECK(empty["content"]["bytes"] == 0);
    }

    TEST_CASE("asset.get shows a sound: how long it is, and what it is made of") {
        AssetFixture fixture;
        const Json sound = fixture.run("asset.get", R"({"asset": "sounds/creak.wav"})");
        CHECK(sound["type"] == "sound");
        CHECK(sound["name"] == "creak");
        CHECK(sound["dependencies"].empty());
        CHECK(sound["content"]["sampleRate"] == 8000);
        CHECK(sound["content"]["channels"] == 2);
        CHECK(sound["content"]["frames"] == 4000);
        CHECK(sound["content"]["seconds"] == 0.5);
        // The samples themselves are not something to read.
        CHECK(sound["content"].size() == 4);

        // A file that is no sound says so when it is asked for, and not before.
        fixture.writeText("sounds/words.mp3", "these are words");
        static_cast<void>(fixture.run("asset.refresh"));
        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "sound"})")["assets"]).size() == 2);
        const Error error = fixture.fail("asset.get", R"({"asset": "sounds/words.mp3"})");
        CHECK(error.code == ErrorCode::ParseError);
        CHECK(error.message.starts_with(
            "asset.get: sounds/words.mp3: it is no WAV, FLAC or MP3 file that can be read: "));
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

    TEST_CASE("asset.instantiate places a mesh as one entity that draws it, with the materials of its model") {
        AssetFixture fixture;
        const World& world = fixture.world;
        const auto placedEntity = [&](const Json& placed) {
            const Entity entity = world.findById(parsedId(placed["id"].get<std::string>()));
            REQUIRE_FALSE(entity.isNull());
            return entity;
        };

        // A mesh built into the engine is drawn with the default material.
        Json placed = fixture.run("asset.instantiate", R"({"asset": "builtin#mesh/cube"})");
        CHECK(placed["name"] == "Cube");
        CHECK(placed["path"] == "Cube");
        CHECK(placed["entityCount"] == 1);
        const Entity cube = placedEntity(placed);
        const auto* renderer = world.tryGet<MeshRendererComponent>(cube);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh == builtinMeshId(BuiltinMesh::Cube));
        CHECK(renderer->materials.empty());
        CHECK(world.transform(cube) == Transform());
        CHECK(fixture.context->selection() == std::vector<Entity>{cube});
        CHECK(fixture.context->history().undoLabel() == "Place 'Cube'");

        // A mesh of a model is drawn as the model draws it: the body of the cart in its
        // paint, and its lamp in the default material.
        fixture.run("entity.create", R"({"name": "Yard"})");
        placed = fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf#mesh/0", "parent": "Yard"})");
        CHECK(placed["path"] == "Yard/Body");
        const Uuid body = world.id(placedEntity(placed));
        renderer = world.tryGet<MeshRendererComponent>(world.findById(body));
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh.toString() == fixture.id("models/cart.gltf#mesh/0"));
        REQUIRE(renderer->materials.size() == 2);
        CHECK(renderer->materials[0].toString() == fixture.id("models/cart.gltf#material/0"));
        CHECK(renderer->materials[1].isNil());
        CHECK(fixture.outline() == std::vector<std::string>{"Cube", "Yard", ".Body"});

        // One step of the history each, which brings back the very entity.
        const Json scene = serializeScene(world, fixture.components);
        fixture.run("editor.undo");
        CHECK(world.findById(body).isNull());
        fixture.run("editor.redo");
        CHECK(serializeScene(world, fixture.components) == scene);
        CHECK(fixture.run("scene.validate")["valid"] == true);
    }

    TEST_CASE("asset.instantiate puts the root of what it places where it is told, relative to its parent") {
        AssetFixture fixture;
        const World& world = fixture.world;
        fixture.run("entity.create", R"({"name": "Yard", "components": {"Transform": {"position": [10, 0, 0]}}})");

        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf", "parent": "Yard", "position": [1, 2, 3]})");
        const Entity cart = world.findByName("cart");
        CHECK(world.transform(cart).position == Vec3(1.0f, 2.0f, 3.0f));
        CHECK(Vec3(world.computeWorldMatrix(cart)[3]) == Vec3(11.0f, 2.0f, 3.0f));
        // A root is put there in the world.
        fixture.run("asset.instantiate", R"({"asset": "builtin#mesh/cube", "position": [-4, 0.5, 2]})");
        CHECK(world.transform(world.findByName("Cube")).position == Vec3(-4.0f, 0.5f, 2.0f));

        // A prefab is placed where it has its root, or where it is told.
        fixture.run("prefab.create", R"({"entity": "Yard/cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "As made"})");
        CHECK(world.transform(world.findByName("As made")).position == Vec3(1.0f, 2.0f, 3.0f));
        fixture.run("asset.instantiate",
                    R"({"asset": "prefabs/cart.prefab.json", "name": "Moved", "position": [0, 0, -6]})");
        CHECK(world.transform(world.findByName("Moved")).position == Vec3(0.0f, 0.0f, -6.0f));
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

        SUBCASE("something that is neither a model nor a prefab") {
            const Error error = fixture.fail("asset.instantiate", R"({"asset": "textures/wall.png"})");
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message == "asset.instantiate: textures/wall.png is a texture; what is placed in a scene is a "
                                   "model, a prefab or a mesh");
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
        SUBCASE("a position that is not three numbers") {
            for (const char* position : {R"([1, 2])", R"([1, "2", 3])", R"({"x": 1})", R"([1, 2, 3, 4])"}) {
                const Error error = fixture.fail(
                    "asset.instantiate", fmt::format(R"({{"asset": "builtin#mesh/cube", "position": {}}})", position));
                CHECK_MESSAGE(error.code == ErrorCode::InvalidArgument, std::string(position));
            }
        }
        SUBCASE("a mesh with a parent that does not exist, or a name with a slash") {
            CHECK(fixture.fail("asset.instantiate", R"({"asset": "builtin#mesh/cube", "parent": "Nowhere"})").code ==
                  ErrorCode::NotFound);
            CHECK(fixture.fail("asset.instantiate", R"({"asset": "builtin#mesh/cube", "name": "a/b"})").code ==
                  ErrorCode::InvalidArgument);
        }
        SUBCASE("a mesh that cannot be loaded") {
            std::filesystem::remove(fixture.root / "models" / "cart.bin");
            const Error error = fixture.fail("asset.instantiate", R"({"asset": "models/cart.gltf#mesh/1"})");
            CHECK(error.message.find("cart.bin") != std::string::npos);
        }
        SUBCASE("what code is told to place where there is no place, or as no entity can be called") {
            EditorActions actions(*fixture.context);
            const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
            const Uuid cart = parsedId(fixture.id("models/cart.gltf"));
            for (const Uuid& asset : {cube, cart}) {
                const Result<Entity> nowhere = actions.instantiateAsset(
                    asset, {.position = Vec3(0.0f, std::numeric_limits<float>::infinity(), 0.0f)});
                REQUIRE_FALSE(nowhere.has_value());
                CHECK(nowhere.error().code == ErrorCode::InvalidArgument);
                CHECK(nowhere.error().message.find("position") != std::string::npos);
                const Result<Entity> named = actions.instantiateAsset(asset, {.name = "a/b"});
                REQUIRE_FALSE(named.has_value());
                CHECK(named.error().code == ErrorCode::InvalidArgument);
            }
            // What is not a mesh is not placed as one.
            const Result<Entity> wall = actions.instantiateMesh(parsedId(fixture.id("textures/wall.png")));
            REQUIRE_FALSE(wall.has_value());
            CHECK(wall.error().message == "textures/wall.png is a texture, not a mesh");
            const Result<Entity> none = actions.instantiateMesh(Uuid::generate());
            REQUIRE_FALSE(none.has_value());
            CHECK(none.error().code == ErrorCode::NotFound);
        }

        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == history);
    }

    TEST_CASE(
        "prefab.create writes an entity and what is below it into a prefab, and marks the entity as its instance") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        const World& world = fixture.world;
        const Entity cart = world.findByName("cart");
        const uint64_t placed = fixture.context->history().stateId();

        const Json made = fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        CHECK(made["type"] == "prefab");
        CHECK(made["name"] == "cart");
        CHECK(made["path"] == "prefabs/cart.prefab.json");
        CHECK(made["entityCount"] == 4);
        CHECK(made["id"] == fixture.id("prefabs/cart.prefab.json"));

        // The file is a prefab as the assets read one: the entities as the scene has them.
        const Json document = unwrap(readJsonFile(fixture.root / "prefabs" / "cart.prefab.json"));
        CHECK(document["format"] == "cordite.prefab");
        CHECK(document["version"] == 1);
        CHECK(document["name"] == "cart");
        REQUIRE(document["entities"].size() == 4);
        CHECK(document["entities"][0]["name"] == "cart");
        CHECK(document["entities"][0]["parent"].is_null());
        CHECK(document["entities"][0]["id"] == world.id(cart).toString());
        CHECK(document["entities"][1]["name"] == "Body");
        CHECK(document["entities"][1]["parent"] == world.id(cart).toString());
        CHECK(document["entities"][1]["components"]["MeshRenderer"]["mesh"] == fixture.id("models/cart.gltf#mesh/0"));
        // The prefab is not an instance of itself: the mark is on the entity, not in the file.
        CHECK_FALSE(document["entities"][0]["components"].contains("PrefabInstance"));

        // It is an asset from then on, and says what is in it.
        CHECK(pathsOf(fixture.run("asset.list", R"({"type": "prefab"})")["assets"]) ==
              std::vector<std::string>{"prefabs/cart.prefab.json"});
        const Json content = fixture.run("asset.get", R"({"asset": "prefabs/cart.prefab.json"})")["content"];
        CHECK(content["name"] == "cart");
        CHECK(content["entityCount"] == 4);
        CHECK(content["entities"] == json(R"(["cart", "Body", "Wheel", "Mirror"])"));
        // What its entities are made of, each once, and nothing wrong with any of it.
        CHECK(pathsOf(content["assets"]) == std::vector<std::string>{"models/cart.gltf#mesh/0",
                                                                     "models/cart.gltf#material/0",
                                                                     "models/cart.gltf#mesh/1"});
        CHECK(content["problems"].empty());

        // The entity it was made of says so, and nothing else about the scene has changed.
        REQUIRE(world.has<PrefabInstanceComponent>(cart));
        CHECK(made["id"] == world.tryGet<PrefabInstanceComponent>(cart)->prefab.toString());
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror"});
        CHECK(fixture.run("scene.validate")["valid"] == true);

        // Every entity of it is linked to itself in the file, which has the ids it has.
        for (const std::string_view path : {"cart", "cart/Body", "cart/Body/Wheel", "cart/Body/Mirror"}) {
            const Entity linked = unwrap(fixture.context->resolveEntity(path));
            REQUIRE(world.has<PrefabLinkComponent>(linked));
            CHECK(world.tryGet<PrefabLinkComponent>(linked)->source == world.id(linked));
            CHECK(world.tryGet<PrefabLinkComponent>(linked)->overrides.empty());
        }

        // That mark is the edit, and what undoing takes back; the file stays an asset.
        CHECK(fixture.context->history().undoLabel() == "Make 'cart' an instance of its prefab");
        fixture.run("editor.undo");
        CHECK(fixture.context->history().stateId() == placed);
        CHECK_FALSE(world.has<PrefabInstanceComponent>(cart));
        CHECK_FALSE(world.has<PrefabLinkComponent>(cart));
        CHECK(fileExists(fixture.root / "prefabs" / "cart.prefab.json"));
        CHECK(made["id"] == fixture.id("prefabs/cart.prefab.json"));
    }

    TEST_CASE("a prefab is placed as copies of its entities, marked, in one step of the history") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("entity.create", R"({"name": "Yard"})");
        const World& world = fixture.world;
        const Entity original = world.findByName("cart");
        const std::string prefab = fixture.id("prefabs/cart.prefab.json");

        const Json placed = fixture.run("asset.instantiate",
                                        R"({"asset": "prefabs/cart.prefab.json", "parent": "Yard", "name": "Cart 2"})");
        CHECK(placed["name"] == "Cart 2");
        CHECK(placed["path"] == "Yard/Cart 2");
        CHECK(placed["entityCount"] == 4);
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror", "Yard", ".Cart 2",
                                                            "..Body", "...Wheel", "...Mirror"});

        const Entity copy = unwrap(fixture.context->resolveEntity("Yard/Cart 2"));
        CHECK(placed["id"] == world.id(copy).toString());
        CHECK(copy != original);
        CHECK(world.id(copy) != world.id(original));
        CHECK(fixture.context->selection() == std::vector<Entity>{copy});
        // Marked as what it is, and made of what the original is made of.
        REQUIRE(world.has<PrefabInstanceComponent>(copy));
        CHECK(world.tryGet<PrefabInstanceComponent>(copy)->prefab.toString() == prefab);
        const Entity body = unwrap(fixture.context->resolveEntity("Yard/Cart 2/Body"));
        CHECK(world.transform(body).position == Vec3(0.0f, 1.0f, 0.0f));
        REQUIRE(world.has<MeshRendererComponent>(body));
        CHECK(world.tryGet<MeshRendererComponent>(body)->mesh.toString() == fixture.id("models/cart.gltf#mesh/0"));
        CHECK(world.has<CameraComponent>(unwrap(fixture.context->resolveEntity("Yard/Cart 2/Body/Mirror"))));
        CHECK(fixture.run("scene.validate")["valid"] == true);

        // By its id as well, by the name its root has, as a root of the scene.
        const Json again = fixture.run("asset.instantiate", fmt::format(R"({{"asset": "{}"}})", prefab));
        CHECK(again["name"] == "cart");
        CHECK(world.parent(unwrap(fixture.context->resolveEntity(again["id"].get<std::string>()))).isNull());
        CHECK(again["id"] != placed["id"]);
        CHECK(world.entityCount() == 13);
        fixture.run("editor.undo");
        CHECK(world.entityCount() == 9);

        // One step: undone, it is gone; redone, it is the very same entities again.
        const Json placedScene = serializeScene(world, fixture.components);
        CHECK(fixture.context->history().undoLabel() == "Place 'Cart 2'");
        fixture.run("editor.undo");
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror", "Yard"});
        fixture.run("editor.redo");
        CHECK(serializeScene(world, fixture.components) == placedScene);
    }

    TEST_CASE("a prefab is named after its entity when it is not told, and written over only when it is told") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Front wheel"})");
        fixture.run("entity.create", R"({"name": "Hub", "parent": "Front wheel"})");

        const Json made = fixture.run("prefab.create", R"({"entity": "Front wheel"})");
        CHECK(made["path"] == "prefabs/front-wheel.prefab.json");
        CHECK(made["name"] == "front-wheel");
        CHECK(made["entityCount"] == 2);
        const std::filesystem::path file = fixture.root / "prefabs" / "front-wheel.prefab.json";
        const std::string written = unwrap(readTextFile(file));
        const Json marked = serializeScene(fixture.world, fixture.components);
        const uint64_t history = fixture.context->history().stateId();

        // What a file is named after an entity, as far as a file can be.
        CHECK(defaultPrefabPath("Front wheel") == "prefabs/front-wheel.prefab.json");
        CHECK(defaultPrefabPath("Crate #7 (old)") == "prefabs/crate-7-old.prefab.json");
        CHECK(defaultPrefabPath("  --Lamp--  ") == "prefabs/lamp.prefab.json");
        CHECK(defaultPrefabPath("a/../../b") == "prefabs/a-b.prefab.json");
        CHECK(defaultPrefabPath("\xC3\x9Cn\xC3\xAF"
                                "code \xE2\x9C\x93") == "prefabs/n-code.prefab.json");
        CHECK(defaultPrefabPath("") == "prefabs/prefab.prefab.json");
        CHECK(defaultPrefabPath("?!") == "prefabs/prefab.prefab.json");

        // The file is there: making it again is refused, whatever case the name is asked in
        // where the file system does not tell, and nothing is touched.
        fixture.run("entity.rename", R"({"entity": "Front wheel/Hub", "name": "Axle"})");
        const Error refused = fixture.fail("prefab.create", R"({"entity": "Front wheel"})");
        CHECK(refused.code == ErrorCode::AlreadyExists);
        CHECK(refused.message == "prefab.create: 'prefabs/front-wheel.prefab.json' exists already; it is replaced only "
                                 "when that is asked for");
        CHECK(fixture.fail("prefab.create", R"({"entity": "Front wheel", "overwrite": false})").code ==
              ErrorCode::AlreadyExists);
        CHECK(unwrap(readTextFile(file)) == written);

        // Told to, it is written over: the same asset, with what the entity is now.
        fixture.run("asset.instantiate", R"({"asset": "prefabs/front-wheel.prefab.json", "name": "Spare"})");
        const Json replaced = fixture.run("prefab.create", R"({"entity": "Front wheel", "overwrite": true})");
        CHECK(replaced["id"] == made["id"]);
        CHECK(replaced["path"] == "prefabs/front-wheel.prefab.json");
        CHECK(fixture.run("asset.get", R"({"asset": "prefabs/front-wheel.prefab.json"})")["content"]["entities"] ==
              json(R"(["Front wheel", "Axle"])"));
        // What was placed from it before is linked, and takes the new name of its hub - a step
        // of its own.
        CHECK(fixture.context->history().undoLabel() == "Bring 1 prefab instance(s) up to date");
        CHECK(fixture.outline() == std::vector<std::string>{"Front wheel", ".Axle", "Spare", ".Axle"});
        fixture.run("editor.undo");
        CHECK(fixture.outline() == std::vector<std::string>{"Front wheel", ".Axle", "Spare", ".Hub"});
        // The hub's new name was its own, an override; written into the prefab, it is the
        // prefab's, and the step before took the override away.
        CHECK(fixture.context->history().undoLabel() == "Make 'Front wheel' an instance of its prefab");
        CHECK(fixture.world.tryGet<PrefabLinkComponent>(fixture.world.findByName("Axle"))->overrides.empty());
        fixture.run("editor.redo");
        // What is placed from now on is the new one.
        fixture.run("asset.instantiate", R"({"asset": "prefabs/front-wheel.prefab.json", "name": "New"})");
        CHECK(fixture.outline() == std::vector<std::string>{"Front wheel", ".Axle", "Spare", ".Axle", "New", ".Axle"});
        CHECK(marked != serializeScene(fixture.world, fixture.components));
        CHECK(history != fixture.context->history().stateId());
    }

    TEST_CASE("an entity that is an instance of one prefab is made into another, and undoing gives it back its mark") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Lamp"})");
        const Json first = fixture.run("prefab.create", R"({"entity": "Lamp", "path": "prefabs/lamp.prefab.json"})");
        const Json placed =
            fixture.run("asset.instantiate", R"({"asset": "prefabs/lamp.prefab.json", "name": "Lantern"})");
        const World& world = fixture.world;
        const Entity lantern = world.findByName("Lantern");
        CHECK(first["id"] == world.tryGet<PrefabInstanceComponent>(lantern)->prefab.toString());

        const Json second =
            fixture.run("prefab.create", R"({"entity": "Lantern", "path": "prefabs/deep/lantern.prefab.json"})");
        CHECK(second["id"] != first["id"]);
        CHECK(second["path"] == "prefabs/deep/lantern.prefab.json");
        CHECK(second["id"] == world.tryGet<PrefabInstanceComponent>(lantern)->prefab.toString());
        // What it was an instance of is no part of what it is now a prefab of.
        const Json document = unwrap(readJsonFile(fixture.root / "prefabs" / "deep" / "lantern.prefab.json"));
        CHECK_FALSE(document["entities"][0]["components"].contains("PrefabInstance"));

        CHECK(fixture.context->history().undoLabel() == "Make 'Lantern' an instance of its prefab");
        fixture.run("editor.undo");
        CHECK(first["id"] == world.tryGet<PrefabInstanceComponent>(lantern)->prefab.toString());
        fixture.run("editor.redo");
        CHECK(second["id"] == world.tryGet<PrefabInstanceComponent>(lantern)->prefab.toString());
        CHECK(placed["id"] == world.id(lantern).toString());
    }

    TEST_CASE("what is changed of a linked copy is kept as its own, in the step of the change") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        const auto overrides = [&](std::string_view path) {
            const Entity entity = unwrap(fixture.context->resolveEntity(path));
            REQUIRE(fixture.world.has<PrefabLinkComponent>(entity));
            return fixture.world.tryGet<PrefabLinkComponent>(entity)->overrides;
        };
        using Entries = std::vector<std::string>;
        CHECK(overrides("Second/Body/Mirror").empty());

        // A property, with the change, and taken back with it.
        fixture.run(
            "component.set",
            R"({"entity": "Second/Body/Mirror", "component": "Camera", "properties": {"verticalFovDegrees": 40}})");
        CHECK(overrides("Second/Body/Mirror") == Entries{"Camera.verticalFovDegrees"});
        fixture.run("editor.undo");
        CHECK(overrides("Second/Body/Mirror").empty());
        fixture.run("editor.redo");
        CHECK(overrides("Second/Body/Mirror") == Entries{"Camera.verticalFovDegrees"});
        // Changed again, it is listed once.
        fixture.run(
            "component.set",
            R"({"entity": "Second/Body/Mirror", "component": "Camera", "properties": {"verticalFovDegrees": 50}})");
        CHECK(overrides("Second/Body/Mirror") == Entries{"Camera.verticalFovDegrees"});

        // The place of the root, and its name, are its own anyway; those of a copy inside are not.
        fixture.run("component.set",
                    R"({"entity": "Second", "component": "Transform", "properties": {"position": [3, 0, 0]}})");
        fixture.run("entity.rename", R"({"entity": "Second", "name": "Spare"})");
        CHECK(overrides("Spare").empty());
        fixture.run("component.set",
                    R"({"entity": "Spare/Body", "component": "Transform", "properties": {"position": [0, 2, 0]}})");
        fixture.run("entity.rename", R"({"entity": "Spare/Body", "name": "Chassis"})");
        fixture.run("entity.setEnabled", R"({"entity": "Spare/Chassis/Wheel", "enabled": false})");
        CHECK(overrides("Spare/Chassis") == Entries{"Transform.position", "name"});
        CHECK(overrides("Spare/Chassis/Wheel") == Entries{"enabled"});

        // A component added is kept, and removed again it is nothing of its own any more; one
        // the prefab has, removed, is kept left out, and added again it is the prefab's.
        fixture.run("component.add", R"({"entity": "Spare/Chassis/Wheel", "component": "Light"})");
        CHECK(overrides("Spare/Chassis/Wheel") == Entries{"enabled", "+Light"});
        fixture.run("component.remove", R"({"entity": "Spare/Chassis/Wheel", "component": "Light"})");
        CHECK(overrides("Spare/Chassis/Wheel") == Entries{"enabled"});
        fixture.run("component.remove", R"({"entity": "Spare/Chassis/Mirror", "component": "Camera"})");
        CHECK(overrides("Spare/Chassis/Mirror") == Entries{"Camera.verticalFovDegrees", "-Camera"});
        fixture.run("editor.undo");
        CHECK(overrides("Spare/Chassis/Mirror") == Entries{"Camera.verticalFovDegrees"});
        fixture.run("editor.redo");
        fixture.run("component.add", R"({"entity": "Spare/Chassis/Mirror", "component": "Camera"})");
        CHECK(overrides("Spare/Chassis/Mirror") == Entries{"Camera.verticalFovDegrees"});

        // The mark of the instance is no part of what a copy keeps of its own.
        fixture.run("component.remove", R"({"entity": "Spare", "component": "PrefabInstance"})");
        CHECK(overrides("Spare").empty());
        fixture.run("editor.undo");

        // What the prefab was made of is linked as well, and the same holds for it.
        fixture.run("entity.rename", R"({"entity": "cart/Body/Wheel", "name": "Tyre"})");
        CHECK(overrides("cart/Body/Tyre") == Entries{"name"});
        // An entity that is no copy has nothing to keep.
        fixture.run("entity.create", R"({"name": "Loose"})");
        fixture.run("entity.rename", R"({"entity": "Loose", "name": "Free"})");
        CHECK_FALSE(fixture.world.has<PrefabLinkComponent>(fixture.world.findByName("Free")));
    }

    TEST_CASE("a prefab that changes changes its instances when the assets are read again, but for what they keep") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        fixture.run(
            "component.set",
            R"({"entity": "Second/Body/Mirror", "component": "Camera", "properties": {"verticalFovDegrees": 40}})");
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t edited = fixture.context->history().stateId();

        // Nothing changed: nothing to bring up to date, and no step.
        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 0);
        CHECK(fixture.context->history().stateId() == edited);

        // The prefab is changed by another program: its wheel renamed, its mirror wider.
        const std::filesystem::path file = fixture.root / "prefabs" / "cart.prefab.json";
        Json document = unwrap(readJsonFile(file));
        for (Json& entity : document["entities"]) {
            if (entity["name"].get<std::string>() == "Wheel") {
                entity["name"] = "Tyre";
            } else if (entity["name"].get<std::string>() == "Mirror") {
                entity["components"]["Camera"]["verticalFovDegrees"] = 70;
            }
        }
        REQUIRE(writeJsonFile(file, document).has_value());

        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 2);
        CHECK(fixture.outline() ==
              std::vector<std::string>{"cart", ".Body", "..Tyre", "..Mirror", "Second", ".Body", "..Tyre", "..Mirror"});
        const auto fov = [&](std::string_view path) {
            const Entity mirror = unwrap(fixture.context->resolveEntity(path));
            return fixture.world.tryGet<CameraComponent>(mirror)->verticalFovDegrees;
        };
        CHECK(fov("cart/Body/Mirror") == 70.0f);
        // What the copy keeps of its own stays.
        CHECK(fov("Second/Body/Mirror") == 40.0f);

        // One step for all of them, which undo takes back whole.
        CHECK(fixture.context->history().undoLabel() == "Bring 2 prefab instance(s) up to date");
        fixture.run("editor.undo");
        CHECK(serializeScene(fixture.world, fixture.components) == before);
        fixture.run("editor.redo");
        CHECK(fov("cart/Body/Mirror") == 70.0f);
        CHECK(fixture.outline().back() == "..Mirror");

        // While the scene is played, the game is not brought up to date: it is no document.
        REQUIRE(fixture.context->beginPlay().has_value());
        document["entities"][0]["name"] = "Wagon";
        REQUIRE(writeJsonFile(file, document).has_value());
        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 0);
        REQUIRE(fixture.context->endPlay().has_value());
    }

    TEST_CASE("prefab.revert gives a linked copy back what its prefab has, one override or all") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        fixture.run(
            "component.set",
            R"({"entity": "Second/Body/Mirror", "component": "Camera", "properties": {"verticalFovDegrees": 40, "nearPlane": 0.5}})");
        fixture.run("entity.rename", R"({"entity": "Second/Body/Wheel", "name": "Tyre"})");
        fixture.run("component.add", R"({"entity": "Second/Body", "component": "Light"})");
        const Json edited = serializeScene(fixture.world, fixture.components);
        const auto mirror = [&] {
            return fixture.world.tryGet<CameraComponent>(unwrap(fixture.context->resolveEntity("Second/Body/Mirror")));
        };

        // One override.
        const Json reverted = fixture.run(
            "prefab.revert", R"({"entity": "Second/Body/Mirror", "override": "Camera.verticalFovDegrees"})");
        CHECK(reverted["path"] == "Second/Body/Mirror");
        CHECK(reverted["overrides"] == json(R"(["Camera.nearPlane"])"));
        // What the prefab has: what the entity it was made of still has.
        CHECK(mirror()->verticalFovDegrees ==
              fixture.world.tryGet<CameraComponent>(unwrap(fixture.context->resolveEntity("cart/Body/Mirror")))
                  ->verticalFovDegrees);
        CHECK(mirror()->nearPlane == 0.5f);
        CHECK(fixture.context->history().undoLabel() == "Revert Camera.verticalFovDegrees of 'Mirror' to its prefab");
        fixture.run("editor.undo");
        CHECK(serializeScene(fixture.world, fixture.components) == edited);
        fixture.run("editor.redo");

        // All of them, for the whole instance, from its root - but its place, which is its own.
        fixture.run("component.set",
                    R"({"entity": "Second", "component": "Transform", "properties": {"position": [3, 0, 0]}})");
        fixture.run("prefab.revert", R"({"entity": "Second"})");
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror", "Second", ".Body",
                                                            "..Wheel", "..Mirror"});
        CHECK(mirror()->nearPlane == 0.1f);
        CHECK_FALSE(fixture.world.has<LightComponent>(unwrap(fixture.context->resolveEntity("Second/Body"))));
        CHECK(fixture.world.transform(fixture.world.findByName("Second")).position == Vec3(3.0f, 0.0f, 0.0f));
        for (const std::string_view path : {"Second", "Second/Body", "Second/Body/Wheel", "Second/Body/Mirror"}) {
            CHECK(fixture.world.tryGet<PrefabLinkComponent>(unwrap(fixture.context->resolveEntity(path)))
                      ->overrides.empty());
        }

        // What cannot be reverted changes nothing.
        const Json now = serializeScene(fixture.world, fixture.components);
        const uint64_t state = fixture.context->history().stateId();
        CHECK(fixture.fail("prefab.revert", R"({"entity": "Second/Body", "override": "name"})").code ==
              ErrorCode::InvalidArgument);
        fixture.run("entity.create", R"({"name": "Loose"})");
        CHECK(fixture.fail("prefab.revert", R"({"entity": "Loose"})").code == ErrorCode::InvalidArgument);
        fixture.run("editor.undo");
        CHECK(fixture.fail("prefab.revert", R"({"entity": "Nowhere"})").code == ErrorCode::NotFound);
        // A prefab that is gone.
        fixture.run("entity.rename", R"({"entity": "Second/Body", "name": "Chassis"})");
        const Json renamed = serializeScene(fixture.world, fixture.components);
        std::error_code removed;
        REQUIRE(std::filesystem::remove(fixture.root / "prefabs" / "cart.prefab.json", removed));
        fixture.run("asset.refresh");
        CHECK_FALSE(fixture.fail("prefab.revert", R"({"entity": "Second/Chassis"})").message.empty());
        CHECK(serializeScene(fixture.world, fixture.components) == renamed);
        fixture.run("editor.undo");
        CHECK(serializeScene(fixture.world, fixture.components) == now);
        CHECK(fixture.context->history().stateId() == state);
    }

    TEST_CASE("prefab.apply writes what a copy keeps into its prefab, and every instance takes it") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        const std::filesystem::path file = fixture.root / "prefabs" / "cart.prefab.json";
        const auto fov = [&](std::string_view path) {
            return fixture.world.tryGet<CameraComponent>(unwrap(fixture.context->resolveEntity(path)))
                ->verticalFovDegrees;
        };
        const auto overrides = [&](std::string_view path) {
            return fixture.world.tryGet<PrefabLinkComponent>(unwrap(fixture.context->resolveEntity(path)))->overrides;
        };
        fixture.run(
            "component.set",
            R"({"entity": "Second/Body/Mirror", "component": "Camera", "properties": {"verticalFovDegrees": 40}})");
        fixture.run("entity.rename", R"({"entity": "Second/Body/Wheel", "name": "Tyre"})");
        const Json edited = serializeScene(fixture.world, fixture.components);
        const std::string writtenBefore = unwrap(readTextFile(file));

        // One override: the prefab has it, so has the other instance, and the copy keeps
        // nothing of it any more.
        const Json applied =
            fixture.run("prefab.apply", R"({"entity": "Second/Body/Mirror", "override": "Camera.verticalFovDegrees"})");
        CHECK(applied["path"] == "prefabs/cart.prefab.json");
        CHECK(applied["entityCount"] == 4);
        CHECK(fov("cart/Body/Mirror") == 40.0f);
        CHECK(fov("Second/Body/Mirror") == 40.0f);
        CHECK(overrides("Second/Body/Mirror").empty());
        // The wheel's new name is still its own.
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror", "Second", ".Body",
                                                            "..Tyre", "..Mirror"});
        const Json document = unwrap(readJsonFile(file));
        CHECK(document["entities"][3]["components"]["Camera"]["verticalFovDegrees"] == 40.0);

        // One step of the scene, which undo takes back; the file stays as it was written.
        CHECK(fixture.context->history().undoLabel() == "Apply Camera.verticalFovDegrees of 'Mirror' to its prefab");
        fixture.run("editor.undo");
        CHECK(serializeScene(fixture.world, fixture.components) == edited);
        CHECK(unwrap(readTextFile(file)) != writtenBefore);
        fixture.run("editor.redo");
        CHECK(fov("cart/Body/Mirror") == 40.0f);

        // Everything, from the root, with what was added to the instance.
        fixture.run("entity.create", R"({"name": "Lamp", "parent": "Second/Body"})");
        fixture.run("prefab.apply", R"({"entity": "Second"})");
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Tyre", "..Mirror", "..Lamp", "Second",
                                                            ".Body", "..Tyre", "..Mirror", "..Lamp"});
        CHECK(overrides("Second/Body/Tyre").empty());
        CHECK(fixture.run("asset.get", R"({"asset": "prefabs/cart.prefab.json"})")["content"]["entities"] ==
              json(R"(["cart", "Body", "Tyre", "Mirror", "Lamp"])"));
        CHECK(fixture.context->history().undoLabel() == "Apply 'Second' to its prefab");
        // Nothing is left to bring up to date.
        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 0);
    }

    TEST_CASE("a prefab inside a prefab reaches the instances of the outer one when it changes") {
        AssetFixture fixture;
        // A wheel prefab, and a cart prefab made of an entity with two wheels on it.
        fixture.run("entity.create", R"({"name": "Wheel", "components": {"Camera": {"verticalFovDegrees": 30}}})");
        fixture.run("prefab.create", R"({"entity": "Wheel", "path": "prefabs/wheel.prefab.json"})");
        fixture.run("entity.create", R"({"name": "Cart"})");
        fixture.run("asset.instantiate",
                    R"({"asset": "prefabs/wheel.prefab.json", "parent": "Cart", "name": "Front"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/wheel.prefab.json", "parent": "Cart", "name": "Back"})");
        fixture.run("prefab.create", R"({"entity": "Cart", "path": "prefabs/cart.prefab.json"})");
        // The cart's file holds the wheels with their links to the wheel prefab.
        const Json document = unwrap(readJsonFile(fixture.root / "prefabs" / "cart.prefab.json"));
        CHECK(document["entities"][1]["components"]["PrefabLink"]["prefab"] == fixture.id("prefabs/wheel.prefab.json"));
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second cart"})");
        const auto fov = [&](std::string_view path) {
            return fixture.world.tryGet<CameraComponent>(unwrap(fixture.context->resolveEntity(path)))
                ->verticalFovDegrees;
        };

        // The wheel changes: every wheel of every cart takes it.
        fixture.run("component.set",
                    R"({"entity": "Wheel", "component": "Camera", "properties": {"verticalFovDegrees": 45}})");
        fixture.run("prefab.apply", R"({"entity": "Wheel", "override": "Camera.verticalFovDegrees"})");
        CHECK(fov("Cart/Front") == 45.0f);
        CHECK(fov("Second cart/Front") == 45.0f);
        CHECK(fov("Second cart/Back") == 45.0f);
        // The cart's file is as it was: what is inside it is read as its own prefab is now.
        CHECK(unwrap(readJsonFile(fixture.root / "prefabs" / "cart.prefab.json")) == document);
        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 0);
    }

    TEST_CASE("what an entity is to a prefab is what the hierarchy and the inspector show") {
        AssetFixture fixture;
        const World& world = fixture.world;
        fixture.run("entity.create", R"({"name": "Wheel", "components": {"Camera": {}}})");
        fixture.run("prefab.create", R"({"entity": "Wheel", "path": "prefabs/wheel.prefab.json"})");
        fixture.run("entity.create", R"({"name": "Cart"})");
        fixture.run("entity.create", R"({"name": "Axle", "parent": "Cart"})");
        fixture.run("asset.instantiate",
                    R"({"asset": "prefabs/wheel.prefab.json", "parent": "Cart", "name": "Front"})");
        fixture.run("prefab.create", R"({"entity": "Cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        fixture.run("entity.create", R"({"name": "Lamp", "parent": "Second"})");
        fixture.run("component.set",
                    R"({"entity": "Second/Front", "component": "Camera", "properties": {"verticalFovDegrees": 40}})");
        fixture.run("component.add", R"({"entity": "Second/Axle", "component": "Light"})");
        const auto viewOf = [&](std::string_view path) {
            return prefabViewOf(world, unwrap(fixture.context->resolveEntity(path)));
        };
        const Uuid cart = *Uuid::parse(fixture.id("prefabs/cart.prefab.json"));
        const Uuid wheel = *Uuid::parse(fixture.id("prefabs/wheel.prefab.json"));
        const Entity second = world.findByName("Second");

        // The root of an instance, and what is part of it - a prefab inside its prefab among it.
        const PrefabView root = viewOf("Second");
        CHECK(root.role == PrefabView::Role::Root);
        CHECK(root.root == second);
        CHECK(root.prefab == cart);
        CHECK(prefabMarker(root).empty());
        const PrefabView front = viewOf("Second/Front");
        CHECK(front.role == PrefabView::Role::Part);
        CHECK(front.root == second);
        CHECK(front.prefab == cart);
        CHECK(front.innerPrefab == wheel);
        CHECK(front.keepsProperty("Camera", "verticalFovDegrees"));
        CHECK_FALSE(front.keepsProperty("Camera", "nearPlane"));
        CHECK(prefabMarker(front) == "*");
        CHECK(front.leftOut().empty());
        // Its place and its name are its own when changed: it is the root of the wheel inside
        // the cart, not of the instance, whose root alone has those of its own anyway.
        fixture.run("component.set",
                    R"({"entity": "Second/Front", "component": "Transform", "properties": {"position": [0, 1, 0]}})");
        fixture.run("entity.rename", R"({"entity": "Second/Front", "name": "Spare"})");
        CHECK(viewOf("Second/Spare").overrides ==
              std::vector<std::string>{"Camera.verticalFovDegrees", "Transform.position", "name"});
        fixture.run("editor.undo");
        fixture.run("editor.undo");
        CHECK(viewOf("Second/Front").overrides == std::vector<std::string>{"Camera.verticalFovDegrees"});
        fixture.run("component.remove", R"({"entity": "Second/Front", "component": "Camera"})");
        CHECK(viewOf("Second/Front").leftOut() == std::vector<std::string>{"Camera"});
        fixture.run("editor.undo");
        // A component that was added is its own whole.
        const PrefabView axle = viewOf("Second/Axle");
        CHECK(axle.role == PrefabView::Role::Part);
        CHECK(axle.innerPrefab.isNil());
        CHECK(axle.addedComponent("Light"));
        CHECK(axle.keepsProperty("Light", "intensity"));
        CHECK_FALSE(axle.addedComponent("Transform"));
        // What was added to the instance, and what is in no instance.
        const PrefabView lamp = viewOf("Second/Lamp");
        CHECK(lamp.role == PrefabView::Role::Added);
        CHECK(lamp.root == second);
        CHECK(lamp.prefab == cart);
        CHECK(prefabMarker(lamp) == "+");
        fixture.run("entity.create", R"({"name": "Loose"})");
        CHECK(viewOf("Loose").role == PrefabView::Role::None);
        CHECK(prefabViewOf(world, Entity{}).role == PrefabView::Role::None);

        // The lines the panels hand on run as they are.
        const auto runLine = [&](const std::string& line) {
            const size_t blank = line.find(' ');
            REQUIRE(blank != std::string::npos);
            return fixture.run(line.substr(0, blank), line.substr(blank + 1));
        };
        const Entity frontEntity = unwrap(fixture.context->resolveEntity("Second/Front"));
        const std::string revert = prefabCommandLine("prefab.revert", world, frontEntity, "Camera.verticalFovDegrees");
        CHECK(revert == fmt::format(R"(prefab.revert {{"entity":"{}","override":"Camera.verticalFovDegrees"}})",
                                    world.id(frontEntity).toString()));
        runLine(revert);
        CHECK(viewOf("Second/Front").overrides.empty());
        runLine(prefabCommandLine("prefab.apply", world, second));
        CHECK(viewOf("Second/Lamp").role == PrefabView::Role::Part);
        CHECK(viewOf("Second/Axle").overrides.empty());
    }

    TEST_CASE("an entity of a prefab deleted from an instance stays deleted, and is reverted or applied") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/cart.prefab.json", "name": "Second"})");
        const auto rootView = [&] { return prefabViewOf(fixture.world, fixture.world.findByName("Second")); };
        const std::vector<std::string> placed = fixture.outline();

        // Deleted: the root keeps it, in the step of the deletion, and reading the assets again
        // does not make it again.
        fixture.run("entity.delete", R"({"entities": ["Second/Body/Wheel"]})");
        CHECK(rootView().deletedEntities() == 1);
        CHECK(rootView().describeOverrides() == "1 deleted entity");
        CHECK(rootView().leftOut().empty());
        CHECK(fixture.run("asset.refresh")["prefabInstancesUpdated"] == 0);
        CHECK(fixture.outline() ==
              std::vector<std::string>{"cart", ".Body", "..Wheel", "..Mirror", "Second", ".Body", "..Mirror"});
        fixture.run("editor.undo");
        CHECK(fixture.outline() == placed);
        CHECK(rootView().overrides.empty());

        // Two in one step, and undone whole.
        fixture.run("entity.delete", R"({"entities": ["Second/Body/Wheel", "Second/Body/Mirror"]})");
        CHECK(rootView().deletedEntities() == 2);
        CHECK(rootView().describeOverrides() == "2 deleted entities");
        fixture.run("editor.undo");
        CHECK(fixture.outline() == placed);
        CHECK(rootView().overrides.empty());

        // Reverted, it is back.
        fixture.run("entity.delete", R"({"entities": ["Second/Body/Wheel"]})");
        const std::string entry = rootView().overrides.front();
        fixture.run("prefab.revert", fmt::format(R"({{"entity": "Second", "override": "{}"}})", entry));
        CHECK(fixture.outline() == placed);
        CHECK(rootView().overrides.empty());

        // Applied, the prefab has it no more, and neither has the other instance.
        fixture.run("entity.delete", R"({"entities": ["Second/Body/Mirror"]})");
        fixture.run("prefab.apply", R"({"entity": "Second"})");
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Body", "..Wheel", "Second", ".Body", "..Wheel"});
        CHECK(fixture.run("asset.get", R"({"asset": "prefabs/cart.prefab.json"})")["content"]["entities"] ==
              json(R"(["cart", "Body", "Wheel"])"));
        CHECK(rootView().overrides.empty());
    }

    TEST_CASE("a prefab is not made to hold itself, and nothing is written") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Box"})");
        fixture.run("prefab.create", R"({"entity": "Box", "path": "prefabs/box.prefab.json"})");
        fixture.run("asset.instantiate", R"({"asset": "prefabs/box.prefab.json", "parent": "Box", "name": "Inner"})");
        const std::filesystem::path file = fixture.root / "prefabs" / "box.prefab.json";
        const std::string written = unwrap(readTextFile(file));
        const Json before = serializeScene(fixture.world, fixture.components);

        // Applied, the box would hold a box.
        const Error applied = fixture.fail("prefab.apply", R"({"entity": "Box"})");
        CHECK(applied.code == ErrorCode::InvalidArgument);
        CHECK(applied.message.find("'prefabs/box.prefab.json' holds 'prefabs/box.prefab.json'") != std::string::npos);
        // And made anew over its own file, too.
        CHECK(
            fixture.fail("prefab.create", R"({"entity": "Box", "path": "prefabs/box.prefab.json", "overwrite": true})")
                .code == ErrorCode::InvalidArgument);
        CHECK(unwrap(readTextFile(file)) == written);
        CHECK(serializeScene(fixture.world, fixture.components) == before);
    }

    TEST_CASE("prefab.apply refuses what it cannot apply, and writes nothing") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        fixture.run("entity.rename", R"({"entity": "cart/Body/Wheel", "name": "Tyre"})");
        fixture.run("entity.create", R"({"name": "Loose"})");
        const std::filesystem::path file = fixture.root / "prefabs" / "cart.prefab.json";
        const std::string written = unwrap(readTextFile(file));
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t state = fixture.context->history().stateId();

        CHECK(fixture.fail("prefab.apply", R"({"entity": "Loose"})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("prefab.apply", R"({"entity": "cart/Body/Tyre", "override": "enabled"})").code ==
              ErrorCode::InvalidArgument);
        CHECK(fixture.fail("prefab.apply", R"({"entity": "Nowhere"})").code == ErrorCode::NotFound);
        REQUIRE(fixture.context->beginPlay().has_value());
        CHECK(fixture.fail("prefab.apply", R"({"entity": "cart/Body/Tyre"})").code == ErrorCode::InvalidState);
        REQUIRE(fixture.context->endPlay().has_value());

        CHECK(unwrap(readTextFile(file)) == written);
        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == state);
    }

    TEST_CASE("a scene that is opened takes what its prefabs were changed to, as changes that are not saved") {
        AssetFixture fixture;
        fixture.run("asset.instantiate", R"({"asset": "models/cart.gltf"})");
        fixture.run("prefab.create", R"({"entity": "cart", "path": "prefabs/cart.prefab.json"})");
        const std::filesystem::path scene = fixture.root / "scenes" / "carts.scene.json";
        fixture.run("scene.save", Json{{"path", pathToUtf8(scene)}}.dump());

        const std::filesystem::path file = fixture.root / "prefabs" / "cart.prefab.json";
        Json document = unwrap(readJsonFile(file));
        document["entities"][1]["name"] = "Chassis";
        REQUIRE(writeJsonFile(file, document).has_value());
        fixture.run("asset.refresh");
        fixture.run("scene.new");

        // Without the prefab it is opened as it was; with it, as the prefab is now.
        fixture.run("scene.open", Json{{"path", pathToUtf8(scene)}}.dump());
        CHECK(fixture.outline() == std::vector<std::string>{"cart", ".Chassis", "..Wheel", "..Mirror"});
        CHECK(fixture.run("editor.state")["scene"]["unsavedChanges"] == true);
        CHECK_FALSE(fixture.context->history().canUndo());

        // Saved, and opened again, there is nothing to change.
        fixture.run("scene.save");
        fixture.run("scene.open", Json{{"path", pathToUtf8(scene)}}.dump());
        CHECK(fixture.run("editor.state")["scene"]["unsavedChanges"] == false);
    }

    TEST_CASE("prefab.create refuses what it cannot make a prefab of, and writes nothing") {
        AssetFixture fixture;
        fixture.run("entity.create", R"({"name": "Yard"})");
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t history = fixture.context->history().stateId();
        const size_t assetCount = fixture.assets->database().assets().size();
        // A directory next to the assets, where nothing of them belongs.
        const std::filesystem::path outside = fixture.directory.path() / "outside.prefab.json";

        SUBCASE("an entity that does not exist") {
            CHECK(fixture.fail("prefab.create", R"({"entity": "Nowhere"})").code == ErrorCode::NotFound);
        }
        SUBCASE("no entity at all") {
            CHECK(fixture.fail("prefab.create").code == ErrorCode::InvalidArgument);
        }
        SUBCASE("a path that leaves the asset directory") {
            for (const std::string& path : {std::string("../outside.prefab.json"),
                                            std::string("prefabs/../../outside.prefab.json"), pathToUtf8(outside)}) {
                CAPTURE(path);
                Json parameters = json(R"({"entity": "Yard"})");
                parameters["path"] = path;
                const Error error = fixture.fail("prefab.create", parameters.dump());
                CHECK(error.code == ErrorCode::InvalidArgument);
                CHECK(error.message.find("is outside") != std::string::npos);
            }
        }
        SUBCASE("a file that would be no prefab") {
            for (const char* file : {"prefabs/yard.json", "prefabs/yard.prefab", "prefabs/yard.scene.json",
                                     "prefabs/.prefab.json", "prefabs"}) {
                const std::string path(file);
                CAPTURE(path);
                const Error error =
                    fixture.fail("prefab.create", fmt::format(R"({{"entity": "Yard", "path": "{}"}})", path));
                CHECK(error.code == ErrorCode::InvalidArgument);
                CHECK(error.message ==
                      fmt::format("prefab.create: the name of a prefab file ends in \".prefab.json\"; got '{}'", path));
            }
        }
        SUBCASE("a scene that is being played") {
            REQUIRE(fixture.context->beginPlay().has_value());
            const Error error = fixture.fail("prefab.create", R"({"entity": "Yard"})");
            CHECK(error.code == ErrorCode::InvalidState);
            CHECK(error.message.find("the scene is being played") != std::string::npos);
            REQUIRE(fixture.context->endPlay().has_value());
        }
        SUBCASE("more entities than a prefab may have") {
            // One too many - and, below, exactly as many as there may be.
            World& world = fixture.world;
            const Entity heap = world.createEntity("Heap");
            for (size_t index = 1; index < kMaxPrefabEntities + 1; ++index) {
                REQUIRE(world.setParent(world.createEntity("Grain"), heap).has_value());
            }
            const Json heaped = serializeScene(world, fixture.components);
            const Error error = fixture.fail("prefab.create", R"({"entity": "Heap"})");
            CHECK(error.code == ErrorCode::InvalidArgument);
            CHECK(error.message ==
                  "prefab.create: 'Heap' is 10001 entities with what is below it; a prefab may have 10000 at most");
            CHECK(serializeScene(world, fixture.components) == heaped);
            CHECK_FALSE(directoryExists(fixture.root / "prefabs"));

            world.destroyEntity(world.children(heap).back());
            CHECK(fixture.run("prefab.create", R"({"entity": "Heap"})")["entityCount"] == kMaxPrefabEntities);
            return;
        }

        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == history);
        CHECK_FALSE(directoryExists(fixture.root / "prefabs"));
        CHECK_FALSE(fileExists(outside));
        REQUIRE(fixture.assets->refresh().has_value());
        CHECK(fixture.assets->database().assets().size() == assetCount);
    }

    TEST_CASE("a prefab that cannot be placed says which file is wrong, and changes nothing") {
        AssetFixture fixture;
        const char* twoRoots = R"({"format": "cordite.prefab", "version": 1, "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "One", "enabled": true, "parent": null,
             "components": {}},
            {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Two", "enabled": true, "parent": null,
             "components": {}}]})";
        const char* unknownComponent = R"({"format": "cordite.prefab", "version": 1, "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "One", "enabled": true, "parent": null,
             "components": {"Engine": {}}}]})";
        fixture.writeText("prefabs/two.prefab.json", twoRoots);
        fixture.writeText("prefabs/engine.prefab.json", unknownComponent);
        fixture.writeText("prefabs/words.prefab.json", "these are words");
        REQUIRE(fixture.assets->refresh().has_value());
        fixture.run("entity.create", R"({"name": "Yard"})");
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t history = fixture.context->history().stateId();

        Error error = fixture.fail("asset.instantiate", R"({"asset": "prefabs/two.prefab.json"})");
        CHECK(error.code == ErrorCode::ParseError);
        CHECK(error.message == "asset.instantiate: prefabs/two.prefab.json: the entities must have one root with all "
                               "the others below it, but 2 of them have no parent among them");
        error = fixture.fail("asset.instantiate", R"({"asset": "prefabs/engine.prefab.json", "parent": "Yard"})");
        CHECK(error.message.starts_with("asset.instantiate: prefabs/engine.prefab.json: entity 'One' "));
        CHECK(error.message.find("Engine") != std::string::npos);
        error = fixture.fail("asset.instantiate", R"({"asset": "prefabs/words.prefab.json"})");
        CHECK(error.code == ErrorCode::ParseError);
        CHECK(error.message.find("words.prefab.json") != std::string::npos);
        // The things asset.instantiate refuses for a model, it refuses for a prefab.
        fixture.writeText("prefabs/one.prefab.json",
                          R"({"format": "cordite.prefab", "version": 1, "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "One", "enabled": true, "parent": null,
             "components": {}}]})");
        REQUIRE(fixture.assets->refresh().has_value());
        CHECK(fixture.fail("asset.instantiate", R"({"asset": "prefabs/one.prefab.json", "parent": "Nowhere"})").code ==
              ErrorCode::NotFound);
        CHECK(fixture.fail("asset.instantiate", R"({"asset": "prefabs/one.prefab.json", "name": "a/b"})").code ==
              ErrorCode::InvalidArgument);

        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == history);
        // And the one that is right is placed, after all of that.
        CHECK(fixture.run("asset.instantiate", R"({"asset": "prefabs/one.prefab.json"})")["entityCount"] == 1);
    }

    TEST_CASE("asset.get says what is wrong with a prefab, and what it uses, without placing it in the scene") {
        AssetFixture fixture;
        // A part whose mesh is no asset, and whose material is a texture; and a prefab
        // that is none inside.
        Json lost = json(R"({"format": "cordite.prefab", "version": 1, "name": "Lost", "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "One", "enabled": true, "parent": null,
             "components": {"MeshRenderer": {"mesh": null, "materials": []}}},
            {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Part", "enabled": true,
             "parent": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
             "components": {"MeshRenderer": {"mesh": "11111111-1111-4111-8111-111111111111", "materials": []}}}]})");
        lost["entities"][0]["components"]["MeshRenderer"]["mesh"] = fixture.id("models/cart.gltf#mesh/1");
        lost["entities"][0]["components"]["MeshRenderer"]["materials"] =
            Json::array({fixture.id("models/cart.gltf#material/0")});
        lost["entities"][1]["components"]["MeshRenderer"]["materials"] =
            Json::array({fixture.id("textures/wall.png"), fixture.id("models/cart.gltf#material/0")});
        fixture.writeText("prefabs/lost.prefab.json", dumpJson(lost));
        fixture.writeText("prefabs/two.prefab.json", R"({"format": "cordite.prefab", "version": 1, "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "One", "enabled": true, "parent": null,
             "components": {}},
            {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Two", "enabled": true, "parent": null,
             "components": {}}]})");
        REQUIRE(fixture.assets->refresh().has_value());
        fixture.run("entity.create", R"({"name": "Yard"})");
        const Json before = serializeScene(fixture.world, fixture.components);
        const uint64_t history = fixture.context->history().stateId();

        Json content = fixture.run("asset.get", R"({"asset": "prefabs/lost.prefab.json"})")["content"];
        CHECK(content["name"] == "Lost");
        CHECK(content["entities"] == json(R"(["One", "Part"])"));
        // What it uses and is there, in the order in which its entities first use it, and
        // each once: the paint is on both entities. The prefab itself is not among them.
        CHECK(pathsOf(content["assets"]) ==
              std::vector<std::string>{"models/cart.gltf#mesh/1", "models/cart.gltf#material/0", "textures/wall.png"});
        // What is wrong is said for the entity it is wrong with, by where it is in the prefab.
        REQUIRE(content["problems"].size() == 2);
        CHECK(content["problems"][0]["entity"] == "One/Part");
        CHECK(content["problems"][0]["problem"] ==
              "MeshRenderer.mesh refers to the asset 11111111-1111-4111-8111-111111111111, which does not exist");
        CHECK(content["problems"][1]["entity"] == "One/Part");
        CHECK(content["problems"][1]["problem"] ==
              "MeshRenderer.materials[0] refers to textures/wall.png, which is a texture, not a material");

        // One that cannot be placed at all says why, and uses nothing.
        content = fixture.run("asset.get", R"({"asset": "prefabs/two.prefab.json"})")["content"];
        CHECK(content["entityCount"] == 2);
        CHECK(content["assets"].empty());
        REQUIRE(content["problems"].size() == 1);
        CHECK_FALSE(content["problems"][0].contains("entity"));
        CHECK(content["problems"][0]["problem"] == "the entities must have one root with all the others below it, but "
                                                   "2 of them have no parent among them");

        // Asking changes nothing: the scene has none of it, and its history no step more.
        CHECK(serializeScene(fixture.world, fixture.components) == before);
        CHECK(fixture.context->history().stateId() == history);
    }

    TEST_CASE("an editor without a project has nowhere to keep a prefab") {
        World world;
        ComponentRegistry components;
        REQUIRE(registerEngineComponents(components).has_value());
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        EditorContext context(world, components, *assets);
        const Entity lamp = world.createEntity("Lamp");

        const Result<Uuid> made = EditorActions(context).createPrefab(lamp);
        REQUIRE_FALSE(made.has_value());
        CHECK(made.error().code == ErrorCode::InvalidState);
        CHECK(made.error().message == "there is no project open to keep a prefab in");
        CHECK_FALSE(world.has<PrefabInstanceComponent>(lamp));
        CHECK(context.history().undoCount() == 0);

        // An entity that is none is no better anywhere.
        world.destroyEntity(lamp);
        CHECK(EditorActions(context).createPrefab(lamp).error().code == ErrorCode::InvalidArgument);
        CHECK(EditorActions(context).instantiatePrefab(Uuid::fromName(Uuid(), "none")).error().code ==
              ErrorCode::NotFound);
    }

    TEST_CASE("an editor that writes ids down writes the one of a new prefab down, and keeps it") {
        AssetFixture fixture;
        // An editor: its assets are told to write a .meta file for what has none.
        fixture.assets = unwrap(AssetManager::create({.rootDirectory = fixture.root, .writeMetaFiles = true}));
        fixture.context = std::make_unique<EditorContext>(fixture.world, fixture.components, *fixture.assets);
        CommandRegistry commands;
        REQUIRE(registerEditorCommands(commands, *fixture.context).has_value());
        const auto run = [&](std::string_view name, std::string_view parameters) {
            return unwrap(commands.execute(name, json(parameters)));
        };

        run("entity.create", R"({"name": "Lamp"})");
        const Json made = run("prefab.create", R"({"entity": "Lamp", "path": "prefabs/lamp.prefab.json"})");
        const std::filesystem::path meta = fixture.root / "prefabs" / "lamp.prefab.json.meta";
        REQUIRE(fileExists(meta));
        CHECK(unwrap(readJsonFile(meta))["id"] == made["id"]);

        // Moved with its .meta, the prefab is the same asset somewhere else - and what was
        // placed from it still names it.
        REQUIRE(ensureDirectory(fixture.root / "props").has_value());
        std::filesystem::rename(fixture.root / "prefabs" / "lamp.prefab.json",
                                fixture.root / "props" / "lamp.prefab.json");
        std::filesystem::rename(meta, fixture.root / "props" / "lamp.prefab.json.meta");
        run("asset.refresh", "{}");
        CHECK(made["id"] == fixture.id("props/lamp.prefab.json"));
        CHECK(run("scene.validate", "{}")["valid"] == true);
        CHECK(run("asset.instantiate", fmt::format(R"({{"asset": "{}"}})", made["id"].get<std::string>()))["name"] ==
              "Lamp");
    }

    TEST_CASE("assets.browse says what the Assets panel lists, and asking changes nothing") {
        AssetFixture fixture;
        const uint64_t revision = fixture.browser.revision();
        const Json listed = fixture.run("assets.browse");

        CHECK(listed["type"].is_null());
        CHECK(listed["search"] == "");
        CHECK(listed["selected"].is_null());
        // Directories open, the model closed over what is inside it, and what is built in last.
        CHECK(browserLines(listed["rows"]) == std::vector<std::string>{"materials/",
                                                                       "  paint.material.json (material)",
                                                                       "models/",
                                                                       "  cart.gltf (model) ...",
                                                                       "scenes/",
                                                                       "  yard.scene.json (scene)",
                                                                       "scripts/",
                                                                       "  gate.lua (script)",
                                                                       "sounds/",
                                                                       "  creak.wav (sound)",
                                                                       "textures/",
                                                                       "  wall.png (texture)",
                                                                       "builtin/",
                                                                       "  Cube (mesh)",
                                                                       "  Sphere (mesh)",
                                                                       "  Plane (mesh)",
                                                                       "  Cylinder (mesh)",
                                                                       "  Cone (mesh)",
                                                                       "  Capsule (mesh)",
                                                                       "  Torus (mesh)",
                                                                       "  Default (material)"});
        // A row says what the other commands need to hear.
        const Json& cart = listed["rows"][3];
        CHECK(cart["kind"] == "asset");
        CHECK(cart["depth"] == 1);
        CHECK(cart["name"] == "cart.gltf");
        CHECK(cart["path"] == "models/cart.gltf");
        CHECK(cart["id"] == fixture.id("models/cart.gltf"));
        CHECK(cart["type"] == "model");
        CHECK(cart["matches"] == true);
        CHECK(cart["open"] == false);
        CHECK_FALSE(cart.contains("selected"));
        const Json& models = listed["rows"][2];
        CHECK(models["kind"] == "directory");
        CHECK(models["path"] == "models");
        CHECK(models["open"] == true);
        CHECK_FALSE(models.contains("id"));
        // What has nothing below it is neither open nor closed.
        CHECK_FALSE(listed["rows"][1].contains("open"));

        // What the panel shows: the tree, with the top shown, and what the top has in it, as
        // a grid.
        CHECK(listed["view"] == "grid");
        CHECK(listed["folder"] == "");
        CHECK(browserLines(listed["folders"]) == std::vector<std::string>{"Assets/ *", "  materials/", "  models/",
                                                                          "  scenes/", "  scripts/", "  sounds/",
                                                                          "  textures/", "  builtin/"});
        CHECK(listed["folders"][0]["path"] == "");
        CHECK(listed["folders"][7]["path"] == "builtin");
        CHECK(browserLines(listed["contents"]) == std::vector<std::string>{"materials/", "models/", "scenes/",
                                                                           "scripts/", "sounds/", "textures/",
                                                                           "builtin/"});
        CHECK(listed["contents"][1]["path"] == "models");

        CHECK(fixture.browser.revision() == revision);
    }

    TEST_CASE("assets.browse shows a folder by its path, as a list or as a grid") {
        AssetFixture fixture;
        Json listed = fixture.run("assets.browse", R"({"folder": "models", "view": "LIST"})");
        CHECK(listed["folder"] == "models");
        CHECK(listed["view"] == "list");
        CHECK(fixture.browser.view() == AssetBrowserView::List);
        CHECK(browserLines(listed["contents"]) == std::vector<std::string>{"cart.gltf (model) ..."});
        CHECK(browserLines(listed["folders"])[2] == "  models/ *");
        // A file is opened among the contents as it is among the rows.
        listed = fixture.run("assets.browse", R"({"open": "models/cart.gltf"})");
        CHECK(browserLines(listed["contents"]) ==
              std::vector<std::string>{"cart.gltf (model)", "  Body (mesh)", "  Wheel (mesh)", "  Paint (material)"});

        // What the engine has built in, and the top again - with blanks around it.
        listed = fixture.run("assets.browse", R"({"folder": "builtin", "view": "grid"})");
        CHECK(listed["folder"] == "builtin");
        CHECK(listed["view"] == "grid");
        CHECK(listed["contents"].size() == 8);
        CHECK(listed["folders"].back().contains("selected"));
        listed = fixture.run("assets.browse", R"({"folder": " "})");
        CHECK(listed["folder"] == "");

        // A search looks in the folder shown and below it, side by side.
        listed = fixture.run("assets.browse", R"({"folder": "models", "type": "mesh"})");
        CHECK(browserLines(listed["contents"]) == std::vector<std::string>{"Body (mesh)", "Wheel (mesh)"});
        // The folder asked for is shown, also when an asset is chosen elsewhere.
        listed = fixture.run("assets.browse", R"({"type": "all", "select": "textures/wall.png", "folder": "sounds"})");
        CHECK(listed["folder"] == "sounds");
        CHECK(listed["selected"]["path"] == "textures/wall.png");
        CHECK(browserLines(listed["contents"]) == std::vector<std::string>{"creak.wav (sound)"});
        // Without a folder, choosing an asset shows the folder it is in.
        listed = fixture.run("assets.browse", R"({"select": "models/cart.gltf#mesh/0"})");
        CHECK(listed["folder"] == "models");
        CHECK(browserLines(listed["contents"])[1] == "  Body (mesh) *");
    }

    TEST_CASE("assets.browse narrows what is listed, chooses an asset and opens what it is in") {
        AssetFixture fixture;

        // Of one type, with a text: the model is there for the wheel inside it.
        Json listed = fixture.run("assets.browse", R"({"type": "mesh", "search": " WH "})");
        CHECK(listed["type"] == "mesh");
        CHECK(listed["search"] == "WH");
        CHECK(browserLines(listed["rows"]) ==
              std::vector<std::string>{"models/", "  cart.gltf (model) -", "    Wheel (mesh)"});
        CHECK(fixture.browser.filter() == AssetBrowserFilter{.type = AssetType::Mesh, .search = "WH"});
        // What is not said stays: the text goes, the type does not.
        listed = fixture.run("assets.browse", R"({"search": ""})");
        CHECK(listed["type"] == "mesh");
        CHECK(listed["rows"].size() == 12);
        // And every type again.
        listed = fixture.run("assets.browse", R"({"type": "all"})");
        CHECK(listed["type"].is_null());
        CHECK(listed["rows"].size() == 21);

        // An asset is chosen by where it is or by its id, and what it is in is opened for
        // it: the file shows what is inside it - three rows more - and which is chosen.
        listed = fixture.run("assets.browse", R"({"select": "models/cart.gltf#mesh/1"})");
        CHECK(listed["selected"]["path"] == "models/cart.gltf#mesh/1");
        CHECK(listed["selected"]["id"] == fixture.id("models/cart.gltf#mesh/1"));
        const std::vector<std::string> opened = browserLines(listed["rows"]);
        REQUIRE(opened.size() == 24);
        CHECK(std::vector<std::string>(opened.begin() + 2, opened.begin() + 8) ==
              std::vector<std::string>{"models/", "  cart.gltf (model)", "    Body (mesh)", "    Wheel (mesh) *",
                                       "    Paint (material)", "scenes/"});
        // Closed again by hand, the file hides it, and it is chosen still; opened, there it is.
        listed = fixture.run("assets.browse", R"({"close": "models/cart.gltf"})");
        CHECK(browserLines(listed["rows"])[3] == "  cart.gltf (model) ...");
        CHECK(listed["selected"]["path"] == "models/cart.gltf#mesh/1");
        listed = fixture.run("assets.browse", R"({"open": "models/cart.gltf"})");
        CHECK(browserLines(listed["rows"])[5] == "    Wheel (mesh) *");
        // An asset the filter leaves out is chosen all the same, and given its place: the
        // folder it is in is shown, and the file it is inside opened, to be seen once the
        // filter lists it.
        fixture.run("assets.browse", R"({"close": "models/cart.gltf", "folder": ""})");
        listed = fixture.run("assets.browse", R"({"type": "texture", "select": "models/cart.gltf#mesh/0"})");
        CHECK(listed["selected"]["path"] == "models/cart.gltf#mesh/0");
        CHECK(listed["folder"] == "models");
        CHECK(browserLines(listed["rows"]) == std::vector<std::string>{"textures/", "  wall.png (texture)"});
        CHECK(listed["contents"].empty());
        listed = fixture.run("assets.browse", R"({"type": "all"})");
        CHECK(browserLines(listed["rows"])[4] == "    Body (mesh) *");
        CHECK(browserLines(listed["contents"])[1] == "  Body (mesh) *");
        listed = fixture.run("assets.browse",
                             fmt::format(R"({{"select": "{}", "close": "models"}})", fixture.id("textures/wall.png")));
        CHECK(listed["selected"]["path"] == "textures/wall.png");
        CHECK(browserLines(listed["rows"])[2] == "models/ ...");
        CHECK(browserLines(listed["rows"])[10] == "  wall.png (texture) *");
        // What is built in is a directory to close like the others; and none is chosen.
        listed = fixture.run("assets.browse", R"({"close": "builtin", "select": ""})");
        CHECK(listed["selected"].is_null());
        CHECK(browserLines(listed["rows"]).back() == "builtin/ ...");
        listed = fixture.run("assets.browse", R"({"open": "models"})");
        // The file in the directory is as it was left: open.
        CHECK(browserLines(listed["rows"])[4] == "    Body (mesh)");
    }

    TEST_CASE("assets.browse that asks for what is not there changes nothing of what the panel lists") {
        AssetFixture fixture;
        fixture.run("assets.browse", R"({"search": "cart", "select": "textures/wall.png"})");
        const AssetBrowserFilter filter = fixture.browser.filter();
        const Uuid selection = fixture.browser.selection();
        const uint64_t revision = fixture.browser.revision();

        Error error = fixture.fail("assets.browse", R"({"type": "font"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message ==
              "assets.browse: 'font' is not an asset type; use \"all\", texture, mesh, material, model, "
              "scene, skeleton, animation, script, sound, prefab, renderTarget or sequence");
        error = fixture.fail("assets.browse", R"({"select": "models/none.gltf"})");
        CHECK(error.code == ErrorCode::NotFound);
        // What can be opened is what has something below it, among what is listed.
        for (const char* path : {"nowhere", "textures/wall.png", "models/cart.gltf#mesh/0", "textures", ""}) {
            CAPTURE(std::string(path));
            for (const char* which : {"open", "close"}) {
                error = fixture.fail("assets.browse", fmt::format(R"({{"{}": "{}"}})", which, path));
                CHECK(error.code == ErrorCode::NotFound);
                CHECK(error.message == fmt::format("assets.browse: the assets that are listed have no directory and no "
                                                   "file '{}' with something in it",
                                                   path));
            }
        }
        CHECK(fixture.fail("assets.browse", R"({"type": 5})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("assets.browse", R"({"sort": "name"})").code == ErrorCode::InvalidArgument);
        // A folder is a directory with an asset in it, the top or what is built in.
        for (const char* path : {"nowhere", "textures/wall.png", "models/cart.gltf", "/textures", "#builtin"}) {
            CAPTURE(std::string(path));
            error = fixture.fail("assets.browse", fmt::format(R"({{"folder": "{}"}})", path));
            CHECK(error.code == ErrorCode::NotFound);
            CHECK(error.message == fmt::format("assets.browse: the assets have no folder '{}': a folder is a directory "
                                               "with an asset in it or below it, \"\" the top and \"builtin\" what "
                                               "the engine has built in",
                                               path));
        }
        error = fixture.fail("assets.browse", R"({"view": "tiles"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "assets.browse: 'tiles' is not a view of the assets; use list or grid");
        CHECK(fixture.fail("assets.browse", R"({"folder": 3})").code == ErrorCode::InvalidArgument);

        // The first half of a request is not done when the second cannot be.
        const AssetBrowserView view = fixture.browser.view();
        const std::string folder = fixture.browser.folder();
        error = fixture.fail("assets.browse", R"({"type": "mesh", "search": "", "select": "", "open": "nowhere"})");
        CHECK(error.code == ErrorCode::NotFound);
        error = fixture.fail("assets.browse", R"({"folder": "models", "view": "tiles"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        error = fixture.fail("assets.browse", R"({"view": "list", "folder": "models", "close": "nowhere"})");
        CHECK(error.code == ErrorCode::NotFound);
        CHECK(fixture.browser.filter() == filter);
        CHECK(fixture.browser.selection() == selection);
        CHECK(fixture.browser.view() == view);
        CHECK(fixture.browser.folder() == folder);
        CHECK(fixture.browser.revision() == revision);
    }

    TEST_CASE("what the Assets panel asks for is said as commands that do it") {
        AssetFixture fixture;
        // A line as the console takes it: a name, and its parameters as JSON.
        const auto runLine = [&fixture](const std::string& line) {
            const size_t blank = line.find(' ');
            REQUIRE(blank != std::string::npos);
            return fixture.run(line.substr(0, blank), line.substr(blank + 1));
        };
        const Uuid cart = *Uuid::parse(fixture.id("models/cart.gltf"));
        fixture.run("entity.create", R"({"name": "Yard"})");
        const std::string yard = fixture.world.id(fixture.world.findByName("Yard")).toString();

        // Nothing is selected: what is placed is a root.
        fixture.run("selection.set", R"({"entities": []})");
        std::string line = placeAssetCommand(*fixture.context, cart);
        CHECK(line == fmt::format(R"(asset.instantiate {{"asset":"{}"}})", cart.toString()));
        runLine(line);
        CHECK(fixture.outline() == std::vector<std::string>{"Yard", "cart", ".Body", "..Wheel", "..Mirror"});

        // Something is selected: new things go under it, as a new entity does.
        fixture.run("selection.set", R"({"entities": ["Yard"]})");
        line = placeAssetCommand(*fixture.context, cart);
        CHECK(line == fmt::format(R"(asset.instantiate {{"asset":"{}","parent":"{}"}})", cart.toString(), yard));
        const Json placed = runLine(line);
        CHECK(placed["path"] == "Yard/cart");
        CHECK(fixture.world.entityCount() == 9);

        // A scene is opened by where it is among the assets - which is no path on the disk,
        // and needs no escaping whatever the name of the directory of the project is.
        const AssetInfo* scene = fixture.assets->database().findByReference("scenes/yard.scene.json");
        REQUIRE(scene != nullptr);
        line = openSceneCommand(*scene);
        CHECK(line == R"(scene.open {"path":"scenes/yard.scene.json"})");
        runLine(line);
        CHECK(fixture.world.sceneName() == "Yard");
        CHECK(fixture.world.entityCount() == 0);

        // What can be placed, and so has a line to place it by.
        CHECK(canBePlaced(AssetType::Model));
        CHECK(canBePlaced(AssetType::Prefab));
        CHECK(canBePlaced(AssetType::Mesh));
        for (const AssetType type :
             {AssetType::Texture, AssetType::Material, AssetType::Scene, AssetType::Skeleton, AssetType::Animation,
              AssetType::Script, AssetType::Sound, AssetType::RenderTarget, AssetType::Sequence}) {
            CHECK_FALSE(canBePlaced(type));
        }
    }

    TEST_CASE("what is dropped onto a viewport is said as a command that places it, or that gives a mesh a material") {
        AssetFixture fixture;
        const auto runLine = [&fixture](const std::string& line) {
            const size_t blank = line.find(' ');
            REQUIRE(blank != std::string::npos);
            return fixture.run(line.substr(0, blank), line.substr(blank + 1));
        };
        World& world = fixture.world;
        const AssetDatabase& database = fixture.assets->database();
        const AssetInfo* cube = database.find(builtinMeshId(BuiltinMesh::Cube));
        const AssetInfo* paint = database.findByReference("materials/paint.material.json");
        const AssetInfo* wall = database.findByReference("textures/wall.png");
        REQUIRE(cube != nullptr);
        REQUIRE(paint != nullptr);
        REQUIRE(wall != nullptr);
        const DropReach reach{.grid = 50.0f, .ahead = 10.0f};
        // From ten metres up and five back, down at the origin: it meets the grid there.
        const Ray ray{Vec3(0.0f, 10.0f, 5.0f), glm::normalize(Vec3(0.0f, -2.0f, -1.0f))};
        const auto placedBy = [&](const std::optional<std::string>& line) {
            REQUIRE(line.has_value());
            CHECK(line->starts_with("asset.instantiate "));
            const Entity placed = world.findById(parsedId(runLine(*line)["id"].get<std::string>()));
            REQUIRE_FALSE(placed.isNull());
            world.updateWorldTransforms();
            return placed;
        };
        const auto materialsOf = [&world](Entity entity) {
            const auto* renderer = world.tryGet<MeshRendererComponent>(entity);
            REQUIRE(renderer != nullptr);
            return renderer->materials;
        };

        // Where nothing is in the way, on the grid - a root whatever is selected.
        fixture.run("entity.create", R"({"name": "Yard"})");
        fixture.run("selection.set", R"({"entities": ["Yard"]})");
        const Entity first = placedBy(viewportDropCommand(*fixture.context, *cube, ray, std::nullopt, reach));
        CHECK(world.parent(first).isNull());
        CHECK_NEAR(world.transform(first).position, Vec3(0.0f), 1.0e-5f);
        CHECK(world.transform(first).position.y == 0.0f);

        // Where the first is in the way, on it: on its top, a quarter of a metre from its middle.
        const std::optional<WorldMeshHit> onFirst = raycastMeshes(world, *fixture.assets, ray);
        REQUIRE(onFirst.has_value());
        CHECK(onFirst->entity == first);
        const Entity second = placedBy(viewportDropCommand(*fixture.context, *cube, ray, onFirst, reach));
        CHECK_NEAR(world.transform(second).position, Vec3(0.0f, 0.5f, 0.25f), 1.0e-5f);

        // A material goes onto the part of the mesh it meets, and is no place to put anything.
        CHECK_FALSE(viewportDropCommand(*fixture.context, *paint, ray, std::nullopt, reach).has_value());
        std::optional<std::string> line = viewportDropCommand(*fixture.context, *paint, ray, onFirst, reach);
        REQUIRE(line.has_value());
        CHECK(line->starts_with("component.set "));
        runLine(*line);
        CHECK(materialsOf(first) == std::vector<Uuid>{paint->id});
        CHECK(fixture.context->history().undoLabel() == "Edit MeshRenderer of 'Cube'");
        // Dropped again, there is nothing to do.
        CHECK_FALSE(viewportDropCommand(*fixture.context, *paint, ray, onFirst, reach).has_value());

        // On a mesh of several parts, onto the one that was met; those before it keep theirs.
        const Entity body = placedBy(viewportDropCommand(
            *fixture.context, *database.findByReference("models/cart.gltf#mesh/0"), ray, std::nullopt, reach));
        const Uuid bodyPaint = parsedId(fixture.id("models/cart.gltf#material/0"));
        const WorldMeshHit onLamp{.entity = body, .hit = {.distance = 1.0f, .submesh = 1}};
        line = viewportDropCommand(*fixture.context, *paint, ray, onLamp, reach);
        REQUIRE(line.has_value());
        runLine(*line);
        CHECK(materialsOf(body) == std::vector<Uuid>{bodyPaint, paint->id});

        // Nothing else is taken.
        CHECK_FALSE(viewportDropCommand(*fixture.context, *wall, ray, onFirst, reach).has_value());
        for (const char* reference : {"scenes/yard.scene.json", "scripts/gate.lua", "sounds/creak.wav"}) {
            const AssetInfo* asset = database.findByReference(reference);
            REQUIRE(asset != nullptr);
            CHECK_FALSE(viewportDropCommand(*fixture.context, *asset, ray, onFirst, reach).has_value());
        }
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
    TEST_CASE("asset.get says what needs an asset, and where the open scene refers to it") {
        AssetFixture fixture;
        const std::string body = fixture.id("models/cart.gltf#mesh/0");
        const std::string paint = fixture.id("materials/paint.material.json");
        fixture.writeText("scenes/depot.scene.json",
                          fmt::format(R"({{"format": "cordite.scene", "version": 1, "entities": [
            {{"id": "{}", "name": "Crate",
             "components": {{"MeshRenderer": {{"mesh": "{}", "materials": ["{}"]}}, "Transform": {{}}}}}}]}})",
                                      Uuid::generate().toString(), body, paint));
        static_cast<void>(fixture.run("asset.refresh"));

        // The mesh is used by the scene; the model it is in holds it, which is no use of it.
        const Json mesh = fixture.run("asset.get", R"({"asset": "models/cart.gltf#mesh/0"})");
        REQUIRE(mesh["usedBy"].size() == 1);
        CHECK(mesh["usedBy"][0]["path"] == "scenes/depot.scene.json");
        CHECK(mesh["usedBy"][0]["type"] == "scene");
        // The texture by the material of the model that has it.
        const Json texture = fixture.run("asset.get", R"({"asset": "textures/wall.png"})");
        REQUIRE(texture["usedBy"].size() == 1);
        CHECK(texture["usedBy"][0]["path"] == "models/cart.gltf#material/0");
        // And the scene says what it uses, as its file has it.
        const Json depot = fixture.run("asset.get", R"({"asset": "scenes/depot.scene.json"})");
        REQUIRE(depot["dependencies"].size() == 2);
        CHECK(depot["dependencies"][0]["id"] == body);
        CHECK(depot["dependencies"][1]["id"] == paint);
        CHECK(depot["usedBy"].empty());
        CHECK(depot["usedInScene"].empty());

        // The open scene is another one: where it refers to the paint is said, to the property.
        CHECK(fixture.run("asset.get", R"({"asset": "materials/paint.material.json"})")["usedInScene"].empty());
        fixture.run("entity.create", R"({"name": "Bare"})");
        fixture.run("entity.create", fmt::format(R"({{"name": "Shed", "parent": "Bare",
            "components": {{"MeshRenderer": {{"mesh": "{0}", "materials": ["{1}", "{1}"]}}}}}})",
                                                 body, paint));
        const Json used = fixture.run("asset.get", R"({"asset": "materials/paint.material.json"})");
        REQUIRE(used["usedInScene"].size() == 2);
        CHECK(used["usedInScene"][0]["entity"] == "Bare/Shed");
        CHECK(used["usedInScene"][0]["id"] == fixture.world.id(fixture.world.findByName("Shed")).toString());
        CHECK(used["usedInScene"][0]["component"] == "MeshRenderer");
        CHECK(used["usedInScene"][0]["property"] == "materials[0]");
        CHECK(used["usedInScene"][1]["property"] == "materials[1]");
        // What needs it among the assets is what their files say, not the open scene.
        REQUIRE(used["usedBy"].size() == 1);
        CHECK(used["usedBy"][0]["path"] == "scenes/depot.scene.json");
    }

    TEST_CASE("a saved scene is read again for what it uses, and one saved under a new name is listed at once") {
        AssetFixture fixture;
        const std::string body = fixture.id("models/cart.gltf#mesh/0");
        const auto usersOfBody = [&fixture] {
            // Kept: a loop over a member of what a call returns would outlive it.
            const Json mesh = fixture.run("asset.get", R"({"asset": "models/cart.gltf#mesh/0"})");
            std::vector<std::string> paths;
            for (const Json& user : mesh["usedBy"]) {
                paths.push_back(user["path"].get<std::string>());
            }
            return paths;
        };
        fixture.run("scene.open", R"({"path": "scenes/yard.scene.json"})");
        fixture.run("entity.create",
                    fmt::format(R"({{"name": "Cart", "components": {{"MeshRenderer": {{"mesh": "{}"}}}}}})", body));
        CHECK(usersOfBody().empty());

        // Saved where it was: read again, and nothing else is.
        const uint64_t revision = fixture.assets->database().revision();
        fixture.run("scene.save");
        CHECK(usersOfBody() == std::vector<std::string>{"scenes/yard.scene.json"});
        CHECK(fixture.assets->database().revision() == revision);

        // Saved under another name among the assets: listed at once, with what it uses.
        Json elsewhere = Json::object();
        elsewhere["path"] = pathToUtf8(fixture.root / "scenes" / "depot.scene.json");
        fixture.run("scene.save", elsewhere.dump());
        CHECK(fixture.assets->database().findByReference("scenes/depot.scene.json") != nullptr);
        CHECK(usersOfBody() == std::vector<std::string>{"scenes/depot.scene.json", "scenes/yard.scene.json"});

        // Saved where no asset is, or as what is no scene to the assets: nothing to tell them.
        const uint64_t listed = fixture.assets->database().revision();
        for (const std::filesystem::path& file :
             {fixture.directory.path() / "outside.scene.json", fixture.root / "scenes" / "notes.json"}) {
            Json parameters = Json::object();
            parameters["path"] = pathToUtf8(file);
            fixture.run("scene.save", parameters.dump());
        }
        CHECK(fixture.assets->database().revision() == listed);
        CHECK(usersOfBody().size() == 2);
    }

    TEST_CASE("a file is moved by a line, and dropped onto a directory of the project it goes into it") {
        AssetFixture fixture;
        const Uuid paint = *Uuid::parse(fixture.id("materials/paint.material.json"));
        CHECK(moveAssetCommand(paint, "looks/paint.material.json") ==
              fmt::format(R"(asset.move {{"asset":"{}","to":"looks/paint.material.json"}})", paint.toString()));

        const AssetDatabase& database = fixture.assets->database();
        const std::vector<AssetBrowserRow> rows = fixture.browser.rows(database);
        const auto rowOf = [&rows](std::string_view path) {
            const auto found = std::ranges::find(rows, path, &AssetBrowserRow::path);
            REQUIRE_MESSAGE(found != rows.end(), "no row " << std::string(path));
            return *found;
        };
        const AssetInfo& material = *database.findByReference("materials/paint.material.json");
        CHECK(dropTarget(material, rowOf("textures")) == "textures/paint.material.json");
        // Where it is already, onto what the engine has built in, onto an asset: nowhere.
        CHECK(dropTarget(material, rowOf("materials")).empty());
        CHECK(dropTarget(material, rowOf(std::string(kBuiltinAssetPath))).empty());
        CHECK(dropTarget(material, rowOf("textures/wall.png")).empty());
        // What has no file of its own does not move.
        CHECK(dropTarget(*database.findByReference("models/cart.gltf#mesh/0"), rowOf("textures")).empty());
        CHECK(dropTarget(*database.find(builtinMeshId(BuiltinMesh::Cube)), rowOf("textures")).empty());

        // Onto the top of the tree, to the top; onto a directory among the contents, into it.
        const std::vector<AssetBrowserRow> folders = fixture.browser.folders(database);
        const AssetBrowserRow& top = folders.front();
        CHECK(dropTarget(material, top) == "paint.material.json");
        CHECK(dropTarget(material, folders.back()).empty());
        const std::vector<AssetBrowserRow> contents = fixture.browser.contents(database);
        const auto textures = std::ranges::find(contents, "textures", &AssetBrowserRow::path);
        REQUIRE(textures != contents.end());
        CHECK(dropTarget(material, *textures) == "textures/paint.material.json");
        // What is at the top already stays there.
        fixture.writeText("notes.lua", "return {}");
        REQUIRE(fixture.assets->refresh().has_value());
        const AssetDatabase& refreshed = fixture.assets->database();
        CHECK(dropTarget(*refreshed.findByReference("notes.lua"), fixture.browser.folders(refreshed).front()).empty());
    }

    TEST_CASE("a sound is heard by a line, and said in a few words beside the rows") {
        const Uuid sound = Uuid::generate();
        CHECK(listenCommand(sound) == fmt::format(R"(asset.listen {{"asset":"{}"}})", sound.toString()));
        // Without a sound the line stops what is heard.
        CHECK(listenCommand(std::nullopt) == "asset.listen");

        CHECK(describeSound({.sampleRate = 48000, .channels = 1, .samples = std::vector<float>(12000)}) ==
              "0.25 s, mono, 48000 Hz");
        CHECK(describeSound({.sampleRate = 44100, .channels = 2, .samples = std::vector<float>(2 * 66150)}) ==
              "1.50 s, stereo, 44100 Hz");
        // A sound with nothing in it lasts no time.
        CHECK(describeSound({.sampleRate = 22050, .channels = 2}) == "0.00 s, stereo, 22050 Hz");
    }

    TEST_CASE("a directory is moved by a line, and dropped onto another directory it goes into it") {
        AssetFixture fixture;
        CHECK(moveDirectoryCommand("looks", "art/looks") == R"(asset.moveDirectory {"from":"looks","to":"art/looks"})");

        const std::vector<AssetBrowserRow> rows = fixture.browser.rows(fixture.assets->database());
        const auto rowOf = [&rows](std::string_view path) {
            const auto found = std::ranges::find(rows, path, &AssetBrowserRow::path);
            REQUIRE_MESSAGE(found != rows.end(), "no row " << std::string(path));
            return *found;
        };
        const auto directory = [](std::string path) {
            AssetBrowserRow row;
            row.kind = AssetBrowserRow::Kind::Directory;
            row.key = path;
            row.path = std::move(path);
            return row;
        };
        CHECK(isProjectDirectory(rowOf("textures")));
        CHECK_FALSE(isProjectDirectory(rowOf(std::string(kBuiltinAssetPath))));
        CHECK_FALSE(isProjectDirectory(rowOf("textures/wall.png")));

        CHECK(directoryDropTarget("materials", rowOf("textures")) == "textures/materials");
        CHECK(directoryDropTarget("art/looks", directory("props")) == "props/looks");
        // Onto itself, into what is inside it, where it is already, onto what is no
        // directory of the project: nowhere.
        CHECK(directoryDropTarget("materials", rowOf("materials")).empty());
        CHECK(directoryDropTarget("art", directory("art/looks")).empty());
        CHECK(directoryDropTarget("art/looks", directory("art")).empty());
        CHECK(directoryDropTarget("materials", rowOf(std::string(kBuiltinAssetPath))).empty());
        CHECK(directoryDropTarget("materials", rowOf("textures/wall.png")).empty());
        CHECK(directoryDropTarget("", rowOf("textures")).empty());
        // A name that only begins like another is not inside it.
        CHECK(directoryDropTarget("art", directory("artwork")) == "artwork/art");

        // The top of the tree takes what is not at the top, and is itself carried nowhere.
        const AssetBrowserRow top = fixture.browser.folders(fixture.assets->database()).front();
        CHECK(isProjectDirectory(top));
        CHECK_FALSE(isMovableDirectory(top));
        CHECK(isMovableDirectory(rowOf("textures")));
        CHECK_FALSE(isMovableDirectory(rowOf(std::string(kBuiltinAssetPath))));
        CHECK_FALSE(isMovableDirectory(rowOf("textures/wall.png")));
        CHECK(directoryDropTarget("art/looks", top) == "looks");
        CHECK(directoryDropTarget("materials", top).empty());
    }

    TEST_CASE("files are brought in by command, and what is dropped goes into the folder shown") {
        AssetFixture fixture;
        const TempDirectory downloads;
        const Image image = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        REQUIRE(image.savePng(downloads.path() / "rust.png").has_value());
        // What it was called by in another project.
        REQUIRE(
            writeTextFile(downloads.path() / "rust.png.meta",
                          R"({"format": "cordite.asset", "version": 1, "id": "22222222-2222-4222-8222-222222222222"})")
                .has_value());

        // Where a drop goes: the folder shown - the top at first, and while it is what the
        // engine has built in. Choosing an asset shows the folder it is in.
        CHECK(importDirectory(fixture.browser, fixture.assets->database()).empty());
        fixture.run("assets.browse", R"({"select": "models/cart.gltf#mesh/0"})");
        CHECK(importDirectory(fixture.browser, fixture.assets->database()) == "models");
        fixture.run("assets.browse", fmt::format(R"({{"select": "{}"}})", builtinMeshId(BuiltinMesh::Cube).toString()));
        CHECK(importDirectory(fixture.browser, fixture.assets->database()).empty());
        fixture.run("assets.browse", R"({"folder": "scripts"})");
        CHECK(importDirectory(fixture.browser, fixture.assets->database()) == "scripts");
        // Not where the chosen asset is, when another folder is shown.
        fixture.run("assets.browse", R"({"select": "textures/wall.png", "folder": "sounds"})");
        CHECK(importDirectory(fixture.browser, fixture.assets->database()) == "sounds");
        fixture.run("assets.browse", R"({"select": "textures/wall.png"})");
        const std::string directory = importDirectory(fixture.browser, fixture.assets->database());
        CHECK(directory == "textures");

        const std::vector<std::filesystem::path> dropped = {downloads.path() / "rust.png"};
        const std::string line = importAssetsCommand(dropped, directory);
        Json expected = Json::object();
        expected["files"] = Json::array({pathToUtf8(dropped[0])});
        expected["to"] = "textures";
        CHECK(line == "asset.import " + expected.dump());
        CHECK(importAssetsCommand(dropped, "") ==
              fmt::format("asset.import {{\"files\":{}}}", expected["files"].dump()));

        const Json imported = fixture.run("asset.import", line.substr(std::string_view("asset.import ").size()));
        REQUIRE(imported["files"].size() == 1);
        CHECK(imported["files"][0]["path"] == "textures/rust.png");
        CHECK(imported["files"][0]["type"] == "texture");
        // The .meta stayed behind: in this project the file is new, with an id of its own.
        CHECK_FALSE(fileExists(fixture.root / "textures" / "rust.png.meta"));
        CHECK(imported["files"][0]["id"] == fixture.id("textures/rust.png"));
        CHECK(imported["files"][0]["id"] != "22222222-2222-4222-8222-222222222222");
        CHECK(imported["problems"].empty());
        CHECK(fixture.fail("asset.import", line.substr(std::string_view("asset.import ").size())).code ==
              ErrorCode::AlreadyExists);
        CHECK(fixture.fail("asset.import", R"({"files": [3]})").code == ErrorCode::InvalidArgument);
        Json many = Json::object();
        many["files"] = Json::array();
        for (int index = 0; index < 1025; ++index) {
            many["files"].push_back("x.png");
        }
        CHECK(fixture.fail("asset.import", many.dump()).message.find("1024 files") != std::string::npos);
    }

    TEST_CASE("a folder and a material are made by command, as the Assets panel asks, inside the assets") {
        AssetFixture fixture;
        const std::string folderLine = createDirectoryCommand("props/metal");
        CHECK(folderLine == R"(asset.createDirectory {"path":"props/metal"})");
        CHECK(fixture.run("asset.createDirectory",
                          folderLine.substr(std::string_view("asset.createDirectory ").size()))["path"] ==
              "props/metal");
        CHECK(directoryExists(fixture.root / "props" / "metal"));
        // Shown by the browser, empty as it is.
        const std::vector<AssetBrowserRow> folders = fixture.browser.folders(fixture.assets->database());
        CHECK(std::ranges::find(folders, "props/metal", &AssetBrowserRow::path) != folders.end());

        const std::string materialLine = createMaterialCommand("props/metal/rust.material.json");
        const Json material =
            fixture.run("asset.createMaterial", materialLine.substr(std::string_view("asset.createMaterial ").size()));
        CHECK(material["type"] == "material");
        CHECK(material["id"] == fixture.id("props/metal/rust.material.json"));

        CHECK(fixture.fail("asset.createDirectory", R"({"path": "props/metal"})").code == ErrorCode::AlreadyExists);
        CHECK(fixture.fail("asset.createDirectory", R"({"path": "../outside"})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("asset.createMaterial", R"({"path": "props/rust.png"})").code == ErrorCode::InvalidArgument);
        CHECK(fixture.fail("asset.createMaterial", R"({"path": "props/metal/rust.material.json"})").code ==
              ErrorCode::AlreadyExists);
        // What the panel asks for when a file is deleted.
        const Uuid asset = Uuid::generate();
        CHECK(deleteAssetCommand(asset, false) == fmt::format(R"(asset.delete {{"asset":"{}"}})", asset.toString()));
        CHECK(deleteAssetCommand(asset, true) ==
              fmt::format(R"(asset.delete {{"asset":"{}","force":true}})", asset.toString()));
    }

    TEST_CASE("the entities that use an asset are found in the order of the hierarchy, and selected by a line") {
        AssetFixture fixture;
        const std::string body = fixture.id("models/cart.gltf#mesh/0");
        const std::string paint = fixture.id("materials/paint.material.json");
        fixture.run("entity.create", R"({"name": "Yard"})");
        // Made after the lamp's parent and before the lamp, but in the hierarchy after it.
        fixture.run("entity.create", fmt::format(R"({{"name": "Shed",
            "components": {{"MeshRenderer": {{"mesh": "{0}", "materials": ["{1}", "{1}"]}}}}}})",
                                                 body, paint));
        fixture.run(
            "entity.create",
            fmt::format(
                R"({{"name": "Lamp", "parent": "Yard", "components": {{"MeshRenderer": {{"materials": ["{}"]}}}}}})",
                paint));
        fixture.run("entity.create",
                    fmt::format(R"({{"name": "Wagon", "components": {{"MeshRenderer": {{"mesh": "{}"}}}}}})", body));
        const auto idOf = [&fixture](std::string_view name) {
            return fixture.world.id(fixture.world.findByName(name));
        };

        // Twice in one entity is one use of it.
        const std::vector<Uuid> users = entitiesUsing(*fixture.context, *Uuid::parse(paint));
        CHECK(users == std::vector<Uuid>{idOf("Lamp"), idOf("Shed")});
        CHECK(entitiesUsing(*fixture.context, *Uuid::parse(body)) == std::vector<Uuid>{idOf("Shed"), idOf("Wagon")});
        CHECK(entitiesUsing(*fixture.context, *Uuid::parse(fixture.id("textures/wall.png"))).empty());

        const std::string line = selectEntitiesCommand(users);
        CHECK(line == fmt::format(R"(selection.set {{"entities":["{}","{}"]}})", idOf("Lamp").toString(),
                                  idOf("Shed").toString()));
        REQUIRE(line.starts_with("selection.set "));
        fixture.run("selection.set", line.substr(std::string_view("selection.set ").size()));
        std::vector<Uuid> selected;
        for (const Entity entity : fixture.context->selection()) {
            selected.push_back(fixture.world.id(entity));
        }
        CHECK(selected == users);
        CHECK(selectEntitiesCommand({}) == R"(selection.set {"entities":[]})");
    }
}
