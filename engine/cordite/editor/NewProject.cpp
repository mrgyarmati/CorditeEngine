#include "cordite/editor/NewProject.h"

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/FileSystem.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"
#include "cordite/world/World.h"

#include <system_error>
#include <utility>

namespace cordite {
namespace {

Transform& placementOf(World& world, Entity entity) {
    // Every entity has one.
    return world.registry().get<Transform>(entity.handle);
}

/// The scene of a new project: something to look at, and the light to see it by.
void buildStarterScene(World& world, std::string_view name) {
    world.setSceneName(name);

    const Entity camera = world.createEntity("Camera");
    Transform& view = placementOf(world, camera);
    view.position = Vec3(0.0f, 2.0f, 4.5f);
    view.rotation = lookRotation(Vec3(0.0f, 0.5f, 0.0f) - view.position);
    world.add<CameraComponent>(camera);

    // A warm sun from above, behind the camera and to its left: one face of the box that
    // the camera sees is lit, the other turned away, and the shadow falls ahead to the right.
    const Entity sun = world.createEntity("Sun");
    placementOf(world, sun).rotation = lookRotation(Vec3(0.5f, -1.0f, -0.7f));
    auto& sunlight = world.add<LightComponent>(sun);
    sunlight.type = LightType::Directional;
    sunlight.color = Vec3(1.0f, 0.9f, 0.78f);
    sunlight.intensity = 0.65f;
    sunlight.castsShadows = true;

    // The cool light of the sky from above, and of the ground from below: what the sun does
    // not reach is not black.
    world.add<AmbientLightComponent>(world.createEntity("Sky")).intensity = 0.7f;

    const Entity floor = world.createEntity("Floor");
    placementOf(world, floor).scale = Vec3(20.0f, 1.0f, 20.0f);
    world.add<MeshRendererComponent>(floor).mesh = builtinMeshId(BuiltinMesh::Plane);

    const Entity box = world.createEntity("Box");
    Transform& boxPlacement = placementOf(world, box);
    boxPlacement.position = Vec3(0.0f, 0.5f, 0.0f);
    boxPlacement.rotation = glm::angleAxis(toRadians(35.0f), kWorldUp);
    world.add<MeshRendererComponent>(box).mesh = builtinMeshId(BuiltinMesh::Cube);
}

} // namespace

Result<Project> createStarterProject(const std::filesystem::path& directory, std::string name) {
    if (name.empty()) {
        name = pathToUtf8(directory.lexically_normal().filename());
        if (name.empty()) {
            name = pathToUtf8(directory.lexically_normal().parent_path().filename());
        }
    }
    const Project place{.directory = directory};
    if (fileExists(place.file())) {
        return makeError(ErrorCode::AlreadyExists, "{} holds a project already", pathToUtf8(directory));
    }
    // A directory that is there may hold assets to make a game of; none of them is written over.
    const std::filesystem::path scene = place.assetDirectory() / pathFromUtf8(kStarterScene);
    if (fileExists(scene)) {
        return makeError(ErrorCode::AlreadyExists, "{} is there already, and a new project would write over it",
                         pathToUtf8(scene));
    }
    std::error_code error;
    const bool existed = std::filesystem::exists(directory, error);

    ComponentRegistry components;
    CORDITE_TRY(registerEngineComponents(components));
    World world;
    buildStarterScene(world, name);

    const auto made = [&]() -> Result<Project> {
        CORDITE_TRY_ASSIGN(Project project, createProject(directory, std::move(name)));
        CORDITE_TRY(saveScene(scene, world, components));
        project.settings.startScene = std::string(kStarterScene);
        CORDITE_TRY(saveProject(project));
        return project;
    }();
    if (!made) {
        // What was written goes - a project without its scene is no project to begin with -
        // and what was there stays.
        if (existed) {
            std::filesystem::remove(place.file(), error);
            std::filesystem::remove(scene, error);
        } else {
            std::filesystem::remove_all(directory, error);
        }
    }
    return made;
}

} // namespace cordite
