#pragma once

/// @file
/// Editor operations by name: the editor's automation interface.
///
/// Everything a person can do in the editor is also a command: a name, a JSON object of
/// parameters, and a JSON result. Menus, scripts and AI agents all go through the same
/// registry, and the registry can describe itself, so a tool can discover what exists
/// and how to call it.
///
/// @code
/// CommandRegistry commands;
/// CORDITE_TRY(commands.add({
///     .name = "entity.rename",
///     .description = "Changes the name of an entity.",
///     .parameters = {{"entity", CommandParameterType::String, "The entity, by id, path or name."},
///                    {"name", CommandParameterType::String, "The new name."}},
///     .handler = [&](const Json& parameters) -> Result<Json> { ... },
/// }));
///
/// CORDITE_TRY_ASSIGN(const Json result, commands.execute("entity.rename", {{"entity", "Lamp"}, {"name", "Lantern"}}));
/// const Json catalogue = commands.describe();     // what an agent reads first
/// @endcode
///
/// Parameters are checked before a handler runs: required ones must be present, every
/// one must have the declared JSON type, and names the command does not declare are
/// rejected, so a misspelled parameter is an error instead of being silently ignored.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

enum class CommandParameterType : uint8_t {
    String,
    /// Any JSON number.
    Number,
    /// A whole number.
    Integer,
    Boolean,
    Object,
    Array,
    /// Any JSON value, including null. The handler decides what is acceptable.
    Any,
};

std::string_view nameOf(CommandParameterType type);

struct CommandParameter {
    std::string name;
    CommandParameterType type = CommandParameterType::String;
    /// One sentence for the catalogue.
    std::string description;
    bool required = true;
};

struct CommandDesc {
    /// Lowercase words joined by dots, group first: "entity.create".
    std::string name;
    /// What the command does and returns, in a sentence or two.
    std::string description;
    std::vector<CommandParameter> parameters;
    /// Receives the validated parameters. Returns the result, or null when there is none.
    std::function<Result<Json>(const Json& parameters)> handler;
};

class CommandRegistry {
public:
    /// Registers a command. Fails with InvalidArgument for a malformed name, a missing
    /// handler or a parameter declared twice, and with AlreadyExists for a taken name.
    [[nodiscard]] Status add(CommandDesc desc);

    /// Runs a command. `parameters` must be a JSON object, or null for none. Fails with
    /// NotFound for an unknown command and InvalidArgument for parameters that do not
    /// match its declaration; otherwise returns what the handler returns.
    [[nodiscard]] Result<Json> execute(std::string_view name, const Json& parameters = nullptr) const;

    /// Returns the command with the given name, or nullptr.
    [[nodiscard]] const CommandDesc* find(std::string_view name) const;

    /// Every command, sorted by name.
    [[nodiscard]] std::span<const CommandDesc> commands() const { return m_commands; }

    /// The catalogue: an array with the name, description and parameters of every command.
    [[nodiscard]] Json describe() const;

private:
    std::vector<CommandDesc> m_commands;
};

} // namespace cordite
