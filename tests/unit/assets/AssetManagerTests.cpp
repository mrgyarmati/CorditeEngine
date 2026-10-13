#include "cordite/assets/AssetManager.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Primitives.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/Json.h"

#include "AssetTestSupport.h"
#include "support/SkinnedModelSupport.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::LogCapture;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// A small project: two textures, a model that uses one of them and embeds another, and
/// whatever material files a test adds.
class Project {
public:
    Project() : m_root(m_directory.path() / "assets") {
        test::writeSolidPng(file("textures/wall.png"), Vec4(1.0f, 0.0f, 0.0f, 1.0f));
        test::writeSolidPng(file("textures/floor.png"), Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        writeModel("models/crate.gltf");
    }

    [[nodiscard]] std::filesystem::path file(std::string_view relativePath) const {
        return m_root / pathFromUtf8(relativePath);
    }

    void writeText(std::string_view relativePath, std::string_view text) const {
        REQUIRE(writeTextFile(file(relativePath), text).has_value());
    }

    /// Writes a material file made of the given members, after the header.
    void writeMaterial(std::string_view relativePath, std::string_view members) const {
        writeText(relativePath, fmt::format(R"({{"format": "cordite.material", "version": 1{}{}}})",
                                            members.empty() ? "" : ", ", members));
    }

    /// Writes a WAV file of `frames` frames, each sample a step louder than the last.
    void writeSound(std::string_view relativePath, uint32_t sampleRate, uint32_t channels, size_t frames) const {
        SoundData sound{.sampleRate = sampleRate, .channels = channels};
        for (size_t sample = 0; sample < frames * channels; ++sample) {
            sound.samples.push_back(static_cast<float>(sample) / static_cast<float>(frames * channels));
        }
        REQUIRE(writeBinaryFile(file(relativePath), unwrap(encodeWav(sound))).has_value());
    }

    void writeModel(std::string_view relativePath) const {
        GltfDocument document;
        document.images.push_back({.name = "Wall", .uri = "../textures/wall.png"});
        document.images.push_back(
            {.name = "Label", .encoded = test::encodeSolidPng(2, 2, Vec4(0.0f, 1.0f, 0.0f, 1.0f))});
        document.images.push_back({.name = "Lost", .uri = "../textures/lost.png"});

        GltfMaterial wood;
        wood.name = "Wood";
        wood.material.roughness = 0.75f;
        wood.images.baseColor = 0;
        wood.images.normal = 1;
        wood.images.emissive = 2;
        document.materials.push_back(wood);
        GltfMaterial metal;
        metal.name = "Metal";
        metal.material.metallic = 1.0f;
        document.materials.push_back(metal);

        GltfMesh body;
        body.mesh.name = "Body";
        const MeshData box = makeBox(Vec3(2.0f));
        const MeshData knob = makeSphere(0.25f, 8, 4);
        appendSubmesh(body.mesh, box.vertices, box.indices);
        appendSubmesh(body.mesh, knob.vertices, knob.indices);
        body.materials = {0, std::nullopt};
        document.meshes.push_back(body);

        GltfMesh lid;
        lid.mesh = makePlane(Vec2(2.0f));
        lid.mesh.name = "Lid";
        lid.materials = {1};
        document.meshes.push_back(lid);

        GltfNode root;
        root.name = "Crate";
        root.mesh = 0;
        root.children = {1, 2};
        document.nodes.push_back(root);
        GltfNode lidNode;
        lidNode.name = "Lid";
        lidNode.parent = 0;
        lidNode.mesh = 1;
        lidNode.transform.position = Vec3(0.0f, 1.0f, 0.0f);
        document.nodes.push_back(lidNode);
        GltfNode eye;
        eye.name = "Eye";
        eye.parent = 0;
        eye.camera = ModelCamera{.verticalFov = 0.5f, .nearPlane = 0.25f, .farPlane = 100.0f};
        document.nodes.push_back(eye);
        document.roots = {0};

        REQUIRE(exportGltf(document, file(relativePath)).has_value());
    }

    [[nodiscard]] std::unique_ptr<AssetManager> open() const {
        return unwrap(AssetManager::create({.rootDirectory = m_root}));
    }

private:
    TempDirectory m_directory;
    std::filesystem::path m_root;
};

Uuid idOf(const AssetManager& assets, std::string_view reference) {
    const AssetInfo* asset = assets.database().findByReference(reference);
    REQUIRE_MESSAGE(asset != nullptr, "no asset " << std::string(reference));
    return asset->id;
}

} // namespace

TEST_SUITE("AssetManager") {
    TEST_CASE("a manager without a directory serves the built-in assets") {
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        for (const BuiltinMesh builtin : kBuiltinMeshes) {
            CAPTURE(nameOf(builtin));
            const std::shared_ptr<const MeshData> mesh = unwrap(assets->loadMesh(builtinMeshId(builtin)));
            REQUIRE(mesh != nullptr);
            CHECK(validateMesh(*mesh).has_value());
            CHECK(mesh->name == assets->database().find(builtinMeshId(builtin))->name);
            // Loaded once, shared from then on.
            CHECK(unwrap(assets->loadMesh(builtinMeshId(builtin))) == mesh);
        }
        // The built-in shapes fit a unit cube; the capsule is the size of a person.
        CHECK_NEAR(unwrap(assets->loadMesh(builtinMeshId(BuiltinMesh::Cube)))->bounds.max, Vec3(0.5f), 1.0e-6f);
        CHECK_NEAR(unwrap(assets->loadMesh(builtinMeshId(BuiltinMesh::Capsule)))->bounds.max, Vec3(0.5f, 1.0f, 0.5f),
                   1.0e-6f);

        const std::shared_ptr<const MaterialData> material = unwrap(assets->loadMaterial(defaultMaterialId()));
        CHECK(*material == MaterialData());
    }

    TEST_CASE("a directory that does not exist is an error") {
        const TempDirectory directory;
        const auto assets = AssetManager::create({.rootDirectory = directory.path() / "nowhere"});
        REQUIRE_FALSE(assets.has_value());
        CHECK(assets.error().code == ErrorCode::NotFound);
    }

    TEST_CASE("a mesh inside a model is loaded with the whole model, once") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();

        const std::shared_ptr<const MeshData> body =
            unwrap(assets->loadMesh(idOf(*assets, "models/crate.gltf#mesh/0")));
        CHECK(body->name == "Body");
        CHECK(body->submeshes.size() == 2);
        CHECK(validateMesh(*body).has_value());
        CHECK_NEAR(body->bounds.max, Vec3(1.0f), 1.0e-6f);
        CHECK(unwrap(assets->loadMesh(idOf(*assets, "models/crate.gltf#mesh/0"))) == body);

        // The file is not needed again for anything else in it.
        std::filesystem::remove(project.file("models/crate.gltf"));
        std::filesystem::remove(project.file("models/crate.bin"));
        const std::shared_ptr<const MeshData> lid = unwrap(assets->loadMesh(idOf(*assets, "models/crate.gltf#mesh/1")));
        CHECK(lid->name == "Lid");
        CHECK(unwrap(assets->loadMaterial(idOf(*assets, "models/crate.gltf#material/1")))->metallic == 1.0f);
        CHECK(unwrap(assets->loadModel(idOf(*assets, "models/crate.gltf")))->nodes.size() == 3);
    }

    TEST_CASE("a material inside a model refers to its textures by asset id") {
        const Project project;
        const LogCapture log;
        const std::unique_ptr<AssetManager> assets = project.open();

        const std::shared_ptr<const MaterialData> wood =
            unwrap(assets->loadMaterial(idOf(*assets, "models/crate.gltf#material/0")));
        CHECK(wood->roughness == 0.75f);
        // An image in a file of its own is that file's asset.
        CHECK(wood->baseColorTexture == idOf(*assets, "textures/wall.png"));
        // An embedded image is an asset inside the model.
        CHECK(wood->normalTexture == idOf(*assets, "models/crate.gltf#image/1"));
        // An image that is not there leaves the slot empty; indexing the project said so.
        CHECK(wood->emissiveTexture.isNil());
        CHECK(log.contains("the image '../textures/lost.png' of material 'Wood' is not a texture"));
        CHECK(wood->metallicRoughnessTexture.isNil());

        const std::shared_ptr<const MaterialData> metal =
            unwrap(assets->loadMaterial(idOf(*assets, "models/crate.gltf#material/1")));
        CHECK(metal->metallic == 1.0f);
        CHECK(materialTextures(*metal).empty());
    }

    TEST_CASE("a model describes its nodes with the ids of their meshes and materials") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();
        const std::shared_ptr<const ModelData> model = unwrap(assets->loadModel(idOf(*assets, "models/crate.gltf")));

        CHECK(model->name == "crate");
        CHECK(model->roots == std::vector<uint32_t>{0});
        REQUIRE(model->nodes.size() == 3);

        const ModelNode& root = model->nodes[0];
        CHECK(root.name == "Crate");
        CHECK(root.children == std::vector<uint32_t>{1, 2});
        CHECK(root.mesh == idOf(*assets, "models/crate.gltf#mesh/0"));
        // One material per submesh; the second submesh has none.
        CHECK(root.materials == std::vector<Uuid>{idOf(*assets, "models/crate.gltf#material/0"), Uuid()});

        const ModelNode& lid = model->nodes[1];
        CHECK(lid.parent == 0u);
        CHECK(lid.transform.position == Vec3(0.0f, 1.0f, 0.0f));
        CHECK(lid.mesh == idOf(*assets, "models/crate.gltf#mesh/1"));
        CHECK(lid.materials == std::vector<Uuid>{idOf(*assets, "models/crate.gltf#material/1")});

        const ModelNode& eye = model->nodes[2];
        CHECK(eye.mesh.isNil());
        CHECK(eye.materials.empty());
        REQUIRE(eye.camera.has_value());
        CHECK(eye.camera->verticalFov == 0.5f);
        CHECK(eye.camera->farPlane == 100.0f);

        CHECK(unwrap(assets->loadModel(idOf(*assets, "models/crate.gltf"))) == model);
    }

    TEST_CASE("a skinned model brings its skeleton and its animations along, as assets of their own") {
        const Project project;
        REQUIRE(exportGltf(test::makeSkinnedBar(), project.file("models/bar.gltf")).has_value());
        const std::unique_ptr<AssetManager> assets = project.open();
        // Nothing is wrong with it. (The project has a model that is, on purpose.)
        for (const std::string& problem : assets->database().problems()) {
            CHECK_MESSAGE(problem.find("bar.gltf") == std::string::npos, problem);
        }

        // Listed without loading anything: by what they are in the file, and by name.
        const AssetInfo* skeletonInfo = assets->database().findByReference("models/bar.gltf#skeleton/0");
        REQUIRE(skeletonInfo != nullptr);
        CHECK(skeletonInfo->type == AssetType::Skeleton);
        CHECK(skeletonInfo->name == "Bar skin");
        const AssetInfo* bendInfo = assets->database().findByReference("models/bar.gltf#animation/0");
        const AssetInfo* swayInfo = assets->database().findByReference("models/bar.gltf#animation/1");
        REQUIRE(bendInfo != nullptr);
        REQUIRE(swayInfo != nullptr);
        CHECK(bendInfo->type == AssetType::Animation);
        CHECK(bendInfo->name == "Bend");
        CHECK(swayInfo->name == "Sway");
        // Their ids follow from the file's and from where they are in it.
        const Uuid modelId = idOf(*assets, "models/bar.gltf");
        CHECK(skeletonInfo->id == subAssetId(modelId, "skeleton/0"));
        CHECK(swayInfo->id == subAssetId(modelId, "animation/1"));
        // The model needs them.
        const AssetInfo* modelInfo = assets->database().find(modelId);
        REQUIRE(modelInfo != nullptr);
        for (const Uuid& needed : {skeletonInfo->id, bendInfo->id, swayInfo->id}) {
            CHECK(std::ranges::find(modelInfo->dependencies, needed) != modelInfo->dependencies.end());
        }

        const std::shared_ptr<const SkeletonData> skeleton = unwrap(assets->loadSkeleton(skeletonInfo->id));
        REQUIRE(skeleton->joints.size() == 3);
        CHECK(skeleton->name == "Bar skin");
        CHECK(skeleton->joints[2].name == "Tip");
        CHECK(validateSkeleton(*skeleton).has_value());
        // Loaded once, shared from then on.
        CHECK(unwrap(assets->loadSkeleton(skeletonInfo->id)) == skeleton);

        const std::shared_ptr<const AnimationData> sway = unwrap(assets->loadAnimation(swayInfo->id));
        CHECK(sway->name == "Sway");
        CHECK(sway->duration == 2.0f);
        REQUIRE(sway->tracks.size() == 1);
        CHECK(sway->tracks[0].joint == "Root");
        CHECK(sway->tracks[0].translation.keys().size() == 3);
        CHECK(validateAnimation(*sway).has_value());
        CHECK(unwrap(assets->loadAnimation(swayInfo->id)) == sway);

        // The model says which of its nodes is drawn with the skeleton, and which clips
        // came with it; its mesh says how its vertices follow the joints.
        const std::shared_ptr<const ModelData> model = unwrap(assets->loadModel(modelId));
        REQUIRE(model->nodes.size() == 4);
        CHECK(model->nodes[0].skeleton == skeletonInfo->id);
        CHECK(model->nodes[1].skeleton.isNil());
        // It also says which of its nodes are the joints.
        CHECK_FALSE(model->nodes[0].joint);
        CHECK(model->nodes[1].joint);
        CHECK(model->nodes[2].joint);
        CHECK(model->nodes[3].joint);
        CHECK(model->animations == std::vector<Uuid>{bendInfo->id, swayInfo->id});
        const std::shared_ptr<const MeshData> mesh = unwrap(assets->loadMesh(model->nodes[0].mesh));
        CHECK(mesh->isSkinned());
        CHECK(jointsUsed(*mesh) == 3);

        // Asking for the one as the other, or for what is not there, says so.
        Result<std::shared_ptr<const SkeletonData>> wrong = assets->loadSkeleton(bendInfo->id);
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == ErrorCode::InvalidArgument);
        CHECK(wrong.error().message == "models/bar.gltf#animation/0 is an animation, not a skeleton");
        Result<std::shared_ptr<const AnimationData>> noClip = assets->loadAnimation(skeletonInfo->id);
        REQUIRE_FALSE(noClip.has_value());
        CHECK(noClip.error().message == "models/bar.gltf#skeleton/0 is a skeleton, not an animation");
        CHECK_FALSE(assets->loadAnimation(skeletonInfo->id).has_value());
        CHECK_FALSE(assets->loadAnimation(modelId).has_value());
        wrong = assets->loadSkeleton(subAssetId(modelId, "skeleton/7"));
        REQUIRE_FALSE(wrong.has_value());
        CHECK(wrong.error().code == ErrorCode::NotFound);

        // Refreshing forgets them, and reads them again when they are asked for.
        REQUIRE(assets->refresh().has_value());
        const std::shared_ptr<const SkeletonData> again = unwrap(assets->loadSkeleton(skeletonInfo->id));
        CHECK(again != skeleton);
        CHECK(*again == *skeleton);
    }

    TEST_CASE("a skin that could not be imported is no skeleton, and its mesh is drawn without one") {
        const Project project;
        GltfDocument document = test::makeSkinnedBar();
        REQUIRE(exportGltf(document, project.file("models/bar.gltf")).has_value());
        // The file by hand: the skin lists a joint twice.
        Json file = unwrap(readJsonFile(project.file("models/bar.gltf")));
        file["skins"][0]["joints"] = unwrap(parseJson("[1, 2, 2]"));
        project.writeText("models/bar.gltf", dumpJson(file));

        const LogCapture log;
        const std::unique_ptr<AssetManager> assets = project.open();
        CHECK(assets->database().findByReference("models/bar.gltf#skeleton/0") == nullptr);
        // The animations are still clips; there is nothing here for them to move.
        CHECK(assets->database().findByReference("models/bar.gltf#animation/0") != nullptr);
        const AssetInfo* modelInfo = assets->database().findByReference("models/bar.gltf");
        REQUIRE(modelInfo != nullptr);
        REQUIRE(modelInfo->warnings.size() == 1);
        CHECK(modelInfo->warnings[0].find("as a joint twice") != std::string::npos);

        const std::shared_ptr<const ModelData> model = unwrap(assets->loadModel(modelInfo->id));
        CHECK(model->nodes[0].skeleton.isNil());
        CHECK_FALSE(model->nodes[0].mesh.isNil());
        CHECK(model->animations.size() == 2);
        CHECK_FALSE(assets->loadSkeleton(subAssetId(modelInfo->id, "skeleton/0")).has_value());
    }

    TEST_CASE("textures are decoded from files and from inside models") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();

        const std::shared_ptr<const Image> wall = unwrap(assets->loadTexture(idOf(*assets, "textures/wall.png")));
        CHECK(wall->width() == 4);
        CHECK(wall->format() == ImageFormat::Rgba8);
        CHECK(wall->pixel(0, 0) == Vec4(1.0f, 0.0f, 0.0f, 1.0f));

        const std::shared_ptr<const Image> label =
            unwrap(assets->loadTexture(idOf(*assets, "models/crate.gltf#image/1")));
        CHECK(label->width() == 2);
        CHECK(label->pixel(1, 1) == Vec4(0.0f, 1.0f, 0.0f, 1.0f));
    }

    TEST_CASE("the pixels of a texture are shared while in use and released afterwards") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid id = idOf(*assets, "textures/wall.png");

        std::shared_ptr<const Image> first = unwrap(assets->loadTexture(id));
        CHECK(unwrap(assets->loadTexture(id)) == first);
        // Nobody else holds it: the manager does not keep pixels alive on its own.
        CHECK(first.use_count() == 1);

        // Once released, the next request reads the file again - as it is now.
        first.reset();
        test::writeSolidPng(project.file("textures/wall.png"), Vec4(0.0f, 1.0f, 0.0f, 1.0f));
        CHECK(unwrap(assets->loadTexture(id))->pixel(0, 0) == Vec4(0.0f, 1.0f, 0.0f, 1.0f));
    }

    TEST_CASE("a material file takes what it does not state from its parent") {
        const Project project;
        project.writeMaterial("materials/metal.material.json",
                              R"("metallic": 1, "roughness": 0.2, "doubleSided": true)");
        const std::unique_ptr<AssetManager> first = project.open();
        const Uuid metal = idOf(*first, "materials/metal.material.json");
        const Uuid wall = idOf(*first, "textures/wall.png");

        project.writeMaterial("materials/rust.material.json",
                              fmt::format(R"("parent": "{}", "roughness": 0.9, "baseColorTexture": "{}")",
                                          metal.toString(), wall.toString()));
        const std::unique_ptr<AssetManager> second = project.open();
        const Uuid rust = idOf(*second, "materials/rust.material.json");
        project.writeMaterial(
            "materials/painted.material.json",
            fmt::format(R"("parent": "{}", "baseColor": [0.1, 0.3, 0.2, 1], "metallic": 0.5)", rust.toString()));

        const std::unique_ptr<AssetManager> assets = project.open();
        const std::shared_ptr<const MaterialData> painted =
            unwrap(assets->loadMaterial(idOf(*assets, "materials/painted.material.json")));
        // Its own.
        CHECK(painted->baseColor == Vec4(0.1f, 0.3f, 0.2f, 1.0f));
        CHECK(painted->metallic == 0.5f);
        // From its parent.
        CHECK(painted->roughness == 0.9f);
        CHECK(painted->baseColorTexture == wall);
        // From its parent's parent.
        CHECK(painted->doubleSided);
        // From nobody: the default.
        CHECK(painted->specular == 0.5f);

        // The parents are loaded on the way and are whole materials themselves.
        CHECK(unwrap(assets->loadMaterial(metal))->roughness == 0.2f);
        CHECK(unwrap(assets->loadMaterial(rust))->metallic == 1.0f);
        CHECK(unwrap(assets->loadMaterial(idOf(*assets, "materials/painted.material.json"))) == painted);
    }

    TEST_CASE("a material may have a material from a model as its parent") {
        const Project project;
        const std::unique_ptr<AssetManager> first = project.open();
        project.writeMaterial(
            "materials/worn.material.json",
            fmt::format(R"("parent": "{}", "roughness": 1)", idOf(*first, "models/crate.gltf#material/0").toString()));

        const std::unique_ptr<AssetManager> assets = project.open();
        const std::shared_ptr<const MaterialData> worn =
            unwrap(assets->loadMaterial(idOf(*assets, "materials/worn.material.json")));
        CHECK(worn->roughness == 1.0f);
        CHECK(worn->baseColorTexture == idOf(*assets, "textures/wall.png"));
    }

    TEST_CASE("material files with a parent that cannot be used are refused") {
        const Project project;
        SUBCASE("a parent that does not exist") {
            project.writeMaterial("orphan.material.json", R"("parent": "11111111-1111-4111-8111-111111111111")");
            const std::unique_ptr<AssetManager> assets = project.open();
            const auto material = assets->loadMaterial(idOf(*assets, "orphan.material.json"));
            REQUIRE_FALSE(material.has_value());
            CHECK(material.error().code == ErrorCode::NotFound);
            CHECK(material.error().message.find("orphan.material.json: its parent") != std::string::npos);
        }
        SUBCASE("a parent that is not a material") {
            const std::unique_ptr<AssetManager> first = project.open();
            project.writeMaterial("odd.material.json",
                                  fmt::format(R"("parent": "{}")", idOf(*first, "textures/wall.png").toString()));
            const std::unique_ptr<AssetManager> assets = project.open();
            const auto material = assets->loadMaterial(idOf(*assets, "odd.material.json"));
            REQUIRE_FALSE(material.has_value());
            CHECK(material.error().message.find("textures/wall.png is a texture, not a material") != std::string::npos);
        }
        SUBCASE("a material that is its own parent") {
            project.writeMaterial("loop.material.json", "");
            const std::unique_ptr<AssetManager> first = project.open();
            const Uuid self = idOf(*first, "loop.material.json");
            project.writeMaterial("loop.material.json", fmt::format(R"("parent": "{}")", self.toString()));

            const std::unique_ptr<AssetManager> assets = project.open();
            const auto material = assets->loadMaterial(self);
            REQUIRE_FALSE(material.has_value());
            CHECK(material.error().code == ErrorCode::InvalidArgument);
            CHECK(material.error().message.find("leads back to itself") != std::string::npos);
        }
        SUBCASE("two materials that are each other's parent") {
            project.writeMaterial("a.material.json", "");
            project.writeMaterial("b.material.json", "");
            const std::unique_ptr<AssetManager> first = project.open();
            const Uuid a = idOf(*first, "a.material.json");
            const Uuid b = idOf(*first, "b.material.json");
            project.writeMaterial("a.material.json", fmt::format(R"("parent": "{}")", b.toString()));
            project.writeMaterial("b.material.json", fmt::format(R"("parent": "{}")", a.toString()));

            const std::unique_ptr<AssetManager> assets = project.open();
            CHECK_FALSE(assets->loadMaterial(a).has_value());
            CHECK_FALSE(assets->loadMaterial(b).has_value());
        }
    }

    TEST_CASE("a material file that is wrong names the file and the mistake") {
        const Project project;
        project.writeMaterial("typo.material.json", R"("rougness": 0.5)");
        const std::unique_ptr<AssetManager> assets = project.open();
        const auto material = assets->loadMaterial(idOf(*assets, "typo.material.json"));
        REQUIRE_FALSE(material.has_value());
        CHECK(material.error().message.find("typo.material.json") != std::string::npos);
        CHECK(material.error().message.find("rougness") != std::string::npos);
    }

    TEST_CASE("asking for the wrong thing fails with a clear message") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();

        SUBCASE("an id that names nothing") {
            const Uuid unknown = *Uuid::parse("11111111-1111-4111-8111-111111111111");
            const auto mesh = assets->loadMesh(unknown);
            REQUIRE_FALSE(mesh.has_value());
            CHECK(mesh.error().code == ErrorCode::NotFound);
            CHECK(mesh.error().message == "there is no asset with the id 11111111-1111-4111-8111-111111111111");
            CHECK(assets->loadMaterial(unknown).error().code == ErrorCode::NotFound);
            CHECK(assets->loadTexture(unknown).error().code == ErrorCode::NotFound);
            CHECK(assets->loadModel(unknown).error().code == ErrorCode::NotFound);
            CHECK(assets->loadMesh(Uuid()).error().code == ErrorCode::NotFound);
        }
        SUBCASE("an asset of another type") {
            const auto mesh = assets->loadMesh(idOf(*assets, "textures/wall.png"));
            REQUIRE_FALSE(mesh.has_value());
            CHECK(mesh.error().code == ErrorCode::InvalidArgument);
            CHECK(mesh.error().message == "textures/wall.png is a texture, not a mesh");

            CHECK(assets->loadTexture(idOf(*assets, "models/crate.gltf#mesh/0")).error().message ==
                  "models/crate.gltf#mesh/0 is a mesh, not a texture");
            CHECK(assets->loadModel(builtinMeshId(BuiltinMesh::Cube)).error().message ==
                  "builtin#mesh/cube is a mesh, not a model");
            CHECK(assets->loadMaterial(idOf(*assets, "models/crate.gltf")).error().message ==
                  "models/crate.gltf is a model, not a material");
        }
    }

    TEST_CASE("a model that cannot be read fails the same way for everything in it, and is read once") {
        const Project project;
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid mesh = idOf(*assets, "models/crate.gltf#mesh/0");
        const Uuid material = idOf(*assets, "models/crate.gltf#material/0");

        // Indexed while it was fine; its geometry is gone by the time it is needed.
        std::filesystem::remove(project.file("models/crate.bin"));
        const auto first = assets->loadMesh(mesh);
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().message.find("crate.bin") != std::string::npos);

        // Even with the file gone altogether the answer is the remembered one.
        std::filesystem::remove(project.file("models/crate.gltf"));
        const auto second = assets->loadMaterial(material);
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().message == first.error().message);
        CHECK(assets->loadModel(idOf(*assets, "models/crate.gltf")).error().message == first.error().message);
    }

    TEST_CASE("a texture file that is not an image is an error, not a crash") {
        const Project project;
        project.writeText("textures/fake.png", "not a PNG at all");
        const std::unique_ptr<AssetManager> assets = project.open();
        const auto texture = assets->loadTexture(idOf(*assets, "textures/fake.png"));
        REQUIRE_FALSE(texture.has_value());
        CHECK(texture.error().message.find("fake.png") != std::string::npos);
    }

    TEST_CASE("refreshing forgets what was loaded and reads the files as they are now") {
        const Project project;
        project.writeMaterial("paint.material.json", R"("roughness": 0.25)");
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid paint = idOf(*assets, "paint.material.json");
        const uint64_t revision = assets->database().revision();

        const std::shared_ptr<const MaterialData> before = unwrap(assets->loadMaterial(paint));
        const std::shared_ptr<const MeshData> meshBefore =
            unwrap(assets->loadMesh(idOf(*assets, "models/crate.gltf#mesh/1")));
        CHECK(before->roughness == 0.25f);

        project.writeMaterial("paint.material.json", R"("roughness": 0.75)");
        test::writeSolidPng(project.file("textures/new.png"), Vec4(1.0f));
        // Without a refresh nothing changes: what was loaded stays loaded.
        CHECK(unwrap(assets->loadMaterial(paint)) == before);
        CHECK(assets->database().findByReference("textures/new.png") == nullptr);

        REQUIRE(assets->refresh().has_value());
        CHECK(assets->database().revision() != revision);
        CHECK(assets->database().findByReference("textures/new.png") != nullptr);
        CHECK(unwrap(assets->loadMaterial(paint))->roughness == 0.75f);
        // What was handed out earlier is still valid, and unchanged.
        CHECK(before->roughness == 0.25f);
        CHECK(meshBefore->name == "Lid");
        CHECK(unwrap(assets->loadMesh(idOf(*assets, "models/crate.gltf#mesh/1"))) != meshBefore);
    }

    TEST_CASE("a script is loaded as the text of its file, and read again after a refresh") {
        const Project project;
        const std::string text = "-- A door.\nlocal Door = {}\r\nreturn Door\n";
        project.writeText("scripts/door.lua", text);
        project.writeText("scripts/empty.lua", "");
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid door = idOf(*assets, "scripts/door.lua");

        // As it is: every byte, line ends and all.
        const std::shared_ptr<const ScriptSource> loaded = unwrap(assets->loadScript(door));
        CHECK(loaded->text == text);
        CHECK(unwrap(assets->loadScript(door)) == loaded);
        CHECK(unwrap(assets->loadScript(idOf(*assets, "scripts/empty.lua")))->text.empty());

        project.writeText("scripts/door.lua", "return {}");
        CHECK(unwrap(assets->loadScript(door))->text == text);
        REQUIRE(assets->refresh().has_value());
        CHECK(unwrap(assets->loadScript(door))->text == "return {}");
        // What was handed out before is what it was.
        CHECK(loaded->text == text);
    }

    TEST_CASE("a script that is no script, too large or gone is an error that names it") {
        const Project project;
        project.writeText("scripts/door.lua", "return {}");
        project.writeText("scripts/huge.lua", std::string(kMaxScriptBytes + 1, ' '));
        project.writeText("scripts/full.lua", std::string(kMaxScriptBytes, ' '));
        const std::unique_ptr<AssetManager> assets = project.open();

        const auto mesh = assets->loadScript(idOf(*assets, "models/crate.gltf#mesh/0"));
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::InvalidArgument);
        CHECK(mesh.error().message == "models/crate.gltf#mesh/0 is a mesh, not a script");
        CHECK(assets->loadMesh(idOf(*assets, "scripts/door.lua")).error().message ==
              "scripts/door.lua is a script, not a mesh");
        CHECK(assets->loadScript(Uuid::fromName(Uuid(), "no such script")).error().code == ErrorCode::NotFound);

        // As much as a script may hold, and one byte more.
        CHECK(unwrap(assets->loadScript(idOf(*assets, "scripts/full.lua")))->text.size() == kMaxScriptBytes);
        const auto huge = assets->loadScript(idOf(*assets, "scripts/huge.lua"));
        REQUIRE_FALSE(huge.has_value());
        CHECK(huge.error().code == ErrorCode::InvalidArgument);
        CHECK(huge.error().message == "scripts/huge.lua has 4194305 bytes; a script has 4194304 at most");

        // A file that was there when the directory was read, and is not any more.
        const Uuid door = idOf(*assets, "scripts/door.lua");
        REQUIRE(std::filesystem::remove(project.file("scripts/door.lua")));
        const auto gone = assets->loadScript(door);
        REQUIRE_FALSE(gone.has_value());
        CHECK(gone.error().code == ErrorCode::IoError);
        CHECK(gone.error().message.starts_with("scripts/door.lua cannot be read: "));
    }

    TEST_CASE("a sound is decoded from its file, once, and again after a refresh") {
        const Project project;
        project.writeSound("sounds/click.wav", 22050, 2, 300);
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid click = idOf(*assets, "sounds/click.wav");

        const std::shared_ptr<const SoundData> loaded = unwrap(assets->loadSound(click));
        CHECK(loaded->sampleRate == 22050);
        CHECK(loaded->channels == 2);
        CHECK(loaded->frames() == 300);
        // The samples of the file: nothing at first, half as loud as can be half-way.
        CHECK(loaded->samples[0] == 0.0f);
        CHECK(loaded->samples[300] == 16384.0f / 32768.0f);
        CHECK(validateSound(*loaded).has_value());
        CHECK(unwrap(assets->loadSound(click)) == loaded);

        // It is kept, whether anybody holds on to it or not: what plays a sound again
        // does not wait for a file to be decoded.
        project.writeSound("sounds/click.wav", 8000, 1, 40);
        CHECK(unwrap(assets->loadSound(click)) == loaded);
        REQUIRE(assets->refresh().has_value());
        const std::shared_ptr<const SoundData> again = unwrap(assets->loadSound(click));
        CHECK(again->sampleRate == 8000);
        CHECK(again->channels == 1);
        CHECK(again->frames() == 40);
        // What was handed out before is what it was.
        CHECK(loaded->frames() == 300);
    }

    TEST_CASE("a sound that is none, too large or gone is an error that names it") {
        const Project project;
        project.writeSound("sounds/click.wav", 48000, 1, 10);
        project.writeText("sounds/words.wav", "these are words, whatever the name says");
        project.writeText("sounds/nothing.mp3", "");
        // More than a sound file may hold. Nothing is written into it: it is refused
        // for its size, before anything of it is read.
        project.writeText("sounds/huge.flac", "fLaC");
        std::filesystem::resize_file(project.file("sounds/huge.flac"), kMaxSoundFileBytes + 1);
        const std::unique_ptr<AssetManager> assets = project.open();

        const auto mesh = assets->loadSound(idOf(*assets, "models/crate.gltf#mesh/0"));
        REQUIRE_FALSE(mesh.has_value());
        CHECK(mesh.error().code == ErrorCode::InvalidArgument);
        CHECK(mesh.error().message == "models/crate.gltf#mesh/0 is a mesh, not a sound");
        CHECK(assets->loadMesh(idOf(*assets, "sounds/click.wav")).error().message ==
              "sounds/click.wav is a sound, not a mesh");
        CHECK(assets->loadSound(Uuid::fromName(Uuid(), "no such sound")).error().code == ErrorCode::NotFound);

        const auto words = assets->loadSound(idOf(*assets, "sounds/words.wav"));
        REQUIRE_FALSE(words.has_value());
        CHECK(words.error().code == ErrorCode::ParseError);
        CHECK(words.error().message.starts_with("sounds/words.wav: it is no WAV, FLAC or MP3 file that can be read: "));
        const auto nothing = assets->loadSound(idOf(*assets, "sounds/nothing.mp3"));
        REQUIRE_FALSE(nothing.has_value());
        CHECK(nothing.error().code == ErrorCode::ParseError);
        CHECK(nothing.error().message == "sounds/nothing.mp3: the file is empty");
        // Asked again, it is read again, and is as wrong as before.
        CHECK_FALSE(assets->loadSound(idOf(*assets, "sounds/words.wav")).has_value());

        const auto huge = assets->loadSound(idOf(*assets, "sounds/huge.flac"));
        REQUIRE_FALSE(huge.has_value());
        CHECK(huge.error().code == ErrorCode::InvalidArgument);
        CHECK(huge.error().message == "sounds/huge.flac has 268435457 bytes; a sound file has 268435456 at most");

        // A file that was there when the directory was read, and is not any more.
        const Uuid click = idOf(*assets, "sounds/click.wav");
        REQUIRE(std::filesystem::remove(project.file("sounds/click.wav")));
        const auto gone = assets->loadSound(click);
        REQUIRE_FALSE(gone.has_value());
        CHECK(gone.error().code == ErrorCode::IoError);
        CHECK(gone.error().message.starts_with("sounds/click.wav cannot be read: "));
    }

    TEST_CASE("a scene is listed, but is not something the manager loads") {
        const Project project;
        project.writeText("main.scene.json", "{}");
        const std::unique_ptr<AssetManager> assets = project.open();
        const Uuid scene = idOf(*assets, "main.scene.json");
        CHECK(assets->database().find(scene)->type == AssetType::Scene);
        CHECK(assets->loadModel(scene).error().message == "main.scene.json is a scene, not a model");
    }
}
