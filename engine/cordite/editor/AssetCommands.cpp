#include "cordite/editor/AssetCommands.h"

#include "cordite/assets/Animation.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Prefab.h"
#include "cordite/assets/ScriptSource.h"
#include "cordite/assets/Sequence.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/AssetFiles.h"
#include "cordite/editor/AssetImport.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/engine/SceneAssets.h"
#include "cordite/math/MathJson.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Components.h"
#include "cordite/world/Entity.h"
#include "cordite/world/EntityPath.h"
#include "cordite/world/Prefab.h"
#include "cordite/world/World.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

using Type = CommandParameterType;

/// What `assets.browse` calls the assets of every type.
constexpr const char* kEveryType = "all";

/// The most files `asset.import` brings in at once: more than a hand drops.
constexpr size_t kMostImportedFiles = 1024;

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

/// The assets that need `asset`, described as `dependencies` are, as their files were last
/// read or saved.
Json describeDependents(const AssetDatabase& database, const Uuid& asset) {
    Json users = Json::array();
    for (const Uuid& user : database.dependents(asset)) {
        if (const AssetInfo* found = database.find(user)) {
            users.push_back(describeAsset(*found));
        }
    }
    return users;
}

/// Where the world refers to `asset`: every entity, component and property, in the order
/// of the hierarchy.
Json describeUsesInScene(const EditorContext& context, const Uuid& asset) {
    Json uses = Json::array();
    for (const AssetReferenceUse& use : collectAssetReferences(context.world(), context.components())) {
        if (use.asset != asset) {
            continue;
        }
        Json entry = Json::object();
        entry["entity"] = context.entityPath(use.entity);
        entry["id"] = context.world().id(use.entity).toString();
        entry["component"] = use.component;
        entry["property"] = use.property;
        uses.push_back(std::move(entry));
    }
    return uses;
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
            entry["entity"] = entityPath(scratch, problem.entity);
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
        case AssetType::RenderTarget: {
            CORDITE_TRY_ASSIGN(const RenderTargetData target, assets.loadRenderTarget(asset.id));
            content["width"] = target.width;
            content["height"] = target.height;
            break;
        }
        case AssetType::Sequence: {
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const SequenceData> sequence, assets.loadSequence(asset.id));
            content["duration"] = jsonFromFloat(sequence->duration);
            content["transformTracks"] = sequence->transforms.size();
            content["propertyTracks"] = sequence->properties.size();
            content["cuts"] = sequence->cuts.size();
            Json events = Json::array();
            for (const SequenceEventKey& event : sequence->events) {
                events.push_back(Json{{"time", jsonFromFloat(event.time)}, {"name", event.name}});
            }
            content["events"] = std::move(events);
            break;
        }
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

/// Rows of the browser, as `assets.browse` returns them.
Json describeRows(const std::vector<AssetBrowserRow>& rows) {
    Json described = Json::array();
    for (const AssetBrowserRow& row : rows) {
        const bool isAsset = row.kind == AssetBrowserRow::Kind::Asset;
        Json entry = Json::object();
        entry["kind"] = isAsset ? "asset" : "directory";
        entry["depth"] = row.depth;
        entry["name"] = row.label;
        entry["path"] = row.path;
        if (isAsset) {
            entry["id"] = row.asset.toString();
            entry["type"] = std::string(nameOf(row.type));
            entry["matches"] = row.matches;
        }
        if (row.hasChildren) {
            entry["open"] = row.open;
        }
        if (row.selected) {
            entry["selected"] = true;
        }
        described.push_back(std::move(entry));
    }
    return described;
}

/// What the browser shows, as `assets.browse` returns it.
Json describeBrowser(const AssetBrowser& browser, const AssetDatabase& database) {
    Json result = Json::object();
    const AssetBrowserFilter& filter = browser.filter();
    result["type"] = filter.type ? Json(std::string(nameOf(*filter.type))) : Json(nullptr);
    result["search"] = filter.search;
    const AssetInfo* selected = database.find(browser.selection());
    result["selected"] = selected != nullptr ? describeAsset(*selected) : Json(nullptr);
    result["view"] = std::string(nameOf(browser.view()));
    // By its path, as the parameter takes it: the last step of the way to it.
    result["folder"] = folderSteps(browser.shownFolder(database)).back().path;
    result["folders"] = describeRows(browser.folders(database));
    result["contents"] = describeRows(browser.contents(database));
    result["rows"] = describeRows(browser.rows(database));
    return result;
}

/// Does what `assets.browse` was asked to, to a browser of its own: the one of the panel
/// is changed only when all of it could be done.
Status applyBrowse(AssetBrowser& browser, const EditorContext& context, const Json& parameters) {
    const AssetDatabase& database = context.assets().database();
    AssetBrowserFilter filter = browser.filter();
    if (parameters.contains("type")) {
        const std::string name = parameters.at("type").get<std::string>();
        filter.type = assetTypeFromName(name);
        if (!filter.type && name != kEveryType) {
            return makeError(ErrorCode::InvalidArgument, "'{}' is not an asset type; use \"{}\", {}", name, kEveryType,
                             assetTypeNames());
        }
    }
    if (parameters.contains("search")) {
        filter.search = parameters.at("search").get<std::string>();
    }
    browser.setFilter(std::move(filter));

    if (parameters.contains("select")) {
        const std::string reference = parameters.at("select").get<std::string>();
        if (trim(reference).empty()) {
            browser.select(Uuid());
        } else {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset, context.resolveAsset(reference));
            browser.select(asset->id);
            // Shown, where the filter lists it: its folder, and what it is in opened.
            browser.reveal(database, asset->id);
        }
    }
    // After the choice, which shows a folder of its own: the folder asked for is the one
    // shown.
    if (parameters.contains("folder")) {
        const std::string path = parameters.at("folder").get<std::string>();
        const std::optional<std::string> folder = assetFolderAt(database, trim(path));
        if (!folder) {
            return makeError(ErrorCode::NotFound,
                             "the assets have no folder '{}': a folder is a directory with an asset in it or below "
                             "it, \"\" the top and \"{}\" what the engine has built in",
                             path, kBuiltinAssetPath);
        }
        browser.showFolder(*folder);
    }
    if (parameters.contains("view")) {
        const std::string name = parameters.at("view").get<std::string>();
        const std::optional<AssetBrowserView> view = assetBrowserViewFromName(name);
        if (!view) {
            return makeError(ErrorCode::InvalidArgument, "'{}' is not a view of the assets; use {} or {}", name,
                             nameOf(AssetBrowserView::List), nameOf(AssetBrowserView::Grid));
        }
        browser.setView(*view);
    }

    for (const bool open : {true, false}) {
        const char* parameter = open ? "open" : "close";
        if (!parameters.contains(parameter)) {
            continue;
        }
        const std::string path = parameters.at(parameter).get<std::string>();
        const std::optional<AssetBrowserRow> row = browser.find(database, path);
        if (!row || !row->hasChildren) {
            return makeError(ErrorCode::NotFound,
                             "the assets that are listed have no directory and no file '{}' with something in it",
                             path);
        }
        browser.setOpen(*row, open);
    }
    return {};
}

} // namespace

Status registerAssetBrowserCommands(CommandRegistry& commands, EditorContext& context, AssetBrowser& browser) {
    return commands.add({
        .name = "assets.browse",
        .description =
            "Says what the Assets panel shows, and sets what it shows: a tree of the folders of the project "
            "(\"folders\", the one shown \"selected\"), and beside it what the folder shown has in it "
            "(\"contents\") - its directories, then its files, each followed by what is inside it once it is "
            "opened - as a list or as a grid of tiles. A type, a text or both narrow the contents down to "
            "every asset asked for in the folder and below it, side by side. It can show another folder, "
            "choose an asset - which shows the folder it is in - open or close a folder of the tree or a file, "
            "and switch between the list and the grid. Returns those, and all the assets as one tree "
            "(\"rows\"), in which every row is open while something is left out. Every row has how far in it "
            "is (\"depth\"); what is below a row that is closed is not among them. Without parameters it "
            "changes nothing.",
        .parameters =
            {{"type", Type::String,
              fmt::format("Only assets of this type: {} - or \"{}\" for those of every type.", assetTypeNames(),
                          kEveryType),
              false},
             {"search", Type::String,
              R"(Only assets with this text in their name or in their path, whatever the case. "" for all of them.)",
              false},
             {"select", Type::String,
              R"(The asset to choose: its id, or its path as a row has it. "" for none. The folder it is in is )"
              "shown, and the file it is inside opened, so that it is among the contents; an asset the filter "
              "leaves out is chosen all the same, and shows once it is listed.",
              false},
             {"folder", Type::String,
              R"(The folder to show, by its path as the tree has it: "" for the top of the asset directory, )"
              R"("models/props" for a directory, "builtin" for what the engine has built in - a directory of )"
              "the project with that name is found first. It is shown after the asset to choose.",
              false},
             {"view", Type::String, "How the contents are shown: list, or grid.", false},
             {"open", Type::String,
              R"(The path of a folder or of a file whose rows are to be shown: "models", "models/crate.gltf".)", false},
             {"close", Type::String, "The path of a folder or of a file whose rows are to be hidden.", false}},
        .handler = [&context, &browser](const Json& parameters) -> Result<Json> {
            AssetBrowser changed = browser;
            CORDITE_TRY(applyBrowse(changed, context, parameters));
            browser = std::move(changed);
            return describeBrowser(browser, context.assets().database());
        },
    });
}

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
        .description = "Describes one asset: the assets it depends on (`dependencies`) and those that depend on "
                       "it (`usedBy`: a material that has a texture, a scene or a prefab whose entities refer "
                       "to it - as their files were last read or saved), where the open scene refers to it "
                       "(`usedInScene`: entity, component and property), what its importer left out, and "
                       "what is in it - the size of a mesh or a texture, the properties of a material, the "
                       "nodes of a model, the text of a script, the length of a sound, the entities of a "
                       "prefab with the assets they use and what is wrong with them (\"problems\"). "
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
            result["usedBy"] = describeDependents(database, asset->id);
            result["usedInScene"] = describeUsesInScene(context, asset->id);
            result["warnings"] = asset->warnings;
            CORDITE_TRY_ASSIGN(result["content"], describeContent(context.assets(), context.components(), *asset));
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.import",
        .description = "Brings files from elsewhere into the assets: copies them into a directory of the asset "
                       "directory under their own names - a glTF file with the buffers and images it names by "
                       "where they are, to the same places beside it - and reads the assets again. They get ids "
                       "of their own; a .meta beside them stays behind. Refused, copying nothing, for a file "
                       "that is not there, is a directory, is among the assets already or is no kind of asset, "
                       "for two files that would go to one place, for a place that holds a file, and for a glTF "
                       "file whose files would land outside the assets. Returns where each file went, and the "
                       "assets they are. Files are no part of the document: editor.undo does not take them away.",
        .parameters = {{"files", Type::Array, "The paths of the files: absolute, or from where the editor runs."},
                       {"to", Type::String,
                        "The directory of the assets they go into, with forward slashes: \"props/crates\". The "
                        "top when left out.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            const Json& listed = parameters.at("files");
            if (listed.size() > kMostImportedFiles) {
                return makeError(ErrorCode::InvalidArgument, "{} files are brought in at once at most, not {}",
                                 kMostImportedFiles, listed.size());
            }
            std::vector<std::filesystem::path> files;
            for (const Json& file : listed) {
                if (!file.is_string()) {
                    return makeError(ErrorCode::InvalidArgument, "'files' is a list of paths, and {} is none",
                                     file.dump());
                }
                files.push_back(pathFromUtf8(file.get_ref<const std::string&>()));
            }
            CORDITE_TRY_ASSIGN(const AssetImport imported,
                               importAssets(context, files, jsonGetOr<std::string>(parameters, "to", "")));
            const AssetDatabase& database = context.assets().database();
            Json brought = Json::array();
            for (const std::string& file : imported.files) {
                // A buffer beside a glTF file is no asset: it is said where it went, and no more.
                const AssetInfo* asset = database.findByReference(file);
                Json entry = asset != nullptr ? describeAsset(*asset) : Json::object();
                entry["path"] = file;
                brought.push_back(std::move(entry));
            }
            Json result = Json::object();
            result["files"] = std::move(brought);
            result["problems"] = describeProblems(database);
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.createDirectory",
        .description = "Makes a directory of the assets, and the directories it is in, and reads the assets again: "
                       "the Assets panel shows it, empty. Refused for a place outside the asset directory, with a "
                       "part that is empty or begins with a dot, and for one where there is a file or a directory. "
                       "Returns the directory. Not undoable: directories are no part of the document.",
        .parameters = {{"path", Type::String,
                        "Where the directory goes, inside the asset directory, with forward slashes: "
                        "\"materials/metal\"."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const std::string made,
                               createAssetDirectory(context, parameters.at("path").get_ref<const std::string&>()));
            Json result = Json::object();
            result["path"] = made;
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.createMaterial",
        .description = "Writes a material with every property at its default - white, matte, no textures - "
                       "into the assets, making the directories it is in, and reads the assets again; set its "
                       "properties in its file, or place it on a mesh with component.set. Refused for a place "
                       "outside the asset directory, for a name that does not end in .material.json, and where "
                       "there is a file. Returns the material. Not undoable: files are no part of the document.",
        .parameters = {{"path", Type::String,
                        "Where the material goes, inside the asset directory, with forward slashes: "
                        "\"materials/rust.material.json\"."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Uuid made,
                               createMaterial(context, parameters.at("path").get_ref<const std::string&>()));
            const AssetInfo* material = context.assets().database().find(made);
            if (material == nullptr) {
                return makeError(ErrorCode::InvalidState, "the new material {} is not among the assets",
                                 made.toString());
            }
            return describeAsset(*material);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.refresh",
        .description = "Reads the asset directory again and reloads everything, after files were added, changed "
                       "or removed, and brings the scene's linked prefab instances up to date with their "
                       "prefabs - one step of the history, while the scene is not played. Returns how many "
                       "assets there are, how many instances changed and what is wrong, if anything.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(context.assets().refresh());
            size_t updated = 0;
            if (!context.isPlaying()) {
                EditorActions actions(context);
                CORDITE_TRY_ASSIGN(updated, actions.updatePrefabInstances());
            }
            const AssetDatabase& database = context.assets().database();
            Json result = Json::object();
            result["assetCount"] = database.assets().size();
            result["prefabInstancesUpdated"] = updated;
            result["problems"] = describeProblems(database);
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "asset.instantiate",
        .description = "Places a model, a prefab or a mesh in the scene. A model becomes one entity per node, with "
                       "their meshes and materials, under a new root entity. A prefab becomes copies of its "
                       "entities, with ids of their own, the root of them marked as an instance of it by a "
                       "PrefabInstance component. A mesh becomes one entity whose MeshRenderer draws it - with the "
                       "materials its model draws it with, for a mesh of a model. The root is returned and "
                       "selected; move it to place what was placed.",
        .parameters = {{"asset", Type::String, kAssetReference},
                       {"parent", Type::String,
                        "The entity to put the new entities under: its id, path or unique name. Without one "
                        "their root becomes a root of the scene.",
                        false},
                       {"name", Type::String,
                        "The name of the new root entity. Defaults to the name of the model or of the mesh, or "
                        "to the name the prefab gives its root.",
                        false},
                       {"position", Type::Array,
                        "Where the root is put: [x, y, z] in metres, relative to the parent - in the world, "
                        "without one. Defaults to the parent's origin for a model and a mesh, and to where the "
                        "prefab has its root.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset, context.resolveAsset(parameters.at("asset").get<std::string>()));
            PlacementOptions options{.name = jsonGetOr<std::string>(parameters, "name", "")};
            if (const auto parent = jsonGetOr<std::string>(parameters, "parent", ""); !parent.empty()) {
                CORDITE_TRY_ASSIGN(options.parent, context.resolveEntity(parent));
            }
            if (parameters.contains("position")) {
                Vec3 position(0.0f);
                if (!fromJson(parameters.at("position"), position)) {
                    return makeError(ErrorCode::InvalidArgument, "'position' must be an array of 3 finite numbers");
                }
                options.position = position;
            }

            EditorActions actions(context);
            CORDITE_TRY_ASSIGN(const Entity instance, actions.instantiateAsset(asset->id, options));

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
                       "assets, reads the assets again, and makes the entity an instance of the new prefab: a "
                       "PrefabInstance component marks it, and a PrefabLink links it and every entity below it "
                       "to itself in the file. Returns the prefab. asset.instantiate places it, and so does a "
                       "script, with cordite.world.instantiate. What the editor places stays linked: when the "
                       "prefab changes, its instances take the change, but for what they keep of their own. "
                       "Undoing takes the mark and the links off; the file stays.",
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
        .name = "prefab.revert",
        .description = "Gives a linked copy of a prefab's entity back what its prefab has, where it kept "
                       "something of its own: one override, or all of them - of every entity of the instance, "
                       "for its root. The overrides of an entity are the overrides of its PrefabLink component. "
                       "The Transform of the root of an instance stays its own. Undoable. Returns the entity.",
        .parameters = {{"entity", Type::String,
                        "A linked copy - an entity with a PrefabLink: its id, its path of names from a root, or "
                        "its name if that is unique."},
                       {"override", Type::String,
                        "The one override to take back, as its PrefabLink lists it: \"Light.intensity\", "
                        "\"+Light\" (a component it added), \"-Light\" (one it left out), \"name\" or "
                        "\"enabled\". Without one, every override is taken back.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, context.resolveEntity(parameters.at("entity").get<std::string>()));
            const auto entry = jsonGetOr<std::string>(parameters, "override", "");
            // Found again by its id: what its prefab no longer has is taken away.
            const std::string id = context.world().id(entity).toString();
            EditorActions actions(context);
            CORDITE_TRY(actions.revertPrefabOverrides(entity, entry));
            CORDITE_TRY_ASSIGN(const Entity reverted, context.resolveEntity(id));
            Json result = Json::object();
            result["id"] = context.world().id(reverted).toString();
            result["path"] = context.entityPath(reverted);
            if (const auto* link = context.world().tryGet<PrefabLinkComponent>(reverted)) {
                result["overrides"] = link->overrides;
            }
            return result;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "prefab.apply",
        .description = "Writes what a linked copy of a prefab's entity keeps of its own into the prefab, so that "
                       "it is the prefab's: one override, or all of them - of every entity of the instance, and "
                       "the entities added to it, for its root. Every linked instance of the prefab in the scene "
                       "then takes what it is now. The root's Transform and name stay the prefab's. The file is "
                       "written, which undo does not take back; what changed in the scene is one step. Returns "
                       "the prefab.",
        .parameters = {{"entity", Type::String,
                        "A linked copy - an entity with a PrefabLink: its id, its path of names from a root, or "
                        "its name if that is unique."},
                       {"override", Type::String,
                        "The one override to apply, as its PrefabLink lists it: \"Light.intensity\", \"+Light\", "
                        "\"-Light\", \"name\" or \"enabled\". Without one, every override is applied.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, context.resolveEntity(parameters.at("entity").get<std::string>()));
            const auto entry = jsonGetOr<std::string>(parameters, "override", "");
            EditorActions actions(context);
            CORDITE_TRY_ASSIGN(const Uuid id, actions.applyPrefabOverrides(entity, entry));
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
