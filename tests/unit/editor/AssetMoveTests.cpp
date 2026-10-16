#include "cordite/editor/AssetMove.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/world/SceneSerializer.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A project with a texture that a material uses by id and another that a glTF file
/// names by where it is, the glTF file and the buffer beside it, a .glb that names
/// nothing, a scene, and a script that names the first texture by where it is.
struct MoveFixture {
    TempDirectory directory;
    std::filesystem::path root = directory.path() / "assets";
    ComponentRegistry components;
    World world;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<EditorContext> context;

    explicit MoveFixture(bool writeMetaFiles = true) {
        REQUIRE(registerEngineComponents(components).has_value());
        writeTexture("textures/brick.png");
        writeTexture("textures/wall.png");
        writeModels();
        writeText("scripts/door.lua", "-- A door that creaks.\n"
                                      "local sound = \"textures/brick.png\" -- by where it is\n"
                                      "local other = \"textures/brick.png.bak\" -- another file\n"
                                      "return {}\n");
        // Ids made of the paths, which the material and the scene are written with.
        const std::unique_ptr<AssetManager> first = unwrap(AssetManager::create({.rootDirectory = root}));
        const std::string brick = first->database().findByReference("textures/brick.png")->id.toString();
        writeText("materials/paint.material.json",
                  fmt::format(R"({{"format": "cordite.material", "version": 1, "baseColorTexture": "{}"}})", brick));

        assets = unwrap(AssetManager::create({.rootDirectory = root,
                                              .writeMetaFiles = writeMetaFiles,
                                              .readReferences = assetReferenceReader(components)}));
        context = std::make_unique<EditorContext>(world, components, *assets);
    }

    void writeText(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(root / pathFromUtf8(relativePath), text).has_value());
    }

    void writeTexture(std::string_view relativePath) const {
        const Image image = unwrap(Image::create(4, 4, ImageFormat::Rgba8));
        REQUIRE(image.savePng(root / pathFromUtf8(relativePath)).has_value());
    }

    void writeModels() const {
        GltfDocument cart;
        cart.images.push_back({.name = "Wall", .uri = "../textures/wall.png"});
        GltfMaterial paint;
        paint.name = "Paint";
        paint.images.baseColor = 0;
        cart.materials.push_back(paint);
        GltfMesh body;
        body.mesh = makeBox();
        body.mesh.name = "Body";
        body.materials = {0};
        cart.meshes.push_back(body);
        cart.nodes.push_back({.name = "Body", .mesh = 0});
        cart.roots = {0};
        REQUIRE(ensureDirectory(root / "models").has_value());
        REQUIRE(exportGltf(cart, root / "models" / "cart.gltf").has_value());

        GltfDocument barrel;
        GltfMesh drum;
        drum.mesh = makeCylinder(0.5f, 1.0f, 12);
        drum.mesh.name = "Drum";
        barrel.meshes.push_back(drum);
        barrel.nodes.push_back({.name = "Drum", .mesh = 0});
        barrel.roots = {0};
        REQUIRE(exportGltf(barrel, root / "models" / "barrel.glb").has_value());
    }

    [[nodiscard]] Uuid id(std::string_view reference) const {
        const AssetInfo* asset = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
        return asset->id;
    }

    [[nodiscard]] std::string where(const Uuid& asset) const {
        const AssetInfo* found = assets->database().find(asset);
        REQUIRE(found != nullptr);
        return found->reference();
    }

    [[nodiscard]] bool exists(std::string_view relativePath) const {
        return fileExists(root / pathFromUtf8(relativePath));
    }

    /// The error of a move that is refused, after checking that nothing moved.
    Error refusal(const Uuid& asset, std::string_view to) {
        const uint64_t revision = assets->database().revision();
        const Result<AssetMove> moved = moveAsset(*context, asset, std::string(to));
        REQUIRE_FALSE(moved.has_value());
        CHECK(assets->database().revision() == revision);
        return moved.error();
    }

    /// The error of a move of a directory that is refused, after checking that nothing
    /// moved.
    Error directoryRefusal(std::string_view from, std::string_view to) {
        const uint64_t revision = assets->database().revision();
        const Result<DirectoryMove> moved = moveAssetDirectory(*context, std::string(from), std::string(to));
        REQUIRE_FALSE(moved.has_value());
        CHECK(assets->database().revision() == revision);
        return moved.error();
    }
};

} // namespace

TEST_SUITE("AssetMove") {
    TEST_CASE("a file moved to another directory keeps its id, and what refers to it by id follows it") {
        MoveFixture fixture;
        const Uuid brick = fixture.id("textures/brick.png");
        const Uuid paint = fixture.id("materials/paint.material.json");
        // A value in the open scene that names it by where it is, as a script is given it.
        const Entity door = fixture.world.createEntity("Door");
        fixture.world.add<ScriptComponent>(door) = {.script = fixture.id("scripts/door.lua"),
                                                    .properties =
                                                        unwrap(parseJson(R"({"sound": "textures/brick.png"})"))};

        const AssetMove moved = unwrap(moveAsset(*fixture.context, brick, "textures/stone/brick.png"));
        CHECK(moved.asset == brick);
        CHECK(moved.from == "textures/brick.png");
        CHECK(moved.to == "textures/stone/brick.png");
        CHECK(fixture.exists("textures/stone/brick.png"));
        CHECK(fixture.exists("textures/stone/brick.png.meta"));
        CHECK_FALSE(fixture.exists("textures/brick.png"));
        CHECK_FALSE(fixture.exists("textures/brick.png.meta"));
        // The id is where the file is, and the material still has it.
        CHECK(fixture.where(brick) == "textures/stone/brick.png");
        CHECK(fixture.assets->database().find(paint)->dependencies == std::vector<Uuid>{brick});
        CHECK(fixture.assets->database().dependents(brick).size() == 1);

        // What names it by where it was is said, to the line, and is left as it is.
        REQUIRE(moved.files.size() == 1);
        CHECK(moved.files[0].file == "scripts/door.lua");
        CHECK(moved.files[0].line == 2);
        CHECK(moved.files[0].text == R"(local sound = "textures/brick.png" -- by where it is)");
        REQUIRE(moved.entities.size() == 1);
        CHECK(moved.entities[0].entity == door);
        CHECK(moved.entities[0].component == "Script");
        CHECK(moved.entities[0].property == "/properties/sound");

        const Json described = describeAssetMove(*fixture.context, moved);
        CHECK(described["id"] == brick.toString());
        CHECK(described["from"] == "textures/brick.png");
        CHECK(described["to"] == "textures/stone/brick.png");
        REQUIRE(described["mentions"].size() == 2);
        CHECK(described["mentions"][0]["file"] == "scripts/door.lua");
        CHECK(described["mentions"][0]["line"] == 2);
        CHECK(described["mentions"][1]["entity"] == "Door");
        CHECK(described["mentions"][1]["id"] == fixture.world.id(door).toString());
        CHECK(described["mentions"][1]["property"] == "/properties/sound");
    }

    TEST_CASE("an id handed over from the assets themselves is good for the whole move") {
        // The assets are read again on the way, which what was theirs would not outlive: the
        // id and the path are taken by value. That is pinned here, at compile time, because
        // a run cannot be relied on to see the difference - an id read from freed memory
        // was found to be the old id more often than not.
        static_assert(std::is_same_v<decltype(&moveAsset), Result<AssetMove> (*)(EditorContext&, Uuid, std::string)>);
        MoveFixture fixture;
        const AssetInfo* brick = fixture.assets->database().findByReference("textures/brick.png");
        REQUIRE(brick != nullptr);
        const Uuid id = brick->id;
        const AssetMove moved = unwrap(moveAsset(*fixture.context, brick->id, "textures/stone.png"));
        CHECK(moved.asset == id);
        CHECK(fixture.where(id) == "textures/stone.png");
    }

    TEST_CASE("a file is renamed where it is, also when only the case of its name changes") {
        MoveFixture fixture;
        const Uuid brick = fixture.id("textures/brick.png");
        unwrap(moveAsset(*fixture.context, brick, "textures/stone.png"));
        CHECK(fixture.where(brick) == "textures/stone.png");
        // On a file system that does not tell, the file is there already: as itself.
        unwrap(moveAsset(*fixture.context, brick, "textures/Stone.png"));
        CHECK(fixture.where(brick) == "textures/Stone.png");
        CHECK(fixture.exists("textures/Stone.png.meta"));
    }

    TEST_CASE("a file that had no .meta gets one where it goes, which keeps the id it had") {
        MoveFixture fixture(false);
        const Uuid brick = fixture.id("textures/brick.png");
        CHECK_FALSE(fixture.exists("textures/brick.png.meta"));
        unwrap(moveAsset(*fixture.context, brick, "stone.png"));
        CHECK(fixture.exists("stone.png.meta"));
        CHECK(fixture.where(brick) == "stone.png");
    }

    TEST_CASE("the file of the open scene follows it") {
        MoveFixture fixture;
        fixture.writeText("scenes/yard.scene.json", R"({"format": "cordite.scene", "version": 1, "entities": []})");
        REQUIRE(fixture.assets->refresh().has_value());
        const std::filesystem::path yard = fixture.root / "scenes" / "yard.scene.json";
        REQUIRE(fixture.context->openScene(yard).has_value());

        unwrap(moveAsset(*fixture.context, fixture.id("scenes/yard.scene.json"), "levels/yard.scene.json"));
        CHECK(fixture.context->scenePath() == fixture.root / "levels" / "yard.scene.json");
        CHECK_FALSE(fixture.context->hasUnsavedChanges());
        fixture.world.createEntity("Gate");
        REQUIRE(fixture.context->saveScene().has_value());
        CHECK(unwrap(readTextFile(fixture.root / "levels" / "yard.scene.json")).find("Gate") != std::string::npos);
        CHECK_FALSE(fixture.exists("scenes/yard.scene.json"));
    }

    TEST_CASE("a glTF file is renamed where it is, and one that names no file moves anywhere") {
        MoveFixture fixture;
        const Uuid cart = fixture.id("models/cart.gltf");
        const Uuid body = fixture.id("models/cart.gltf#mesh/0");
        unwrap(moveAsset(*fixture.context, cart, "models/wagon.gltf"));
        // What is in it keeps its ids, and it still finds its buffer and its texture.
        CHECK(fixture.where(body) == "models/wagon.gltf#mesh/0");
        CHECK(fixture.assets->database().problems().empty());
        CHECK(fixture.assets->loadMesh(body).has_value());

        const Uuid barrel = fixture.id("models/barrel.glb");
        unwrap(moveAsset(*fixture.context, barrel, "props/barrel.glb"));
        CHECK(fixture.where(barrel) == "props/barrel.glb");
        CHECK(fixture.assets->database().problems().empty());
    }

    TEST_CASE("a directory moves with all that is in it, and every asset in it keeps its id") {
        MoveFixture fixture(false);
        fixture.writeText("scripts/doors/creak.lua", "return {}\n");
        fixture.writeText("scripts/notes.txt", "not an asset, and moved all the same");
        REQUIRE(fixture.assets->refresh().has_value());
        const Uuid door = fixture.id("scripts/door.lua");
        const Uuid creak = fixture.id("scripts/doors/creak.lua");

        const DirectoryMove moved = unwrap(moveAssetDirectory(*fixture.context, "scripts", "gameplay/scripts"));
        CHECK(moved.from == "scripts");
        CHECK(moved.to == "gameplay/scripts");
        REQUIRE(moved.assets.size() == 2);
        // Without a .meta, a file would have been given a new id made of its new path.
        CHECK(fixture.where(door) == "gameplay/scripts/door.lua");
        CHECK(fixture.where(creak) == "gameplay/scripts/doors/creak.lua");
        CHECK(fixture.exists("gameplay/scripts/door.lua.meta"));
        CHECK(fixture.exists("gameplay/scripts/notes.txt"));
        CHECK_FALSE(std::filesystem::exists(fixture.root / "scripts"));
        CHECK(fixture.assets->database().problems().empty());
    }

    TEST_CASE("what refers to the assets of a directory by id follows it, and what names them by where they were is "
              "listed") {
        MoveFixture fixture;
        const Uuid paint = fixture.id("materials/paint.material.json");
        fixture.writeTexture("decals/crack.png");
        fixture.writeText("scripts/wall.lua", "local decal = \"decals/crack.png\"\nreturn {}\n");
        REQUIRE(fixture.assets->refresh().has_value());

        const DirectoryMove materials = unwrap(moveAssetDirectory(*fixture.context, "materials", "art/materials"));
        CHECK(fixture.where(paint) == "art/materials/paint.material.json");
        // The material loads where it is now, and still finds its texture, by id.
        const std::shared_ptr<const MaterialData> material = unwrap(fixture.assets->loadMaterial(paint));
        CHECK(fixture.where(material->baseColorTexture) == "textures/brick.png");
        CHECK(materials.assets.front().files.empty());

        const DirectoryMove decals = unwrap(moveAssetDirectory(*fixture.context, "decals", "art/decals"));
        REQUIRE(decals.assets.size() == 1);
        REQUIRE(decals.assets.front().files.size() == 1);
        CHECK(decals.assets.front().files.front().file == "scripts/wall.lua");
        CHECK(decals.assets.front().files.front().line == 1);
        const Json described = describeDirectoryMove(*fixture.context, decals);
        CHECK(described["from"] == "decals");
        CHECK(described["assets"][0]["to"] == "art/decals/crack.png");
        CHECK(described["assets"][0]["mentions"].size() == 1);
    }

    TEST_CASE("a directory of glTF files moves where the files they name by where they are stay where they say") {
        MoveFixture fixture;
        const Uuid body = fixture.id("models/cart.gltf#mesh/0");
        // As deep as it was: "../textures/wall.png" is still the texture.
        unwrap(moveAssetDirectory(*fixture.context, "models", "props"));
        CHECK(fixture.where(body) == "props/cart.gltf#mesh/0");
        CHECK(fixture.assets->database().problems().empty());
        CHECK(fixture.assets->loadMesh(body).has_value());
        // Deeper, it would name a texture that is not there; and the texture cannot go
        // without the file that names it.
        CHECK(fixture.directoryRefusal("props", "art/props").message.find("props/cart.gltf names textures/wall.png") !=
              std::string::npos);
        CHECK(fixture.directoryRefusal("textures", "art/textures").message.find("textures/wall.png") !=
              std::string::npos);
    }

    TEST_CASE("the open scene follows its directory") {
        MoveFixture fixture;
        fixture.writeText("scenes/yard.scene.json", R"({"format": "cordite.scene", "version": 1, "entities": []})");
        REQUIRE(fixture.assets->refresh().has_value());
        REQUIRE(fixture.context->openScene(fixture.root / "scenes" / "yard.scene.json").has_value());
        unwrap(moveAssetDirectory(*fixture.context, "scenes", "levels"));
        CHECK(fixture.context->scenePath() == fixture.root / "levels" / "yard.scene.json");
        fixture.world.createEntity("Gate");
        REQUIRE(fixture.context->saveScene().has_value());
        CHECK(unwrap(readTextFile(fixture.root / "levels" / "yard.scene.json")).find("Gate") != std::string::npos);
    }

    TEST_CASE("a directory goes nowhere it cannot, and nothing moves") {
        MoveFixture fixture;
        CHECK(fixture.directoryRefusal("nowhere", "elsewhere").code == ErrorCode::NotFound);
        CHECK(fixture.directoryRefusal("scripts", "models").code == ErrorCode::AlreadyExists);
        CHECK(fixture.directoryRefusal("scripts", "scripts").code == ErrorCode::InvalidArgument);
        CHECK(fixture.directoryRefusal("scripts", "scripts/inner").code == ErrorCode::InvalidArgument);
        CHECK(fixture.directoryRefusal("scripts", "../scripts").code == ErrorCode::InvalidArgument);
        CHECK(fixture.directoryRefusal("scripts", ".hidden").code == ErrorCode::InvalidArgument);
        CHECK(fixture.directoryRefusal("", "elsewhere").code == ErrorCode::InvalidArgument);
        CHECK(fixture.exists("scripts/door.lua"));
    }

    TEST_CASE("what a move would break, or cannot do, is refused, and nothing moves") {
        MoveFixture fixture;
        const Uuid brick = fixture.id("textures/brick.png");
        const auto refused = [&](const Uuid& asset, std::string_view to, ErrorCode code, std::string_view words) {
            CAPTURE(to);
            const Error error = fixture.refusal(asset, to);
            CHECK(error.code == code);
            CHECK_MESSAGE(error.message.find(words) != std::string::npos, error.message);
        };
        refused(Uuid::generate(), "x.png", ErrorCode::NotFound, "there is no asset");
        refused(builtinMeshId(BuiltinMesh::Cube), "cube.gltf", ErrorCode::InvalidArgument, "built into the engine");
        refused(fixture.id("models/cart.gltf#mesh/0"), "body.gltf", ErrorCode::InvalidArgument,
                "is inside models/cart.gltf");
        for (const std::string_view outside : {"/brick.png", "C:/brick.png", "textures\\brick2.png"}) {
            refused(brick, outside, ErrorCode::InvalidArgument, "relative path with forward slashes");
        }
        refused(brick, "../brick.png", ErrorCode::InvalidArgument, "a part that is '..'");
        refused(brick, "", ErrorCode::InvalidArgument, "say where");
        refused(brick, "textures//brick2.png", ErrorCode::InvalidArgument, "a part that is empty");
        refused(brick, "textures/./brick2.png", ErrorCode::InvalidArgument, "a part that is '.'");
        refused(brick, "textures/../brick2.png", ErrorCode::InvalidArgument, "a part that is '..'");
        refused(brick, "textures/", ErrorCode::InvalidArgument, "a part that is empty");
        refused(brick, ".hidden/brick.png", ErrorCode::InvalidArgument, "begins with a dot");
        refused(brick, "textures/brick.jpg", ErrorCode::InvalidArgument, "with '.png'");
        refused(brick, "textures/brick", ErrorCode::InvalidArgument, "another kind of file");
        refused(brick, "textures/brick.png", ErrorCode::InvalidArgument, "where it is to go");
        refused(brick, "textures/wall.png", ErrorCode::AlreadyExists, "textures/wall.png already");
        fixture.writeText("textures/taken.png.meta", "{}");
        refused(brick, "textures/taken.png", ErrorCode::AlreadyExists, "textures/taken.png.meta already");
        // A glTF file leaves its buffer and its texture behind in another directory.
        refused(fixture.id("models/cart.gltf"), "vehicles/cart.gltf", ErrorCode::InvalidArgument,
                "names 'cart.bin' and '../textures/wall.png' by where they are");
        // And a texture it names by where it is would be lost to it.
        refused(fixture.id("textures/wall.png"), "textures/plaster.png", ErrorCode::InvalidArgument,
                "models/cart.gltf names textures/wall.png by where it is");

        CHECK(fixture.exists("textures/brick.png"));
        CHECK(fixture.exists("textures/brick.png.meta"));
        CHECK(fixture.exists("models/cart.gltf"));
        CHECK(fixture.where(brick) == "textures/brick.png");
    }
}
