#include "cordite/editor/AssetFiles.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/renderer/RenderComponents.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A project with a texture that a material uses, a material that nothing uses, and a model
/// with a mesh inside it.
struct FilesFixture {
    TempDirectory directory;
    std::filesystem::path root = directory.path() / "assets";
    ComponentRegistry components;
    World world;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<EditorContext> context;

    FilesFixture() {
        REQUIRE(registerEngineComponents(components).has_value());
        const Image image = unwrap(Image::create(4, 4, ImageFormat::Rgba8));
        REQUIRE(ensureDirectory(root / "textures").has_value());
        REQUIRE(image.savePng(root / "textures" / "brick.png").has_value());
        GltfDocument barrel;
        GltfMesh drum;
        drum.mesh = makeCylinder(0.5f, 1.0f, 12);
        drum.mesh.name = "Drum";
        barrel.meshes.push_back(drum);
        barrel.nodes.push_back({.name = "Drum", .mesh = 0});
        barrel.roots = {0};
        REQUIRE(ensureDirectory(root / "models").has_value());
        REQUIRE(exportGltf(barrel, root / "models" / "barrel.glb").has_value());
        // The ids are made of the paths: the material is written with the texture's.
        const std::unique_ptr<AssetManager> first = unwrap(AssetManager::create({.rootDirectory = root}));
        const std::string brick = first->database().findByReference("textures/brick.png")->id.toString();
        REQUIRE(writeTextFile(
                    root / "materials" / "paint.material.json",
                    fmt::format(R"({{"format": "cordite.material", "version": 1, "baseColorTexture": "{}"}})", brick))
                    .has_value());
        REQUIRE(
            writeTextFile(root / "materials" / "spare.material.json", R"({"format": "cordite.material", "version": 1})")
                .has_value());
        assets = unwrap(AssetManager::create(
            {.rootDirectory = root, .writeMetaFiles = true, .readReferences = assetReferenceReader(components)}));
        context = std::make_unique<EditorContext>(world, components, *assets);
    }

    [[nodiscard]] const AssetInfo& asset(std::string_view reference) const {
        const AssetInfo* found = assets->database().findByReference(reference);
        REQUIRE_MESSAGE(found != nullptr, "no asset " << std::string(reference));
        return *found;
    }

    /// The error of what is refused, after checking that nothing of the assets changed.
    template <typename T>
    [[nodiscard]] Error refused(const Result<T>& result, ErrorCode code) const {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == code);
        return result.error();
    }
};

} // namespace

TEST_SUITE("AssetFiles") {
    TEST_CASE("a directory is made inside the assets, and is one of them while it is empty") {
        FilesFixture fixture;
        CHECK(unwrap(createAssetDirectory(*fixture.context, "props/metal")) == "props/metal");
        CHECK(directoryExists(fixture.root / "props" / "metal"));
        const auto directories = fixture.assets->database().directories();
        CHECK(std::ranges::find(directories, "props") != directories.end());
        CHECK(std::ranges::find(directories, "props/metal") != directories.end());
        // In the order of their paths.
        CHECK(std::ranges::is_sorted(directories));

        // Where there is one already, a file, or no place inside the assets: nothing is made.
        CHECK(fixture.refused(createAssetDirectory(*fixture.context, "props/metal"), ErrorCode::AlreadyExists)
                  .message.find("a directory") != std::string::npos);
        CHECK(fixture.refused(createAssetDirectory(*fixture.context, "textures/brick.png"), ErrorCode::AlreadyExists)
                  .message.find("a file") != std::string::npos);
        for (const char* nowhere : {"", "../outside", "props/../../outside", ".hidden", "props//metal", "C:/games"}) {
            CAPTURE(nowhere);
            (void)fixture.refused(createAssetDirectory(*fixture.context, nowhere), ErrorCode::InvalidArgument);
        }
        CHECK_FALSE(std::filesystem::exists(fixture.directory.path() / "outside"));
    }

    TEST_CASE("directories whose names begin with a dot are no part of the assets") {
        FilesFixture fixture;
        REQUIRE(ensureDirectory(fixture.root / ".scratch" / "notes").has_value());
        REQUIRE(fixture.assets->refresh().has_value());
        for (const std::string& directory : fixture.assets->database().directories()) {
            CHECK_FALSE(directory.starts_with('.'));
        }
    }

    TEST_CASE("a material is written with every property at its default, and gets an id") {
        FilesFixture fixture;
        const Uuid made = unwrap(createMaterial(*fixture.context, "materials/metal/rust.material.json"));
        const AssetInfo& material = fixture.asset("materials/metal/rust.material.json");
        CHECK(material.id == made);
        CHECK(material.type == AssetType::Material);
        CHECK(fileExists(assetMetaPath(fixture.root / "materials" / "metal" / "rust.material.json")));
        const Json document = unwrap(readJsonFile(fixture.root / "materials" / "metal" / "rust.material.json"));
        CHECK(document == serializeMaterial(MaterialData{}));

        (void)fixture.refused(createMaterial(*fixture.context, "materials/metal/rust.material.json"),
                              ErrorCode::AlreadyExists);
        for (const char* wrong :
             {"materials/rust.png", "materials/rust.material", ".material.json", "materials/.material.json"}) {
            CAPTURE(wrong);
            (void)fixture.refused(createMaterial(*fixture.context, wrong), ErrorCode::InvalidArgument);
        }
        (void)fixture.refused(createMaterial(*fixture.context, "../outside.material.json"), ErrorCode::InvalidArgument);
    }

    TEST_CASE("an asset that nothing uses is deleted with its .meta") {
        FilesFixture fixture;
        const Uuid spare = fixture.asset("materials/spare.material.json").id;
        const std::filesystem::path file = fixture.root / "materials" / "spare.material.json";
        REQUIRE(fileExists(assetMetaPath(file)));
        CHECK(unwrap(findAssetUsers(*fixture.context, spare)).empty());
        const AssetDeletion deleted = unwrap(deleteAsset(*fixture.context, spare, false));
        CHECK(deleted.path == "materials/spare.material.json");
        CHECK(deleted.users.empty());
        CHECK_FALSE(fileExists(file));
        CHECK_FALSE(fileExists(assetMetaPath(file)));
        CHECK(fixture.assets->database().find(spare) == nullptr);
        CHECK(fixture.assets->database().problems().empty());
    }

    TEST_CASE("an asset that another uses is kept unless deleting it is forced, and the users are said") {
        FilesFixture fixture;
        const Uuid brick = fixture.asset("textures/brick.png").id;
        const Uuid paint = fixture.asset("materials/paint.material.json").id;
        const AssetUsers users = unwrap(findAssetUsers(*fixture.context, brick));
        CHECK(users.assets == std::vector<Uuid>{paint});
        CHECK(users.sceneReferences == 0);

        const Error error = fixture.refused(deleteAsset(*fixture.context, brick, false), ErrorCode::InvalidState);
        CHECK(error.message.find("materials/paint.material.json") != std::string::npos);
        CHECK(fileExists(fixture.root / "textures" / "brick.png"));

        const AssetDeletion deleted = unwrap(deleteAsset(*fixture.context, brick, true));
        CHECK(deleted.users.assets == std::vector<Uuid>{paint});
        CHECK_FALSE(fileExists(fixture.root / "textures" / "brick.png"));
        CHECK(fixture.assets->database().find(brick) == nullptr);
    }

    TEST_CASE("the open scene uses what its properties name, and a model is used by what uses its meshes") {
        FilesFixture fixture;
        const Uuid paint = fixture.asset("materials/paint.material.json").id;
        const Uuid drum = fixture.asset("models/barrel.glb#mesh/0").id;
        const Entity entity = fixture.world.createEntity("Barrel");
        fixture.world.add<MeshRendererComponent>(entity, MeshRendererComponent{.mesh = drum, .materials = {paint}});

        CHECK(unwrap(findAssetUsers(*fixture.context, paint)).sceneReferences == 1);
        // The model is what the mesh is in: deleting the file takes the mesh.
        CHECK(unwrap(findAssetUsers(*fixture.context, fixture.asset("models/barrel.glb").id)).sceneReferences == 1);
        const Error error = fixture.refused(deleteAsset(*fixture.context, fixture.asset("models/barrel.glb").id, false),
                                            ErrorCode::InvalidState);
        CHECK(error.message.find("1 property of the open scene") != std::string::npos);
        // What is inside a file is not deleted on its own.
        (void)fixture.refused(deleteAsset(*fixture.context, drum, true), ErrorCode::InvalidArgument);
        CHECK(fileExists(fixture.root / "models" / "barrel.glb"));
    }

    TEST_CASE("what is built in, is not there, is the open scene or is played is not deleted") {
        FilesFixture fixture;
        (void)fixture.refused(deleteAsset(*fixture.context, builtinMeshId(BuiltinMesh::Cube), true),
                              ErrorCode::InvalidArgument);
        (void)fixture.refused(deleteAsset(*fixture.context, Uuid::generate(), true), ErrorCode::NotFound);
        (void)fixture.refused(findAssetUsers(*fixture.context, Uuid::generate()), ErrorCode::NotFound);

        const std::filesystem::path scene = fixture.root / "scenes" / "yard.scene.json";
        REQUIRE(fixture.context->saveScene(scene).has_value());
        REQUIRE(fixture.assets->refresh().has_value());
        const Uuid yard = fixture.asset("scenes/yard.scene.json").id;
        CHECK(
            fixture.refused(deleteAsset(*fixture.context, yard, true), ErrorCode::InvalidState).message.find("open") !=
            std::string::npos);
        CHECK(fileExists(scene));

        REQUIRE(fixture.context->beginPlay().has_value());
        (void)fixture.refused(deleteAsset(*fixture.context, fixture.asset("materials/spare.material.json").id, true),
                              ErrorCode::InvalidState);
        CHECK(fileExists(fixture.root / "materials" / "spare.material.json"));
    }

    TEST_CASE("a new folder or material is given the first name that nothing has") {
        FilesFixture fixture;
        CHECK(freeAssetPath(*fixture.context, "", "New folder", "") == "New folder");
        CHECK(freeAssetPath(*fixture.context, "materials", "New material", ".material.json") ==
              "materials/New material.material.json");
        REQUIRE(createAssetDirectory(*fixture.context, "New folder").has_value());
        REQUIRE(createMaterial(*fixture.context, "materials/New material.material.json").has_value());
        CHECK(freeAssetPath(*fixture.context, "", "New folder", "") == "New folder 2");
        CHECK(freeAssetPath(*fixture.context, "materials", "New material", ".material.json") ==
              "materials/New material 2.material.json");
    }
}
