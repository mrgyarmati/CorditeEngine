#include "cordite/editor/NewProject.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/FileSystem.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"
#include "cordite/world/World.h"

#include "support/TestSupport.h"

#include <filesystem>
#include <string>

using namespace cordite;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// The entity of a world with a name, which must be there.
Entity named(World& world, std::string_view name) {
    const Entity entity = world.findByName(name);
    REQUIRE_MESSAGE(!entity.isNull(), "no entity is called " << std::string(name));
    return entity;
}

} // namespace

TEST_SUITE("NewProject") {
    TEST_CASE("a new project has a start scene with something to look at, and light to see it by") {
        const TempDirectory directory;
        const Project made = unwrap(createStarterProject(directory.path() / "depot", "Depot: the Game"));
        CHECK(made.settings.name == "Depot: the Game");
        CHECK(made.settings.startScene == kStarterScene);
        CHECK(unwrap(loadProject(directory.path() / "depot")).settings == made.settings);

        // The scene is read as strictly as any other, with the engine's components.
        ComponentRegistry components;
        REQUIRE(registerEngineComponents(components).has_value());
        World world;
        REQUIRE(loadScene(made.startScenePath(), world, components).has_value());
        CHECK(world.sceneName() == "Depot: the Game");

        const Entity camera = named(world, "Camera");
        REQUIRE(world.has<CameraComponent>(camera));
        CHECK(world.tryGet<CameraComponent>(camera)->primary);
        const Transform& view = world.registry().get<Transform>(camera.handle);
        // It looks at the box, from above and in front of it.
        CHECK(view.position.y > 0.5f);
        CHECK(glm::dot(view.forward(), glm::normalize(Vec3(0.0f, 0.5f, 0.0f) - view.position)) > 0.999f);

        const LightComponent* sun = world.tryGet<LightComponent>(named(world, "Sun"));
        REQUIRE(sun != nullptr);
        CHECK(sun->type == LightType::Directional);
        CHECK(sun->castsShadows);
        CHECK(world.has<AmbientLightComponent>(named(world, "Sky")));

        // The built-in meshes and material: a project with no other file.
        CHECK(world.tryGet<MeshRendererComponent>(named(world, "Floor"))->mesh == builtinMeshId(BuiltinMesh::Plane));
        CHECK(world.tryGet<MeshRendererComponent>(named(world, "Box"))->mesh == builtinMeshId(BuiltinMesh::Cube));
        size_t files = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(made.assetDirectory())) {
            files += entry.is_regular_file() ? 1 : 0;
        }
        CHECK(files == 1);
    }

    TEST_CASE("a new project is called after its directory unless it is given a name") {
        const TempDirectory directory;
        CHECK(unwrap(createStarterProject(directory.path() / "harbour", "")).settings.name == "harbour");
        CHECK(unwrap(createStarterProject(directory.path() / "yard" / "", "")).settings.name == "yard");
    }

    TEST_CASE("a new project writes over nothing, and what it wrote goes when it cannot be finished") {
        const TempDirectory directory;
        // A project is there already.
        const std::filesystem::path taken = directory.path() / "taken";
        REQUIRE(writeTextFile(taken / "project.cordite", "mine").has_value());
        const Result<Project> again = createStarterProject(taken, "Again");
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::AlreadyExists);
        CHECK(unwrap(readTextFile(taken / "project.cordite")) == "mine");

        // A scene where the new one would go.
        const std::filesystem::path scened = directory.path() / "scened";
        const std::filesystem::path scene = scened / "assets" / "scenes" / "main.scene.json";
        REQUIRE(writeTextFile(scene, "mine").has_value());
        const Result<Project> over = createStarterProject(scened, "Over");
        REQUIRE_FALSE(over.has_value());
        CHECK(over.error().code == ErrorCode::AlreadyExists);
        CHECK(unwrap(readTextFile(scene)) == "mine");
        CHECK_FALSE(std::filesystem::exists(scened / "project.cordite"));

        // Assets that are there are kept, and become the project's.
        const std::filesystem::path stocked = directory.path() / "stocked";
        REQUIRE(writeTextFile(stocked / "assets" / "scripts" / "door.lua", "return {}\n").has_value());
        const Project project = unwrap(createStarterProject(stocked, "Stocked"));
        CHECK(fileExists(project.assetDirectory() / "scripts" / "door.lua"));

        // The scene cannot be written: a file is where its directory would be. The project
        // file goes again, and the file that was there stays.
        const std::filesystem::path blocked = directory.path() / "blocked";
        REQUIRE(writeTextFile(blocked / "assets" / "scenes", "a file").has_value());
        REQUIRE_FALSE(createStarterProject(blocked, "Blocked").has_value());
        CHECK_FALSE(std::filesystem::exists(blocked / "project.cordite"));
        CHECK(unwrap(readTextFile(blocked / "assets" / "scenes")) == "a file");
    }
}
