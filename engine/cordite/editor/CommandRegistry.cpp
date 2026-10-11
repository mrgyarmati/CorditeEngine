#include "cordite/editor/CommandRegistry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// Names are lowercase words joined by dots; words may contain digits and inner capitals
/// ("entity.setParent"), and there must be at least a group and a verb.
bool isValidCommandName(std::string_view name) {
    if (name.empty() || name.find('.') == std::string_view::npos) {
        return false;
    }

    bool wordStart = true;
    for (const char character : name) {
        if (character == '.') {
            if (wordStart) {
                return false;
            }
            wordStart = true;
            continue;
        }
        const bool isLower = character >= 'a' && character <= 'z';
        const bool isUpper = character >= 'A' && character <= 'Z';
        const bool isDigit = character >= '0' && character <= '9';
        // A word starts with a lowercase letter and continues with letters and digits.
        const bool allowed = isLower || (!wordStart && (isUpper || isDigit));
        if (!allowed) {
            return false;
        }
        wordStart = false;
    }
    return !wordStart;
}

bool matchesType(const Json& value, CommandParameterType type) {
    switch (type) {
        case CommandParameterType::String:
            return value.is_string();
        case CommandParameterType::Number:
            return value.is_number();
        case CommandParameterType::Integer:
            // JSON has one number type: 2.0 is as whole as 2.
            return value.is_number_integer() ||
                   (value.is_number_float() && std::trunc(value.get<double>()) == value.get<double>());
        case CommandParameterType::Boolean:
            return value.is_boolean();
        case CommandParameterType::Object:
            return value.is_object();
        case CommandParameterType::Array:
            return value.is_array();
        case CommandParameterType::Any:
            return true;
    }
    return false;
}

Status validateParameters(const CommandDesc& command, const Json& parameters) {
    if (!parameters.is_object()) {
        return makeError(ErrorCode::InvalidArgument, "parameters must be a JSON object, found {}",
                         jsonTypeName(parameters));
    }

    for (const auto& [key, value] : parameters.items()) {
        const auto declared = std::ranges::find(command.parameters, key, &CommandParameter::name);
        if (declared == command.parameters.end()) {
            return makeError(ErrorCode::InvalidArgument, "unknown parameter '{}'", key);
        }
        // An optional parameter given as null counts as left out.
        if (value.is_null() && !declared->required) {
            continue;
        }
        if (!matchesType(value, declared->type)) {
            return makeError(ErrorCode::InvalidArgument, "parameter '{}' must be of type {}, found {}", key,
                             nameOf(declared->type), jsonTypeName(value));
        }
    }

    for (const CommandParameter& parameter : command.parameters) {
        if (parameter.required && !parameters.contains(parameter.name)) {
            return makeError(ErrorCode::InvalidArgument, "missing parameter '{}'", parameter.name);
        }
    }
    return {};
}

} // namespace

std::string_view nameOf(CommandParameterType type) {
    switch (type) {
        case CommandParameterType::String:
            return "string";
        case CommandParameterType::Number:
            return "number";
        case CommandParameterType::Integer:
            return "integer";
        case CommandParameterType::Boolean:
            return "boolean";
        case CommandParameterType::Object:
            return "object";
        case CommandParameterType::Array:
            return "array";
        case CommandParameterType::Any:
            return "any";
    }
    return "unknown";
}

Status CommandRegistry::add(CommandDesc desc) {
    if (!isValidCommandName(desc.name)) {
        return makeError(ErrorCode::InvalidArgument,
                         "'{}' is not a valid command name; use dotted lowercase words such as 'entity.create'",
                         desc.name);
    }
    if (desc.handler == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "command '{}' has no handler", desc.name);
    }
    for (size_t index = 0; index < desc.parameters.size(); ++index) {
        const std::string& parameterName = desc.parameters[index].name;
        const auto earlier =
            std::ranges::find(desc.parameters.begin(), desc.parameters.begin() + static_cast<std::ptrdiff_t>(index),
                              parameterName, &CommandParameter::name);
        if (parameterName.empty() || earlier != desc.parameters.begin() + static_cast<std::ptrdiff_t>(index)) {
            return makeError(ErrorCode::InvalidArgument, "command '{}' declares the parameter '{}' twice or unnamed",
                             desc.name, parameterName);
        }
    }

    const auto position = std::ranges::lower_bound(m_commands, desc.name, {}, &CommandDesc::name);
    if (position != m_commands.end() && position->name == desc.name) {
        return makeError(ErrorCode::AlreadyExists, "a command named '{}' is already registered", desc.name);
    }
    m_commands.insert(position, std::move(desc));
    return {};
}

const CommandDesc* CommandRegistry::find(std::string_view name) const {
    const auto position = std::ranges::lower_bound(m_commands, name, {}, &CommandDesc::name);
    return position != m_commands.end() && position->name == name ? &*position : nullptr;
}

Result<Json> CommandRegistry::execute(std::string_view name, const Json& parameters) const {
    const CommandDesc* command = find(name);
    if (command == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no command named '{}'", name);
    }

    const Json empty = Json::object();
    const Json& effective = parameters.is_null() ? empty : parameters;
    if (const Status valid = validateParameters(*command, effective); !valid) {
        return tl::unexpected(valid.error().withContext(command->name));
    }

    Result<Json> result = command->handler(effective);
    if (!result) {
        return tl::unexpected(result.error().withContext(command->name));
    }
    return result;
}

Json CommandRegistry::describe() const {
    Json catalogue = Json::array();
    for (const CommandDesc& command : m_commands) {
        Json parameters = Json::array();
        for (const CommandParameter& parameter : command.parameters) {
            Json entry = Json::object();
            entry["name"] = parameter.name;
            entry["type"] = nameOf(parameter.type);
            entry["required"] = parameter.required;
            entry["description"] = parameter.description;
            parameters.push_back(std::move(entry));
        }

        Json entry = Json::object();
        entry["name"] = command.name;
        entry["description"] = command.description;
        entry["parameters"] = std::move(parameters);
        catalogue.push_back(std::move(entry));
    }
    return catalogue;
}

} // namespace cordite
