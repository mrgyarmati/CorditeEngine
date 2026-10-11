#include "cordite/editor/EditorCommands.h"

#include "cordite/core/FileSystem.h"
#include "cordite/editor/AssetCommands.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/world/SceneSerializer.h"

#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

using Type = CommandParameterType;

constexpr const char* kEntityReference =
    "An entity: its id, its path of names from a root (\"Room/Table/Lamp\"), or its name if that is unique.";

/// The short form in which results refer to an entity.
Json describeEntity(const EditorContext& context, Entity entity) {
    const World& world = context.world();
    Json description = Json::object();
    description["id"] = world.id(entity).toString();
    description["name"] = world.name(entity);
    description["path"] = context.entityPath(entity);
    return description;
}

Json describeEntities(const EditorContext& context, std::span<const Entity> entities) {
    Json list = Json::array();
    for (const Entity entity : entities) {
        list.push_back(describeEntity(context, entity));
    }
    return list;
}

/// The names of the components an entity has, in registration order.
Json componentNames(const EditorContext& context, Entity entity) {
    Json names = Json::array();
    for (const ComponentDescriptor* descriptor : context.components().components()) {
        if (descriptor->has(context.world().registry(), entity.handle)) {
            names.push_back(descriptor->type.name());
        }
    }
    return names;
}

/// An entity and everything below it, as nested objects.
Json describeSubtree(const EditorContext& context, Entity entity) {
    const World& world = context.world();
    Json node = describeEntity(context, entity);
    node["enabled"] = world.isEnabled(entity);
    node["components"] = componentNames(context, entity);
    Json children = Json::array();
    for (const Entity child : world.children(entity)) {
        children.push_back(describeSubtree(context, child));
    }
    node["children"] = std::move(children);
    return node;
}

Result<Entity> entityParameter(const EditorContext& context, const Json& parameters, const char* name) {
    return context.resolveEntity(parameters.at(name).get<std::string>());
}

/// Resolves an array of entity references. Every element must be a string.
Result<std::vector<Entity>> entityListParameter(const EditorContext& context, const Json& parameters,
                                                const char* name) {
    std::vector<Entity> entities;
    for (const Json& reference : parameters.at(name)) {
        if (!reference.is_string()) {
            return makeError(ErrorCode::InvalidArgument, "'{}' must be an array of entity references, found {} in it",
                             name, jsonTypeName(reference));
        }
        CORDITE_TRY_ASSIGN(const Entity entity, context.resolveEntity(reference.get<std::string>()));
        entities.push_back(entity);
    }
    return entities;
}

Result<const ComponentDescriptor*> componentParameter(const EditorContext& context, const Json& parameters) {
    const std::string name = parameters.at("component").get<std::string>();
    const ComponentDescriptor* descriptor = context.components().find(name);
    if (descriptor == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no component type named '{}'", name);
    }
    return descriptor;
}

Json describeHistory(const EditorContext& context) {
    const UndoStack& history = context.history();
    Json description = Json::object();
    description["canUndo"] = history.canUndo();
    description["undoLabel"] = history.undoLabel();
    description["canRedo"] = history.canRedo();
    description["redoLabel"] = history.redoLabel();
    return description;
}

Json describeScene(const EditorContext& context) {
    Json description = Json::object();
    description["name"] = context.world().sceneName();
    description["path"] = context.scenePath().empty() ? Json(nullptr) : Json(pathToUtf8(context.scenePath()));
    description["unsavedChanges"] = context.hasUnsavedChanges();
    description["entityCount"] = context.world().entityCount();
    return description;
}

Status registerSceneCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "scene.new",
        .description = "Replaces the scene with an empty one. Unsaved changes are discarded.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            context.newScene();
            return describeScene(context);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "scene.open",
        .description = "Loads a scene file, replacing the current scene. Nothing changes if the file is invalid.",
        .parameters = {{"path", Type::String,
                        "The scene to load: a scene asset as asset.list names it (\"scenes/main.scene.json\"), or "
                        "the path of a file."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            const std::string path = parameters.at("path").get<std::string>();
            // A scene of the project is found wherever the editor was started from.
            const AssetDatabase& database = context.assets().database();
            const AssetInfo* asset = database.findByReference(path);
            const bool isSceneAsset = asset != nullptr && asset->type == AssetType::Scene;
            CORDITE_TRY(context.openScene(isSceneAsset ? database.sourceFile(*asset) : pathFromUtf8(path)));
            return describeScene(context);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "scene.save",
        .description = "Saves the scene to a file, or to the file it already has when no path is given.",
        .parameters = {{"path", Type::String, "Where to save. Becomes the scene's file.", false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            const auto path = jsonGetOr<std::string>(parameters, "path", "");
            CORDITE_TRY(context.saveScene(pathFromUtf8(path)));
            return describeScene(context);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "scene.describe",
        .description = "Returns the hierarchy: every entity with its id, path, state, component names and children.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            Json description = describeScene(context);
            Json roots = Json::array();
            for (const Entity root : context.world().roots()) {
                roots.push_back(describeSubtree(context, root));
            }
            description["entities"] = std::move(roots);
            return description;
        },
    }));

    return commands.add({
        .name = "scene.get",
        .description = "Returns the whole scene as the JSON document that scene.save would write.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            return serializeScene(context.world(), context.components());
        },
    });
}

Status registerEntityCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "entity.create",
        .description = "Creates an entity and returns its id and path.",
        .parameters = {{"name", Type::String, "Its name. Defaults to \"Entity\".", false},
                       {"parent", Type::String, "The entity to create it under. Without one it becomes a root.", false},
                       {"components", Type::Object,
                        "Components to give it, by name, each with the properties to set, e.g. "
                        "{\"Transform\": {\"position\": [0, 1, 0]}, \"Camera\": {}}.",
                        false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CreateEntityOptions options;
            options.name = jsonGetOr<std::string>(parameters, "name", "Entity");
            if (const auto parent = parameters.find("parent"); parent != parameters.end() && !parent->is_null()) {
                CORDITE_TRY_ASSIGN(options.parent, entityParameter(context, parameters, "parent"));
            }
            if (const auto components = parameters.find("components");
                components != parameters.end() && !components->is_null()) {
                options.components = *components;
            }
            CORDITE_TRY_ASSIGN(const Entity entity, EditorActions(context).createEntity(options));
            return describeEntity(context, entity);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "entity.delete",
        .description = "Deletes entities and everything below them.",
        .parameters = {{"entities", Type::Array, "The entities to delete, each given as an id, a path or a name."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const std::vector<Entity> entities,
                               entityListParameter(context, parameters, "entities"));
            CORDITE_TRY(EditorActions(context).deleteEntities(entities));
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "entity.duplicate",
        .description = "Copies entities, with everything below them, next to the originals. Returns the copies.",
        .parameters = {{"entities", Type::Array, "The entities to copy, each given as an id, a path or a name."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const std::vector<Entity> entities,
                               entityListParameter(context, parameters, "entities"));
            CORDITE_TRY_ASSIGN(const std::vector<Entity> copies, EditorActions(context).duplicateEntities(entities));
            return describeEntities(context, copies);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "entity.rename",
        .description = "Changes the name of an entity.",
        .parameters = {{"entity", Type::String, kEntityReference},
                       {"name", Type::String, "The new name. Must not be empty or contain '/'."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            CORDITE_TRY(EditorActions(context).renameEntity(entity, parameters.at("name").get<std::string>()));
            return describeEntity(context, entity);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "entity.setEnabled",
        .description = "Enables or disables an entity. A disabled entity, and everything below it, is ignored.",
        .parameters = {{"entity", Type::String, kEntityReference}, {"enabled", Type::Boolean, "The new state."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            CORDITE_TRY(EditorActions(context).setEntityEnabled(entity, parameters.at("enabled").get<bool>()));
            return describeEntity(context, entity);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "entity.setParent",
        .description = "Moves an entity under another one, or makes it a root.",
        .parameters =
            {{"entity", Type::String, kEntityReference},
             {"parent", Type::Any, "The new parent, or null to make the entity a root."},
             {"index", Type::Integer, "Its position among the new siblings. Defaults to last.", false},
             {"keepWorldTransform", Type::Boolean,
              "Keep the entity where it is in the world (the default) rather than keeping its local transform.",
              false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));

            Entity parent;
            const Json& parentReference = parameters.at("parent");
            if (parentReference.is_string()) {
                CORDITE_TRY_ASSIGN(parent, entityParameter(context, parameters, "parent"));
            } else if (!parentReference.is_null()) {
                return makeError(ErrorCode::InvalidArgument, "'parent' must be an entity reference or null, found {}",
                                 jsonTypeName(parentReference));
            }

            std::optional<size_t> index;
            if (const auto given = parameters.find("index"); given != parameters.end() && !given->is_null()) {
                const double value = given->get<double>();
                if (value < 0.0) {
                    return makeError(ErrorCode::InvalidArgument, "'index' must not be negative, got {}", value);
                }
                index = static_cast<size_t>(value);
            }

            const bool keepWorldTransform = jsonGetOr<bool>(parameters, "keepWorldTransform", true);
            CORDITE_TRY(EditorActions(context).setParent(entity, parent, index, keepWorldTransform));
            return describeEntity(context, entity);
        },
    }));

    return commands.add({
        .name = "entity.get",
        .description = "Returns everything about an entity: its state, parent, children and components with all "
                       "their properties.",
        .parameters = {{"entity", Type::String, kEntityReference}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            const World& world = context.world();

            Json description = describeEntity(context, entity);
            description["enabled"] = world.isEnabled(entity);
            const Entity parent = world.parent(entity);
            description["parent"] = parent.isNull() ? Json(nullptr) : describeEntity(context, parent);
            description["children"] = describeEntities(context, world.children(entity));

            Json components = Json::object();
            for (const ComponentDescriptor* descriptor : context.components().components()) {
                if (const void* instance = descriptor->getConst(world.registry(), entity.handle)) {
                    components[descriptor->type.name()] = descriptor->type.toJson(instance);
                }
            }
            description["components"] = std::move(components);
            return description;
        },
    });
}

Status registerComponentCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "component.types",
        .description = "Describes every component type: what it is for and the name, type, range and meaning of "
                       "each of its properties.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            Json types = Json::array();
            for (const ComponentDescriptor* descriptor : context.components().components()) {
                Json type = descriptor->type.describe();
                type["description"] = descriptor->traits.description;
                // Whether the component can be added and removed, or is always there.
                type["optional"] = descriptor->traits.optional;
                types.push_back(std::move(type));
            }
            return types;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "component.add",
        .description = "Gives an entity a component it does not have yet.",
        .parameters = {{"entity", Type::String, kEntityReference},
                       {"component", Type::String, "The name of the component type, e.g. \"Camera\"."},
                       {"properties", Type::Object, "Properties to set; the others get their defaults.", false}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            const auto properties = parameters.find("properties");
            const Json none = Json::object();
            CORDITE_TRY(EditorActions(context).addComponent(
                entity, parameters.at("component").get<std::string>(),
                properties != parameters.end() && !properties->is_null() ? *properties : none));
            return describeEntity(context, entity);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "component.remove",
        .description = "Takes a component away from an entity.",
        .parameters = {{"entity", Type::String, kEntityReference},
                       {"component", Type::String, "The name of the component type."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            CORDITE_TRY(EditorActions(context).removeComponent(entity, parameters.at("component").get<std::string>()));
            return describeEntity(context, entity);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "component.set",
        .description = "Changes properties of a component. Properties that are not mentioned keep their values. "
                       "Returns the component's properties after the change.",
        .parameters = {{"entity", Type::String, kEntityReference},
                       {"component", Type::String, "The name of the component type."},
                       {"properties", Type::Object, "The properties to change and their new values."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, componentParameter(context, parameters));
            CORDITE_TRY(EditorActions(context).setComponentProperties(entity, descriptor->type.name(),
                                                                      parameters.at("properties")));
            return descriptor->type.toJson(descriptor->getConst(context.world().registry(), entity.handle));
        },
    }));

    return commands.add({
        .name = "component.get",
        .description = "Returns the properties of one component of an entity.",
        .parameters = {{"entity", Type::String, kEntityReference},
                       {"component", Type::String, "The name of the component type."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const Entity entity, entityParameter(context, parameters, "entity"));
            CORDITE_TRY_ASSIGN(const ComponentDescriptor* descriptor, componentParameter(context, parameters));
            const void* instance = descriptor->getConst(context.world().registry(), entity.handle);
            if (instance == nullptr) {
                return makeError(ErrorCode::NotFound, "'{}' has no {} component", context.world().name(entity),
                                 descriptor->type.name());
            }
            return descriptor->type.toJson(instance);
        },
    });
}

Status registerSessionCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(commands.add({
        .name = "editor.commands",
        .description = "Lists every command with its description and parameters.",
        .parameters = {},
        .handler = [&commands](const Json& /*parameters*/) -> Result<Json> { return commands.describe(); },
    }));

    CORDITE_TRY(commands.add({
        .name = "editor.state",
        .description = "Returns what the editor is working on: the scene and its file, whether there are unsaved "
                       "changes, what can be undone and redone, and the selection.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            Json state = Json::object();
            state["scene"] = describeScene(context);
            state["history"] = describeHistory(context);
            state["selection"] = describeEntities(context, context.selection());
            return state;
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "editor.undo",
        .description = "Takes back the most recent edit. Returns what can be undone and redone afterwards.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(context.history().undo());
            return describeHistory(context);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "editor.redo",
        .description = "Makes the most recently undone edit again.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(context.history().redo());
            return describeHistory(context);
        },
    }));

    CORDITE_TRY(commands.add({
        .name = "selection.set",
        .description = "Selects exactly the given entities; an empty list clears the selection. The entity given "
                       "last is the one the inspector shows.",
        .parameters = {{"entities", Type::Array, "The entities to select, each given as an id, a path or a name."}},
        .handler = [&context](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const std::vector<Entity> entities,
                               entityListParameter(context, parameters, "entities"));
            context.setSelection(entities);
            return describeEntities(context, context.selection());
        },
    }));

    return commands.add({
        .name = "selection.get",
        .description = "Returns the selected entities in the order they were selected.",
        .parameters = {},
        .handler = [&context](const Json& /*parameters*/) -> Result<Json> {
            return describeEntities(context, context.selection());
        },
    });
}

} // namespace

Status registerEditorCommands(CommandRegistry& commands, EditorContext& context) {
    CORDITE_TRY(registerSessionCommands(commands, context));
    CORDITE_TRY(registerSceneCommands(commands, context));
    CORDITE_TRY(registerEntityCommands(commands, context));
    CORDITE_TRY(registerComponentCommands(commands, context));
    return registerAssetCommands(commands, context);
}

} // namespace cordite
