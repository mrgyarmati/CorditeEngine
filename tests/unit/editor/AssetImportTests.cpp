#include "cordite/editor/AssetImport.h"

#include "cordite/assets/AssetManager.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/FileSystem.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/SceneAssets.h"

#include "support/TestSupport.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A project, and beside it files from elsewhere: a texture, a note, and a glTF file with
/// its buffer and a texture it names in a directory next to its own.
struct ImportFixture {
    TempDirectory directory;
    std::filesystem::path root = directory.path() / "project" / "assets";
    std::filesystem::path downloads = directory.path() / "downloads";
    ComponentRegistry components;
    World world;
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<EditorContext> context;

    ImportFixture() {
        REQUIRE(registerEngineComponents(components).has_value());
        REQUIRE(ensureDirectory(root).has_value());
        writeTexture(downloads / "rust.png");
        writeTexture(downloads / "other" / "rust.png");
        writeTexture(downloads / "textures" / "wall.png");
        REQUIRE(writeTextFile(downloads / "notes.txt", "Bring the cart.\n").has_value());
        writeModel(downloads / "models" / "cart.gltf", "../textures/wall.png");
        writeModel(downloads / "models" / "wagon.gltf", "../textures/wall.png");
        assets = unwrap(AssetManager::create(
            {.rootDirectory = root, .writeMetaFiles = true, .readReferences = assetReferenceReader(components)}));
        context = std::make_unique<EditorContext>(world, components, *assets);
    }

    static void writeTexture(const std::filesystem::path& file) {
        REQUIRE(ensureDirectory(file.parent_path()).has_value());
        const Image image = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        REQUIRE(image.savePng(file).has_value());
    }

    /// A model with a mesh and a material whose color is the image at `image`, by where it is.
    static void writeModel(const std::filesystem::path& file, std::string_view image) {
        GltfDocument document;
        document.images.push_back({.name = "Wall", .uri = std::string(image)});
        GltfMaterial paint;
        paint.name = "Paint";
        paint.images.baseColor = 0;
        document.materials.push_back(paint);
        GltfMesh body;
        body.mesh = makeBox();
        body.mesh.name = "Body";
        body.materials = {0};
        document.meshes.push_back(body);
        document.nodes.push_back({.name = "Body", .mesh = 0});
        document.roots = {0};
        REQUIRE(ensureDirectory(file.parent_path()).has_value());
        REQUIRE(exportGltf(document, file).has_value());
    }

    [[nodiscard]] bool has(std::string_view reference) const { return fileExists(root / pathFromUtf8(reference)); }

    /// Every file and directory of the project, from its top.
    [[nodiscard]] std::vector<std::string> contents() const {
        std::vector<std::string> paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            paths.push_back(pathToUtf8(entry.path()));
        }
        return paths;
    }

    /// The error of an import that is refused, after checking that nothing came in.
    Error refusal(std::vector<std::filesystem::path> files, std::string to = {}) {
        const uint64_t revision = assets->database().revision();
        const std::vector<std::string> before = contents();
        const Result<AssetImport> imported = importAssets(*context, std::move(files), std::move(to));
        REQUIRE_FALSE(imported.has_value());
        CHECK(assets->database().revision() == revision);
        CHECK(contents() == before);
        return imported.error();
    }
};

} // namespace

TEST_SUITE("AssetImport") {
    TEST_CASE("files are brought in under their own names, and a glTF file with what it names, beside it") {
        ImportFixture fixture;
        // What the file was called by in another project stays there.
        REQUIRE(
            writeTextFile(fixture.downloads / "rust.png.meta",
                          R"({"format": "cordite.asset", "version": 1, "id": "22222222-2222-4222-8222-222222222222"})")
                .has_value());
        AssetImport imported = unwrap(importAssets(*fixture.context, {fixture.downloads / "rust.png"}, ""));
        CHECK(imported.files == std::vector<std::string>{"rust.png"});
        const AssetInfo* rust = fixture.assets->database().findByReference("rust.png");
        REQUIRE(rust != nullptr);
        CHECK(rust->id.toString() != "22222222-2222-4222-8222-222222222222");

        // Two models that name one texture bring it in once, where it was beside them.
        imported = unwrap(importAssets(
            *fixture.context, {fixture.downloads / "models" / "cart.gltf", fixture.downloads / "models" / "wagon.gltf"},
            "vehicles"));
        CHECK(imported.files == std::vector<std::string>{"vehicles/cart.gltf", "vehicles/cart.bin", "textures/wall.png",
                                                         "vehicles/wagon.gltf", "vehicles/wagon.bin"});
        const AssetDatabase& database = fixture.assets->database();
        CHECK(database.problems().empty());
        const AssetInfo* wall = database.findByReference("textures/wall.png");
        REQUIRE(wall != nullptr);
        CHECK(database.dependents(wall->id).size() == 2);
        CHECK(fixture.assets->loadMesh(database.findByReference("vehicles/cart.gltf#mesh/0")->id).has_value());
    }

    TEST_CASE("what cannot be brought in is refused, and nothing comes in") {
        ImportFixture fixture;
        const std::filesystem::path rust = fixture.downloads / "rust.png";
        const auto refused = [&](std::vector<std::filesystem::path> files, std::string to, ErrorCode code,
                                 std::string_view words) {
            const Error error = fixture.refusal(std::move(files), std::move(to));
            CHECK(error.code == code);
            CHECK_MESSAGE(error.message.find(words) != std::string::npos, error.message);
        };
        refused({}, "", ErrorCode::InvalidArgument, "name the files");
        refused({rust, fixture.downloads / "gone.png"}, "", ErrorCode::NotFound, "gone.png is not there");
        refused({rust, fixture.downloads / "textures"}, "", ErrorCode::InvalidArgument, "is a directory");
        refused({rust, fixture.downloads / "notes.txt"}, "", ErrorCode::InvalidArgument,
                "notes.txt is no kind of asset");
        refused({rust, fixture.downloads / "other" / "rust.png"}, "", ErrorCode::InvalidArgument,
                "would both be rust.png");
        refused({rust}, ".hidden", ErrorCode::InvalidArgument, "begins with a dot");
        refused({rust}, "../outside", ErrorCode::InvalidArgument, "a part that is '..'");
        // A model at the top would bring its texture to a place above the assets.
        refused({rust, fixture.downloads / "models" / "cart.gltf"}, "", ErrorCode::InvalidArgument,
                "cart.gltf names '../textures/wall.png', which would land outside the assets");
        // A file of the project is moved, not brought in.
        ImportFixture::writeTexture(fixture.root / "textures" / "brick.png");
        refused({fixture.root / "textures" / "brick.png"}, "", ErrorCode::InvalidArgument,
                "among the assets already, as textures/brick.png");
        // What is there already stays as it is.
        ImportFixture::writeTexture(fixture.root / "textures" / "wall.png");
        refused({rust, fixture.downloads / "textures" / "wall.png"}, "textures", ErrorCode::AlreadyExists,
                "there is a file at textures/wall.png already");
    }

    TEST_CASE("a copy that fails takes back the copies before it") {
        // Two names that differ only in case are one place on a file system that does not
        // tell them apart - Windows', macOS' - which only the second copy finds out.
        ImportFixture fixture;
        ImportFixture::writeTexture(fixture.downloads / "upper" / "Moss.png");
        ImportFixture::writeTexture(fixture.downloads / "lower" / "moss.png");
        std::error_code error;
        const bool caseBlind = std::filesystem::equivalent(fixture.downloads / "upper" / "Moss.png",
                                                           fixture.downloads / "upper" / "moss.png", error) &&
                               !error;
        const std::vector<std::string> before = fixture.contents();
        const Result<AssetImport> imported = importAssets(
            *fixture.context, {fixture.downloads / "lower" / "moss.png", fixture.downloads / "upper" / "Moss.png"}, "");
        if (caseBlind) {
            REQUIRE_FALSE(imported.has_value());
            CHECK(imported.error().code == ErrorCode::IoError);
            CHECK(fixture.contents() == before);
        } else {
            CHECK(imported.has_value());
        }
    }

    TEST_CASE("an OBJ file comes with its MTL file and the images that names, beside it") {
        ImportFixture fixture;
        REQUIRE(writeTextFile(fixture.downloads / "models" / "crate.mtl",
                              "newmtl Wood\nKd 1 1 1\nmap_Kd ../textures/wall.png\n")
                    .has_value());
        REQUIRE(writeTextFile(fixture.downloads / "models" / "crate.obj",
                              "mtllib crate.mtl\no Crate\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\n"
                              "usemtl Wood\nf 1/1 2/2 3/3\n")
                    .has_value());
        const AssetImport imported =
            unwrap(importAssets(*fixture.context, {fixture.downloads / "models" / "crate.obj"}, "props"));
        CHECK(imported.files == std::vector<std::string>{"props/crate.obj", "props/crate.mtl", "textures/wall.png"});
        const AssetDatabase& database = fixture.assets->database();
        const AssetInfo* wall = database.findByReference("textures/wall.png");
        REQUIRE(wall != nullptr);
        CHECK(database.dependents(wall->id).size() == 1);
        CHECK(fixture.assets->loadMesh(database.findByReference("props/crate.obj#mesh/0")->id).has_value());
    }

    TEST_CASE("a glTF file that names a file that is not there is refused") {
        ImportFixture fixture;
        ImportFixture::writeModel(fixture.downloads / "models" / "lost.gltf", "missing.png");
        const Error error = fixture.refusal({fixture.downloads / "models" / "lost.gltf"}, "vehicles");
        CHECK(error.code == ErrorCode::NotFound);
        CHECK(error.message.find("names 'missing.png', which is not there") != std::string::npos);
    }

    TEST_CASE("without a project there is nowhere to bring files") {
        ComponentRegistry components;
        World world;
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        EditorContext context(world, components, *assets);
        const Result<AssetImport> imported = importAssets(context, {pathFromUtf8("rust.png")}, "");
        REQUIRE_FALSE(imported.has_value());
        CHECK(imported.error().code == ErrorCode::InvalidState);
    }
}
