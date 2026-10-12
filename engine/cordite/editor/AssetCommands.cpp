#include "cordite/editor/AssetCommands.h"

#include "cordite/assets/Animation.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/math/MathJson.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

using Type = CommandParameterType;

constexpr const char* kAssetReference =
    "An asset: its id, or its path within the asset directory, followed by '#' and its place in the file for an "
    "asset inside another (\"models/crate.gltf#mesh/0\"). asset.list shows both for every asset.";

/// The short form in which results refer to an asset.
Json describeAsset(const AssetInfo& asset) {
    Json description = Json::object();
    description["id"] = asset.id.toString();
    description["type"] = nameOf(asset.type);
    description["name"] = asset.name;
    description["path"] = asset.reference();
    return description;
}

Json describeProblems(const AssetDatabase& database) {
    Json problems = Json::array();
    for (const std::string& problem : database.problems()) {
        problems.push_back(problem);
    }
    return problems;
}

/// What is inside an asset. Loads it, so a broken asset fails here with the reason.
Result<Json> describeContent(AssetManager& assets, const AssetInfo& asset) {
    Json content = Json::object();
    switch (asset.type) {
        case AssetType::Mesh: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const MeshData> mesh, assets.loadMesh(asset.id));
            content["vertices"] = mesh->vertices.size();
            content["triangles"] = mesh->triangleCount();
            // A mesh renderer takes one material per submesh.
            content["submeshes"] = mesh->submeshes.size();
            content["bounds"] = toJson(mesh->bounds);
            // A skinned mesh needs a skeleton of at least this many joints.
            content["skinned"] = mesh->isSkinned();
            content["joints"] = jointsUsed(*mesh);
            break;
        }
        case AssetType::Material: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const MaterialData> material, assets.loadMaterial(asset.id));
            content["properties"] = materialTypeInfo().toJson(material.get());
            break;
        }
        case AssetType::Texture: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const Image> image, assets.loadTexture(asset.id));
            content["width"] = image->width();
            content["height"] = image->height();
            content["format"] = nameOf(image->format());
            break;
        }
        case AssetType::Model: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> model, assets.loadModel(asset.id));
            Json nodes = Json::array();
            for (const ModelNode& node : model->nodes) {
                Json entry = Json::object();
                entry["name"] = node.name;
                entry["parent"] = node.parent ? Json(*node.parent) : Json(nullptr);
                entry["mesh"] = node.mesh.isNil() ? Json(nullptr) : Json(node.mesh.toString());
                entry["skeleton"] = node.skeleton.isNil() ? Json(nullptr) : Json(node.skeleton.toString());
                entry["camera"] = node.camera.has_value();
                nodes.push_back(std::move(entry));
            }
            content["nodes"] = std::move(nodes);
            content["roots"] = model->roots;
            Json animations = Json::array();
            for (const Uuid& animation : model->animations) {
                animations.push_back(animation.toString());
            }
            content["animations"] = std::move(animations);
            break;
        }
        case AssetType::Skeleton: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const SkeletonData> skeleton, assets.loadSkeleton(asset.id));
            Json joints = Json::array();
            for (const SkeletonJoint& joint : skeleton->joints) {
                Json entry = Json::object();
                entry["name"] = joint.name;
                entry["parent"] = joint.parent == kNoJoint ? Json(nullptr) : Json(joint.parent);
                joints.push_back(std::move(entry));
            }
            content["joints"] = std::move(joints);
            break;
        }
        case AssetType::Animation: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const AnimationData> animation, assets.loadAnimation(asset.id));
            content["duration"] = jsonFromFloat(animation->duration);
            // The joints the clip moves: it fits a skeleton that has joints of these names.
            Json joints = Json::array();
            for (const AnimationTrack& track : animation->tracks) {
                joints.push_back(track.joint);
            }
            content["joints"] = std::move(joints);
            break;
        }
        case AssetType::Scene:
            // Opened with scene.open; nothing to say about it without doing that.
            break;
    }
    return content;
}

} // namespace

Status registerAssetCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "asset.list",
        .description = "Lists the assets the scene can use, each with its id, type, name and path, and what is "
                       "wrong with the asset directory, if anything. Built-in assets have the path \"builtin\".",
        .parameters = {{"type", Type::String,
                        "Only assets of this type: texture, mesh, material, model, scene, skeleton or animation.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            std::optional<AssetType> type;
            if (const auto name = jsonGetOr<std::string>(parameters, "type", ""); !name.empty()) {
                type = assetTypeFromName(name);
                if (!type) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not an asset type; use texture, mesh, material, model or scene", name);
                }
            }

            const AssetDatabase& database = context.assets().database();
            Json assets = Json::array();
            for (const AssetInfo& asset : database.assets()) {
                if (!type || asset.type == *type) {
                    assets.push_back(describeAsset(asset));
                }
            }
            Json result = Json::object();
            result["directory"] =
                database.rootDirectory().empty() ? Json(nullptr) : Json(pathToUtf8(database.rootDirectory()));
            result["assets"] = std::move(assets);
            result["problems"] = describeProblems(database);
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.get",
        .description = "Describes one asset: the assets it depends on, what its importer left out, and what is in "
                       "it - the size of a mesh or a texture, the properties of a material, the nodes of a model. "
                       "Fails with the reason when the asset cannot be loaded.",
        .parameters = {{"asset", Type::String, kAssetReference}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset, context.resolveAsset(parameters.at("asset").get<std::string>()));
            const AssetDatabase& database = context.assets().database();

            Json result = describeAsset(*asset);
            Json dependencies = Json::array();
            for (const Uuid& dependency : asset->dependencies) {
                if (const AssetInfo* needed = database.find(dependency)) {
                    dependencies.push_back(describeAsset(*needed));
                } else {
                    // Still listed, so that a missing dependency is seen and not dropped.
                    Json missing = Json::object();
                    missing["id"] = dependency.toString();
                    missing["missing"] = true;
                    dependencies.push_back(std::move(missing));
                }
            }
            result["dependencies"] = std::move(dependencies);
            result["warnings"] = asset->warnings;
            CORDITE_TRY_ASSIGN(result["content"], describeContent(context.assets(), *asset));
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.refresh",
        .description = "Reads the asset directory again and reloads everything, after files were added, changed "
                       "or removed. Returns how many assets there are and what is wrong, if anything.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(context.assets().refresh());
            const AssetDatabase& database = context.assets().database();
            Json result = Json::object();
            result["assetCount"] = database.assets().size();
            result["problems"] = describeProblems(database);
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.instantiate",
        .description = "Places a model in the scene: one entity per node of the model, with their meshes and "
                       "materials, under a new root entity that is returned and selected. Move that root to "
                       "place the model.",
        .parameters = {{"asset", Type::String, kAssetReference},
                       {"parent", Type::String,
                        "The entity to put the model under: its id, path or unique name. Without one it becomes a "
                        "root.",
                        false},
                       {"name", Type::String, "The name of the new root entity. Defaults to the name of the model.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset, context.resolveAsset(parameters.at("asset").get<std::string>()));
            ModelInstanceOptions options;
            options.name = jsonGetOr<std::string>(parameters, "name", "");
            if (options.name.find('/') != std::string::npos) {
                return makeError(ErrorCode::InvalidArgument, "an entity name must not contain '/', got '{}'",
                                 options.name);
            }
            if (const auto parent = jsonGetOr<std::string>(parameters, "parent", ""); !parent.empty()) {
                CORDITE_TRY_ASSIGN(options.parent, context.resolveEntity(parent));
            }

            EditorActions actions(context);
            CORDITE_TRY_ASSIGN(const Entity instance, actions.instantiateModel(asset->id, options));

            const World& world = context.world();
            Json result = Json::object();
            result["id"] = world.id(instance).toString();
            result["name"] = world.name(instance);
            result["path"] = context.entityPath(instance);
            uint32_t entityCount = 0;
            world.traverseSubtree(instance, [&entityCount](Entity /*entity*/, uint32_t /*depth*/) { ++entityCount; });
            result["entityCount"] = entityCount;
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "scene.validate",
        .description = "Checks every reference from the scene to an asset: the asset must exist and be of the "
                       "kind the property expects. Returns the problems found, and the assets the scene uses.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            const World& world = context.world();
            const AssetDatabase& database = context.assets().database();

            Json problems = Json::array();
            for (const SceneProblem& problem : findSceneProblems(world, context.components(), database)) {
                Json entry = Json::object();
                entry["entity"] = context.entityPath(problem.entity);
                entry["id"] = world.id(problem.entity).toString();
                entry["problem"] = problem.message;
                problems.push_back(std::move(entry));
            }

            // Each asset once, in the order the scene first uses it.
            Json used = Json::array();
            std::vector<Uuid> seen;
            for (const AssetReferenceUse& use : collectAssetReferences(world, context.components())) {
                if (std::ranges::find(seen, use.asset) != seen.end()) {
                    continue;
                }
                seen.push_back(use.asset);
                if (const AssetInfo* asset = database.find(use.asset)) {
                    used.push_back(describeAsset(*asset));
                }
            }

            Json result = Json::object();
            result["valid"] = problems.empty();
            result["problems"] = std::move(problems);
            result["assets"] = std::move(used);
            return result;
        },
    }));
    return {};
}

} // namespace cordite
