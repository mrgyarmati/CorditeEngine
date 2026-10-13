#include "cordite/editor/AssetCommands.h"

#include "cordite/assets/Animation.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Prefab.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/math/MathJson.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/Entity.h"
#include "cordite/world/Prefab.h"
#include "cordite/world/World.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
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

/// The assets the components of a world refer to: each once, in the order in which the
/// world first uses it. What is referred to and is not there is left out here;
/// findSceneProblems() is what names it.
Json describeUsedAssets(const World& world, const ComponentRegistry& components, const AssetDatabase& database) {
    Json used = Json::array();
    std::vector<Uuid> seen;
    for (const AssetReferenceUse& use : collectAssetReferences(world, components)) {
        if (std::ranges::find(seen, use.asset) != seen.end()) {
            continue;
        }
        seen.push_back(use.asset);
        if (const AssetInfo* asset = database.find(use.asset)) {
            used.push_back(describeAsset(*asset));
        }
    }
    return used;
}

/// What is in a prefab, and what placing it anywhere would say: it is placed in a world
/// of its own for that - whether its entities are right, which assets they use, and
/// what they refer to that is not there or is of the wrong kind.
Result<Json> describePrefab(AssetManager& assets, const ComponentRegistry& components, const AssetInfo& asset) {
    CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> prefab, assets.loadPrefab(asset.id));
    Json content = Json::object();
    content["name"] = prefab->name;
    content["entityCount"] = prefab->entities.size();
    Json names = Json::array();
    for (const Json& entity : prefab->entities) {
        names.push_back(jsonGetOr<std::string>(entity, "name", ""));
    }
    content["entities"] = std::move(names);

    World scratch;
    Json problems = Json::array();
    const Result<Entity> placed = instantiatePrefab(scratch, components, prefab->entities, asset.id);
    if (placed) {
        // Without the mark of an instance: it names the prefab itself, which is no use of it.
        scratch.remove<PrefabInstanceComponent>(*placed);
        for (const SceneProblem& problem : findSceneProblems(scratch, components, assets.database())) {
            Json entry = Json::object();
            entry["entity"] = entityPathIn(scratch, problem.entity);
            entry["problem"] = problem.message;
            problems.push_back(std::move(entry));
        }
    } else {
        Json entry = Json::object();
        entry["problem"] = placed.error().message;
        problems.push_back(std::move(entry));
    }
    content["assets"] = describeUsedAssets(scratch, components, assets.database());
    content["problems"] = std::move(problems);
    return content;
}

Json describeProblems(const AssetDatabase& database) {
    Json problems = Json::array();
    for (const std::string& problem : database.problems()) {
        problems.push_back(problem);
    }
    return problems;
}

/// What is inside an asset. Loads it, so a broken asset fails here with the reason.
Result<Json> describeContent(AssetManager& assets, const ComponentRegistry& components, const AssetInfo& asset) {
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
                entry["joint"] = node.joint;
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
        case AssetType::Script: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const ScriptSource> script, assets.loadScript(asset.id));
            content["bytes"] = script->text.size();
            content["lines"] =
                std::ranges::count(script->text, '\n') + (script->text.empty() || script->text.back() == '\n' ? 0 : 1);
            content["text"] = script->text;
            break;
        }
        case AssetType::Sound: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const SoundData> sound, assets.loadSound(asset.id));
            content["sampleRate"] = sound->sampleRate;
            content["channels"] = sound->channels;
            content["frames"] = sound->frames();
            content["seconds"] = sound->seconds();
            break;
        }
        case AssetType::Prefab:
            return describePrefab(assets, components, asset);
        case AssetType::Scene:
            // Opened with scene.open; nothing to say about it without doing that.
            break;
    }
    return content;
}

/// The names of the asset types the way a sentence lists them: "texture, mesh, ... or
/// sound". From the list of types, so that a new one is named without being added here.
std::string assetTypeNames() {
    std::string names;
    for (size_t index = 0; index < kAssetTypes.size(); ++index) {
        if (index > 0) {
            names += index + 1 == kAssetTypes.size() ? " or " : ", ";
        }
        names += nameOf(kAssetTypes[index]);
    }
    return names;
}

} // namespace

Status registerAssetCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "asset.list",
        .description = "Lists the assets the scene can use, each with its id, type, name and path, and what is "
                       "wrong with the asset directory, if anything. Built-in assets have the path \"builtin\".",
        .parameters = {{"type", Type::String, fmt::format("Only assets of this type: {}.", assetTypeNames()), false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            std::optional<AssetType> type;
            if (const auto name = jsonGetOr<std::string>(parameters, "type", ""); !name.empty()) {
                type = assetTypeFromName(name);
                if (!type) {
                    return makeError(ErrorCode::InvalidArgument, "'{}' is not an asset type; use {}", name,
                                     assetTypeNames());
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
                       "it - the size of a mesh or a texture, the properties of a material, the nodes of a model, "
                       "the text of a script, the length of a sound, the entities of a prefab with the assets "
                       "they use and what is wrong with them (\"problems\"). "
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
            CORDITE_TRY_ASSIGN(result["content"], describeContent(context.assets(), context.components(), *asset));
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
        .description = "Places a model or a prefab in the scene. A model becomes one entity per node, with "
                       "their meshes and materials, under a new root entity. A prefab becomes copies of its "
                       "entities, with ids of their own, the root of them marked as an instance of it by a "
                       "PrefabInstance component. The root is returned and selected; move it to place what was "
                       "placed.",
        .parameters = {{"asset", Type::String, kAssetReference},
                       {"parent", Type::String,
                        "The entity to put the new entities under: its id, path or unique name. Without one "
                        "their root becomes a root of the scene.",
                        false},
                       {"name", Type::String,
                        "The name of the new root entity. Defaults to the name of the model, or to the name the "
                        "prefab gives its root.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset, context.resolveAsset(parameters.at("asset").get<std::string>()));
            const auto name = jsonGetOr<std::string>(parameters, "name", "");
            if (name.find('/') != std::string::npos) {
                return makeError(ErrorCode::InvalidArgument, "an entity name must not contain '/', got '{}'", name);
            }
            Entity under;
            if (const auto parent = jsonGetOr<std::string>(parameters, "parent", ""); !parent.empty()) {
                CORDITE_TRY_ASSIGN(under, context.resolveEntity(parent));
            }

            EditorActions actions(context);
            Entity instance;
            if (asset->type == AssetType::Model) {
                CORDITE_TRY_ASSIGN(instance, actions.instantiateModel(asset->id, {.parent = under, .name = name}));
            } else if (asset->type == AssetType::Prefab) {
                CORDITE_TRY_ASSIGN(instance, actions.instantiatePrefab(asset->id, {.parent = under, .name = name}));
            } else {
                return makeError(ErrorCode::InvalidArgument,
                                 "{} is {}; what is placed in a scene is a model or a prefab", asset->reference(),
                                 withArticle(asset->type));
            }

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
        .name = "prefab.create",
        .description = "Makes a prefab of an entity and everything below it: writes them into a file among the "
                       "assets, reads the assets again, and marks the entity as an instance of the new prefab "
                       "with a PrefabInstance component. Returns the prefab. asset.instantiate places it, and "
                       "so does a script, with cordite.world.instantiate. What was placed is a copy: changing "
                       "the prefab later does not change it. Undoing takes the mark off the entity; the file "
                       "stays.",
        .parameters = {{"entity", Type::String,
                        "The entity to make the prefab of: its id, its path of names from a root, or its name if "
                        "that is unique."},
                       {"path", Type::String,
                        "Where the file goes, within the asset directory. Its name ends in \".prefab.json\": "
                        "\"prefabs/cart.prefab.json\". Without one it is named after the entity, in \"prefabs\".",
                        false},
                       {"overwrite", Type::Boolean,
                        "Whether a file that is there already is replaced. Without it, such a file is an error. "
                        "A prefab that is replaced keeps its id.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, context.resolveEntity(parameters.at("entity").get<std::string>()));
            const auto path = jsonGetOr<std::string>(parameters, "path", "");
            const bool overwrite = jsonGetOr<bool>(parameters, "overwrite", false);

            EditorActions actions(context);
            CORDITE_TRY_ASSIGN(const Uuid id, actions.createPrefab(entity, path, overwrite));
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const PrefabData> prefab, context.assets().loadPrefab(id));
            const AssetInfo* asset = context.assets().database().find(id);
            if (asset == nullptr) {
                return makeError(ErrorCode::InvalidState, "the prefab {} is not among the assets", id.toString());
            }
            Json result = describeAsset(*asset);
            result["entityCount"] = prefab->entities.size();
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "scene.validate",
        .description = "Checks every reference from the scene to an asset - the asset must exist and be of the "
                       "kind the property expects - and every collider for what no shape or body can be made "
                       "of: a shape of a mesh that names none, the triangles of a mesh in a dynamic body. "
                       "Returns the problems found, and the assets the scene uses.",
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

            Json result = Json::object();
            result["valid"] = problems.empty();
            result["problems"] = std::move(problems);
            // Each asset once, in the order the scene first uses it.
            result["assets"] = describeUsedAssets(world, context.components(), database);
            return result;
        },
    }));
    return {};
}

} // namespace cordite
