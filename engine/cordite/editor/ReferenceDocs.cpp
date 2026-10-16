#include "cordite/editor/ReferenceDocs.h"

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/editor/CommandRegistry.h"
#include "cordite/platform/InputMap.h"
#include "cordite/platform/KeyCodes.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace cordite {
namespace {

/// Text as a cell of a Markdown table: on one line, and with nothing that ends the cell.
std::string cell(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        if (character == '\n' || character == '\r') {
            out += ' ';
        } else if (character == '|') {
            out += "\\|";
        } else {
            out += character;
        }
    }
    return out;
}

/// What kind of value a property holds, as a person reads it: "number", "color", "asset
/// (texture)", "list of numbers".
std::string kindOf(const Json& property) {
    const auto semantic = jsonGetOr<std::string>(property, "semantic", "");
    if (semantic == "color") {
        return "color";
    }
    if (semantic == "entity") {
        return "entity";
    }
    if (semantic == "asset") {
        const auto type = jsonGetOr<std::string>(property, "assetType", "");
        return type.empty() ? std::string("asset") : fmt::format("asset ({})", type);
    }
    if (semantic == "bitMask") {
        return "bits";
    }
    auto type = jsonGetOr<std::string>(property, "type", "");
    if (type == "array") {
        return fmt::format("list of {}", jsonGetOr<std::string>(property, "elementType", "values"));
    }
    if (type == "object") {
        return jsonGetOr<std::string>(property, "objectType", "object");
    }
    return type;
}

/// The values a property takes: its options, or its range and unit.
std::string valuesOf(const Json& property) {
    std::vector<std::string> parts;
    if (const auto options = property.find("options"); options != property.end() && options->is_array()) {
        std::vector<std::string> names;
        for (const Json& option : *options) {
            names.push_back(fmt::format("`{}`", option.is_string() ? option.get<std::string>()
                                                                   : jsonGetOr<std::string>(option, "name", "")));
        }
        parts.push_back(fmt::format("{}", fmt::join(names, ", ")));
    }
    const auto minimum = property.find("minimum");
    const auto maximum = property.find("maximum");
    const bool hasMinimum = minimum != property.end();
    const bool hasMaximum = maximum != property.end();
    if (hasMinimum && hasMaximum) {
        parts.push_back(fmt::format("{} to {}", minimum->dump(), maximum->dump()));
    } else if (hasMinimum) {
        parts.push_back(fmt::format("at least {}", minimum->dump()));
    } else if (hasMaximum) {
        parts.push_back(fmt::format("at most {}", maximum->dump()));
    }
    if (const auto unit = jsonGetOr<std::string>(property, "unit", ""); !unit.empty()) {
        parts.push_back(unit);
    }
    return fmt::format("{}", fmt::join(parts, "; "));
}

} // namespace

std::string componentReference(const ComponentRegistry& components) {
    std::vector<const ComponentDescriptor*> sorted(components.components().begin(), components.components().end());
    std::ranges::sort(sorted, [](const ComponentDescriptor* left, const ComponentDescriptor* right) {
        return left->type.name() < right->type.name();
    });
    // Defaults are what a component is when it is added: one of each, on an entity of a
    // world of its own.
    World world;
    const Entity entity = world.createEntity("Defaults");

    std::string page = "# Components\n\n"
                       "Every component an entity can have, with every property: the kind of value it holds, its "
                       "default, the values it takes and what it means. Written by the engine "
                       "(`componentReference`, editor/ReferenceDocs.h) and held to it by a test: do not change it "
                       "by hand. Properties marked *not saved* are what a running game says, and are not in "
                       "scene files; *read only* ones are saved, but cannot be edited.\n";
    for (const ComponentDescriptor* descriptor : sorted) {
        const void* defaults = descriptor->add(world.registry(), entity.handle);
        page += fmt::format("\n## {}\n\n", descriptor->type.name());
        if (!descriptor->traits.description.empty()) {
            page += fmt::format("{}\n\n", descriptor->traits.description);
        }
        if (descriptor->type.properties().empty()) {
            page += "It has no properties.\n";
            continue;
        }
        page += "| Property | Kind | Default | Values | Meaning |\n|---|---|---|---|---|\n";
        for (const PropertyInfo& property : descriptor->type.properties()) {
            const Json description = property.describe();
            auto meaning = jsonGetOr<std::string>(description, "description", "");
            if (jsonGetOr<bool>(description, "transient", false)) {
                meaning.insert(0, "*Not saved.* ");
            }
            if (jsonGetOr<bool>(description, "readOnly", false)) {
                meaning.insert(0, "*Read only.* ");
            }
            page += fmt::format("| `{}` | {} | `{}` | {} | {} |\n", property.name(), cell(kindOf(description)),
                                cell(property.get(defaults).dump()), cell(valuesOf(description)), cell(meaning));
        }
    }
    return page;
}

std::string scriptReference(const ScriptSystem& scripts) {
    std::string page = "# Scripting API\n\n"
                       "Everything gameplay scripts are written against: the functions the engine calls in a "
                       "script, and every function scripts can call, library by library. Written by the engine "
                       "(`scriptReference`, editor/ReferenceDocs.h) from the catalogue scripts are given, and held "
                       "to it by a test: do not change it by hand. How scripts are written is in the "
                       "[guide](../guide/scripting.md).\n\n"
                       "## Functions the engine calls\n\n"
                       "| Function | When |\n|---|---|\n";
    for (const ScriptCallbackInfo& callback : scripts.callbacks()) {
        page += fmt::format("| `{}` | {} |\n", cell(callback.signature), cell(callback.description));
    }

    // A library is what a function's name says before its own: "cordite.world" of
    // "cordite.world.find". Libraries come in the order the catalogue first names them.
    const std::vector<ScriptFunctionInfo> functions = scripts.runtime().functions();
    std::vector<std::string> libraries;
    for (const ScriptFunctionInfo& function : functions) {
        const size_t dot = function.name.rfind('.');
        const std::string library = dot == std::string::npos ? std::string() : function.name.substr(0, dot);
        if (std::ranges::find(libraries, library) == libraries.end()) {
            libraries.push_back(library);
        }
    }
    for (const std::string& library : libraries) {
        page += fmt::format("\n## {}\n\n| Function | What it does |\n|---|---|\n",
                            library.empty() ? std::string("Everywhere") : library);
        for (const ScriptFunctionInfo& function : functions) {
            const size_t dot = function.name.rfind('.');
            if ((dot == std::string::npos ? std::string() : function.name.substr(0, dot)) == library) {
                page += fmt::format("| `{}` | {} |\n", cell(function.signature), cell(function.description));
            }
        }
    }
    return page;
}

std::string commandReference(const CommandRegistry& commands) {
    std::vector<const CommandDesc*> sorted;
    sorted.reserve(commands.commands().size());
    for (const CommandDesc& command : commands.commands()) {
        sorted.push_back(&command);
    }
    std::ranges::sort(sorted,
                      [](const CommandDesc* left, const CommandDesc* right) { return left->name < right->name; });

    std::string page = "# Editor commands\n\n"
                       "Every command of the editor: what the menus, the toolbar and the Inspector do, by name, with "
                       "JSON parameters and a JSON result - typed in the Console, run from a script of commands "
                       "(`--exec`), or by an agent. Written by the engine (`commandReference`, "
                       "editor/ReferenceDocs.h) and held to it by a test: do not change it by hand. How to run "
                       "them is in the [guide](../guide/automation.md).\n";
    for (const CommandDesc* command : sorted) {
        page += fmt::format("\n## {}\n\n{}\n", command->name, command->description);
        if (command->parameters.empty()) {
            page += "\nIt takes no parameters.\n";
            continue;
        }
        page += "\n| Parameter | Kind | Needed | Meaning |\n|---|---|---|---|\n";
        for (const CommandParameter& parameter : command->parameters) {
            page += fmt::format("| `{}` | {} | {} | {} |\n", parameter.name, nameOf(parameter.type),
                                parameter.required ? "yes" : "no", cell(parameter.description));
        }
    }
    return page;
}

std::string inputReference() {
    const auto list = [](auto count, auto nameOfValue) {
        std::vector<std::string> names;
        for (size_t index = 0; index < static_cast<size_t>(count); ++index) {
            const std::string_view name = nameOfValue(index);
            // What no control is called: a key the system has no name for.
            if (!name.empty() && name != "Unknown") {
                names.push_back(fmt::format("`{}`", name));
            }
        }
        return fmt::format("{}\n", fmt::join(names, ", "));
    };
    std::string page = "# Input names\n\n"
                       "The names an input map binds controls by. Written by the engine (`inputReference`, "
                       "editor/ReferenceDocs.h) and held to it by a test: do not change it by hand. How an input "
                       "map is written is in the [guide](../guide/input.md).\n\n"
                       "## Keys\n\n"
                       "For `key`, `positiveKey` and `negativeKey`. Keys are named by where they are on a US "
                       "keyboard, whatever the layout of the keyboard says on them.\n\n";
    page += list(Key::Count, [](size_t index) { return nameOf(static_cast<Key>(index)); });
    page += "\n## Mouse buttons\n\nFor `mouseButton`.\n\n";
    page += list(MouseButton::Count, [](size_t index) { return nameOf(static_cast<MouseButton>(index)); });
    page += "\n## Gamepad buttons\n\nFor `gamepadButton`, `positiveButton` and `negativeButton`.\n\n";
    page += list(GamepadButton::Count, [](size_t index) { return nameOf(static_cast<GamepadButton>(index)); });
    page += "\n## Gamepad sticks and triggers\n\nFor `gamepadAxis`.\n\n";
    page += list(GamepadAxis::Count, [](size_t index) { return nameOf(static_cast<GamepadAxis>(index)); });
    page += "\n## The pointer and the wheel\n\nFor `mouse` and `scroll`: `x` and `y`.\n";
    return page;
}

} // namespace cordite
